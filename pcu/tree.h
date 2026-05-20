/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Read-Copy Update mechanism for mutual exclusion (tree-based version)
 * Internal non-public definitions.
 *
 * Copyright IBM Corporation, 2008
 *
 * Author: Ingo Molnar <mingo@elte.hu>
 *	   Paul E. McKenney <paulmck@linux.ibm.com>
 */

#include <linux/cache.h>
#include <linux/spinlock.h>
#include <linux/rtmutex.h>
#include <linux/threads.h>
#include <linux/cpumask.h>
#include <linux/seqlock.h>
#include <linux/swait.h>
#include <linux/pcu_node_tree.h>

#include "pcu_segcblist.h"

/* Communicate arguments to a workqueue handler. */
struct pcu_exp_work {
	unsigned long rew_s;
	struct work_struct rew_work;
};

/* PCU's kthread states for tracing. */
#define PCU_KTHREAD_STOPPED  0
#define PCU_KTHREAD_RUNNING  1
#define PCU_KTHREAD_WAITING  2
#define PCU_KTHREAD_OFFCPU   3
#define PCU_KTHREAD_YIELDING 4
#define PCU_KTHREAD_MAX      4

/*
 * Definition for node within the PCU grace-period-detection hierarchy.
 */
struct pcu_node {
	raw_spinlock_t __private lock;	/* Root pcu_node's lock protects */
					/*  some pcu_state fields as well as */
					/*  following. */
	unsigned long gp_seq;	/* Track rsp->gp_seq. */
	unsigned long gp_seq_needed; /* Track furthest future GP request. */
	unsigned long completedqs; /* All QSes done for this node. */
	unsigned long qsmask;	/* CPUs or groups that need to switch in */
				/*  order for current grace period to proceed.*/
				/*  In leaf pcu_node, each bit corresponds to */
				/*  an pcu_data structure, otherwise, each */
				/*  bit corresponds to a child pcu_node */
				/*  structure. */
	unsigned long pcu_gp_init_mask;	/* Mask of offline CPUs at GP init. */
	unsigned long qsmaskinit;
				/* Per-GP initial value for qsmask. */
				/*  Initialized from ->qsmaskinitnext at the */
				/*  beginning of each grace period. */
	unsigned long qsmaskinitnext;
	unsigned long ofl_seq;	/* CPU-hotplug operation sequence count. */
				/* Online CPUs for next grace period. */
	unsigned long expmask;	/* CPUs or groups that need to check in */
				/*  to allow the current expedited GP */
				/*  to complete. */
	unsigned long expmaskinit;
				/* Per-GP initial values for expmask. */
				/*  Initialized from ->expmaskinitnext at the */
				/*  beginning of each expedited GP. */
	unsigned long expmaskinitnext;
				/* Online CPUs for next expedited GP. */
				/*  Any CPU that has ever been online will */
				/*  have its bit set. */
	unsigned long cbovldmask;
				/* CPUs experiencing callback overload. */
	unsigned long ffmask;	/* Fully functional CPUs. */
	unsigned long grpmask;	/* Mask to apply to parent qsmask. */
				/*  Only one bit will be set in this mask. */
	int	grplo;		/* lowest-numbered CPU here. */
	int	grphi;		/* highest-numbered CPU here. */
	u8	grpnum;		/* group number for next level up. */
	u8	level;		/* root is at level 0. */
	bool	wait_blkd_tasks;/* Necessary to wait for blocked tasks to */
				/*  exit PCU read-side critical sections */
				/*  before propagating offline up the */
				/*  pcu_node tree? */
	struct pcu_node *parent;
	struct list_head blkd_tasks;
				/* Tasks blocked in PCU read-side critical */
				/*  section.  Tasks are placed at the head */
				/*  of this list and age towards the tail. */
	struct list_head *gp_tasks;
				/* Pointer to the first task blocking the */
				/*  current grace period, or NULL if there */
				/*  is no such task. */
	struct list_head *exp_tasks;
				/* Pointer to the first task blocking the */
				/*  current expedited grace period, or NULL */
				/*  if there is no such task.  If there */
				/*  is no current expedited grace period, */
				/*  then there can cannot be any such task. */
	struct list_head *boost_tasks;
				/* Pointer to first task that needs to be */
				/*  priority boosted, or NULL if no priority */
				/*  boosting is needed for this pcu_node */
				/*  structure.  If there are no tasks */
				/*  queued on this pcu_node structure that */
				/*  are blocking the current grace period, */
				/*  there can be no such task. */
	struct rt_mutex boost_mtx;
				/* Used only for the priority-boosting */
				/*  side effect, not as a lock. */
	unsigned long boost_time;
				/* When to start boosting (jiffies). */
	struct task_struct *boost_kthread_task;
				/* kthread that takes care of priority */
				/*  boosting for this pcu_node structure. */
	unsigned int boost_kthread_status;
				/* State of boost_kthread_task for tracing. */
	unsigned long n_boosts;	/* Number of boosts for this pcu_node structure. */
#ifdef CONFIG_RCU_NOCB_CPU
	struct swait_queue_head nocb_gp_wq[2];
				/* Place for pcu_nocb_kthread() to wait GP. */
#endif /* #ifdef CONFIG_RCU_NOCB_CPU */
	raw_spinlock_t fqslock ____cacheline_internodealigned_in_smp;

	spinlock_t exp_lock ____cacheline_internodealigned_in_smp;
	unsigned long exp_seq_rq;
	wait_queue_head_t exp_wq[4];
	struct pcu_exp_work rew;
	bool exp_need_flush;	/* Need to flush workitem? */
} ____cacheline_internodealigned_in_smp;

/*
 * Bitmasks in an pcu_node cover the interval [grplo, grphi] of CPU IDs, and
 * are indexed relative to this interval rather than the global CPU ID space.
 * This generates the bit for a CPU in node-local masks.
 */
#define leaf_node_cpu_bit(rnp, cpu) (BIT((cpu) - (rnp)->grplo))

/*
 * Union to allow "aggregate OR" operation on the need for a quiescent
 * state by the normal and expedited grace periods.
 */
union pcu_noqs {
	struct {
		u8 norm;
		u8 exp;
	} b; /* Bits. */
	u16 s; /* Set of bits, aggregate OR here. */
};

/* Per-CPU data for read-copy update. */
struct pcu_data {
	/* 1) quiescent-state and grace-period handling : */
	unsigned long	gp_seq;		/* Track rsp->gp_seq counter. */
	unsigned long	gp_seq_needed;	/* Track furthest future GP request. */
	union pcu_noqs	cpu_no_qs;	/* No QSes yet for this CPU. */
	bool		core_needs_qs;	/* Core waits for quiesc state. */
	bool		beenonline;	/* CPU online at least once. */
	bool		gpwrap;		/* Possible ->gp_seq wrap. */
	bool		exp_deferred_qs; /* This CPU awaiting a deferred QS? */
	bool		cpu_started;	/* PCU watching this onlining CPU. */
	struct pcu_node *mynode;	/* This CPU's leaf of hierarchy */
	unsigned long grpmask;		/* Mask to apply to leaf qsmask. */
	unsigned long	ticks_this_gp;	/* The number of scheduling-clock */
					/*  ticks this CPU has handled */
					/*  during and after the last grace */
					/* period it is aware of. */
	struct irq_work defer_qs_iw;	/* Obtain later scheduler attention. */
	bool defer_qs_iw_pending;	/* Scheduler attention pending? */
	struct work_struct strict_work;	/* Schedule readers for strict GPs. */

	/* 2) batch handling */
	struct pcu_segcblist cblist;	/* Segmented callback list, with */
					/* different callbacks waiting for */
					/* different grace periods. */
	long		qlen_last_fqs_check;
					/* qlen at last check for QS forcing */
	unsigned long	n_cbs_invoked;	/* # callbacks invoked since boot. */
	unsigned long	n_force_qs_snap;
					/* did other CPU force QS recently? */
	long		blimit;		/* Upper limit on a processed batch */

	/* 3) dynticks interface. */
	int dynticks_snap;		/* Per-GP tracking for dynticks. */
	long dynticks_nesting;		/* Track process nesting level. */
	long dynticks_nmi_nesting;	/* Track irq/NMI nesting level. */
	atomic_t dynticks;		/* Even value for idle, else odd. */
	bool pcu_need_heavy_qs;		/* GP old, so heavy quiescent state! */
	bool pcu_urgent_qs;		/* GP old need light quiescent state. */
	bool pcu_forced_tick;		/* Forced tick to provide QS. */
	bool pcu_forced_tick_exp;	/*   ... provide QS to expedited GP. */
#ifdef CONFIG_RCU_FAST_NO_HZ
	unsigned long last_accelerate;	/* Last jiffy CBs were accelerated. */
	unsigned long last_advance_all;	/* Last jiffy CBs were all advanced. */
	int tick_nohz_enabled_snap;	/* Previously seen value from sysfs. */
#endif /* #ifdef CONFIG_RCU_FAST_NO_HZ */

	/* 4) pcu_barrier(), OOM callbacks, and expediting. */
	struct pcu_head barrier_head;
	int exp_dynticks_snap;		/* Double-check need for IPI. */

	/* 5) Callback offloading. */
#ifdef CONFIG_RCU_NOCB_CPU
	struct swait_queue_head nocb_cb_wq; /* For nocb kthreads to sleep on. */
	struct swait_queue_head nocb_state_wq; /* For offloading state changes */
	struct task_struct *nocb_gp_kthread;
	raw_spinlock_t nocb_lock;	/* Guard following pair of fields. */
	atomic_t nocb_lock_contended;	/* Contention experienced. */
	int nocb_defer_wakeup;		/* Defer wakeup of nocb_kthread. */
	struct timer_list nocb_timer;	/* Enforce finite deferral. */
	unsigned long nocb_gp_adv_time;	/* Last call_pcu() CB adv (jiffies). */

	/* The following fields are used by call_pcu, hence own cacheline. */
	raw_spinlock_t nocb_bypass_lock ____cacheline_internodealigned_in_smp;
	struct pcu_cblist nocb_bypass;	/* Lock-contention-bypass CB list. */
	unsigned long nocb_bypass_first; /* Time (jiffies) of first enqueue. */
	unsigned long nocb_nobypass_last; /* Last ->cblist enqueue (jiffies). */
	int nocb_nobypass_count;	/* # ->cblist enqueues at ^^^ time. */

	/* The following fields are used by GP kthread, hence own cacheline. */
	raw_spinlock_t nocb_gp_lock ____cacheline_internodealigned_in_smp;
	u8 nocb_gp_sleep;		/* Is the nocb GP thread asleep? */
	u8 nocb_gp_bypass;		/* Found a bypass on last scan? */
	u8 nocb_gp_gp;			/* GP to wait for on last scan? */
	unsigned long nocb_gp_seq;	/*  If so, ->gp_seq to wait for. */
	unsigned long nocb_gp_loops;	/* # passes through wait code. */
	struct swait_queue_head nocb_gp_wq; /* For nocb kthreads to sleep on. */
	bool nocb_cb_sleep;		/* Is the nocb CB thread asleep? */
	struct task_struct *nocb_cb_kthread;
	struct pcu_data *nocb_next_cb_rdp;
					/* Next pcu_data in wakeup chain. */

	/* The following fields are used by CB kthread, hence new cacheline. */
	struct pcu_data *nocb_gp_rdp ____cacheline_internodealigned_in_smp;
					/* GP rdp takes GP-end wakeups. */
#endif /* #ifdef CONFIG_RCU_NOCB_CPU */

	/* 6) PCU priority boosting. */
	struct task_struct *pcu_cpu_kthread_task;
					/* pcuc per-CPU kthread or NULL. */
	unsigned int pcu_cpu_kthread_status;
	char pcu_cpu_has_work;

	/* 7) Diagnostic data, including PCU CPU stall warnings. */
	unsigned int softirq_snap;	/* Snapshot of softirq activity. */
	/* ->pcu_iw* fields protected by leaf pcu_node ->lock. */
	struct irq_work pcu_iw;		/* Check for non-irq activity. */
	bool pcu_iw_pending;		/* Is ->pcu_iw pending? */
	unsigned long pcu_iw_gp_seq;	/* ->gp_seq associated with ->pcu_iw. */
	unsigned long pcu_ofl_gp_seq;	/* ->gp_seq at last offline. */
	short pcu_ofl_gp_flags;		/* ->gp_flags at last offline. */
	unsigned long pcu_onl_gp_seq;	/* ->gp_seq at last online. */
	short pcu_onl_gp_flags;		/* ->gp_flags at last online. */
	unsigned long last_fqs_resched;	/* Time of last pcu_resched(). */

	int cpu;
};

/* Values for nocb_defer_wakeup field in struct pcu_data. */
#define PCU_NOCB_WAKE_NOT	0
#define PCU_NOCB_WAKE_BYPASS	1
#define PCU_NOCB_WAKE		2
#define PCU_NOCB_WAKE_FORCE	3

#define PCU_JIFFIES_TILL_FORCE_QS (1 + (HZ > 250) + (HZ > 500))
					/* For jiffies_till_first_fqs and */
					/*  and jiffies_till_next_fqs. */

#define PCU_JIFFIES_FQS_DIV	256	/* Very large systems need more */
					/*  delay between bouts of */
					/*  quiescent-state forcing. */

#define PCU_STALL_RAT_DELAY	2	/* Allow other CPUs time to take */
					/*  at least one scheduling clock */
					/*  irq before ratting on them. */

#define pcu_wait(cond)							\
do {									\
	for (;;) {							\
		set_current_state(TASK_INTERRUPTIBLE);			\
		if (cond)						\
			break;						\
		schedule();						\
	}								\
	__set_current_state(TASK_RUNNING);				\
} while (0)

/*
 * PCU global state, including node hierarchy.  This hierarchy is
 * represented in "heap" form in a dense array.  The root (first level)
 * of the hierarchy is in ->node[0] (referenced by ->level[0]), the second
 * level in ->node[1] through ->node[m] (->node[1] referenced by ->level[1]),
 * and the third level in ->node[m+1] and following (->node[m+1] referenced
 * by ->level[2]).  The number of levels is determined by the number of
 * CPUs and by CONFIG_RCU_FANOUT.  Small systems will have a "hierarchy"
 * consisting of a single pcu_node.
 */
struct pcu_state {
	struct pcu_node node[NUM_PCU_NODES];	/* Hierarchy. */
	struct pcu_node *level[PCU_NUM_LVLS + 1];
						/* Hierarchy levels (+1 to */
						/*  shut bogus gcc warning) */
	int ncpus;				/* # CPUs seen so far. */
	int n_online_cpus;			/* # CPUs online for PCU. */

	/* The following fields are guarded by the root pcu_node's lock. */

	u8	boost ____cacheline_internodealigned_in_smp;
						/* Subject to priority boost. */
	unsigned long gp_seq;			/* Grace-period sequence #. */
	unsigned long gp_max;			/* Maximum GP duration in */
						/*  jiffies. */
	struct task_struct *gp_kthread;		/* Task for grace periods. */
	struct swait_queue_head gp_wq;		/* Where GP task waits. */
	short gp_flags;				/* Commands for GP task. */
	short gp_state;				/* GP kthread sleep state. */
	unsigned long gp_wake_time;		/* Last GP kthread wake. */
	unsigned long gp_wake_seq;		/* ->gp_seq at ^^^. */

	/* End of fields guarded by root pcu_node's lock. */

	struct mutex barrier_mutex;		/* Guards barrier fields. */
	atomic_t barrier_cpu_count;		/* # CPUs waiting on. */
	struct completion barrier_completion;	/* Wake at barrier end. */
	unsigned long barrier_sequence;		/* ++ at start and end of */
						/*  pcu_barrier(). */
	/* End of fields guarded by barrier_mutex. */

	struct mutex exp_mutex;			/* Serialize expedited GP. */
	struct mutex exp_wake_mutex;		/* Serialize wakeup. */
	unsigned long expedited_sequence;	/* Take a ticket. */
	atomic_t expedited_need_qs;		/* # CPUs left to check in. */
	struct swait_queue_head expedited_wq;	/* Wait for check-ins. */
	int ncpus_snap;				/* # CPUs seen last time. */
	u8 cbovld;				/* Callback overload now? */
	u8 cbovldnext;				/* ^        ^  next time? */

	unsigned long jiffies_force_qs;		/* Time at which to invoke */
						/*  force_quiescent_state(). */
	unsigned long jiffies_kick_kthreads;	/* Time at which to kick */
						/*  kthreads, if configured. */
	unsigned long n_force_qs;		/* Number of calls to */
						/*  force_quiescent_state(). */
	unsigned long gp_start;			/* Time at which GP started, */
						/*  but in jiffies. */
	unsigned long gp_end;			/* Time last GP ended, again */
						/*  in jiffies. */
	unsigned long gp_activity;		/* Time of last GP kthread */
						/*  activity in jiffies. */
	unsigned long gp_req_activity;		/* Time of last GP request */
						/*  in jiffies. */
	unsigned long jiffies_stall;		/* Time at which to check */
						/*  for CPU stalls. */
	unsigned long jiffies_resched;		/* Time at which to resched */
						/*  a reluctant CPU. */
	unsigned long n_force_qs_gpstart;	/* Snapshot of n_force_qs at */
						/*  GP start. */
	const char *name;			/* Name of structure. */
	char abbr;				/* Abbreviated name. */

	raw_spinlock_t ofl_lock ____cacheline_internodealigned_in_smp;
						/* Synchronize offline with */
						/*  GP pre-initialization. */
};

/* Values for pcu_state structure's gp_flags field. */
#define PCU_GP_FLAG_INIT 0x1	/* Need grace-period initialization. */
#define PCU_GP_FLAG_FQS  0x2	/* Need grace-period quiescent-state forcing. */
#define PCU_GP_FLAG_OVLD 0x4	/* Experiencing callback overload. */

/* Values for pcu_state structure's gp_state field. */
#define PCU_GP_IDLE	 0	/* Initial state and no GP in progress. */
#define PCU_GP_WAIT_GPS  1	/* Wait for grace-period start. */
#define PCU_GP_DONE_GPS  2	/* Wait done for grace-period start. */
#define PCU_GP_ONOFF     3	/* Grace-period initialization hotplug. */
#define PCU_GP_INIT      4	/* Grace-period initialization. */
#define PCU_GP_WAIT_FQS  5	/* Wait for force-quiescent-state time. */
#define PCU_GP_DOING_FQS 6	/* Wait done for force-quiescent-state time. */
#define PCU_GP_CLEANUP   7	/* Grace-period cleanup started. */
#define PCU_GP_CLEANED   8	/* Grace-period cleanup complete. */

/*
 * In order to export the pcu_state name to the tracing tools, it
 * needs to be added in the __tracepoint_string section.
 * This requires defining a separate variable tp_<sname>_varname
 * that points to the string being used, and this will allow
 * the tracing userspace tools to be able to decipher the string
 * address to the matching string.
 */
#ifdef CONFIG_PREEMPT_PCU
#define PCU_ABBR 'p'
#define PCU_NAME_RAW "pcu_preempt"
#else /* #ifdef CONFIG_PREEMPT_PCU */
#define PCU_ABBR 's'
#define PCU_NAME_RAW "pcu_sched"
#endif /* #else #ifdef CONFIG_PREEMPT_PCU */
#ifndef CONFIG_TRACING
#define PCU_NAME PCU_NAME_RAW
#else /* #ifdef CONFIG_TRACING */
static char pcu_name[] = PCU_NAME_RAW;
static const char *tp_pcu_varname __used __tracepoint_string = pcu_name;
#define PCU_NAME pcu_name
#endif /* #else #ifdef CONFIG_TRACING */

/* Forward declarations for tree_plugin.h */
static void pcu_bootup_announce(void);
static void pcu_qs(void);
static int pcu_preempt_blocked_readers_cgp(struct pcu_node *rnp);
#ifdef CONFIG_HOTPLUG_CPU
static bool pcu_preempt_has_tasks(struct pcu_node *rnp);
#endif /* #ifdef CONFIG_HOTPLUG_CPU */
static int pcu_print_task_exp_stall(struct pcu_node *rnp);
static void pcu_preempt_check_blocked_tasks(struct pcu_node *rnp);
static void pcu_flavor_sched_clock_irq(int user);
static void dump_blkd_tasks(struct pcu_node *rnp, int ncheck);
static void pcu_initiate_boost(struct pcu_node *rnp, unsigned long flags);
static void pcu_preempt_boost_start_gp(struct pcu_node *rnp);
static bool pcu_is_callbacks_kthread(void);
static void pcu_cpu_kthread_setup(unsigned int cpu);
static void pcu_spawn_one_boost_kthread(struct pcu_node *rnp);
static void pcu_spawn_boost_kthreads(void);
static void pcu_cleanup_after_idle(void);
static void pcu_prepare_for_idle(void);
static bool pcu_preempt_has_tasks(struct pcu_node *rnp);
static bool pcu_preempt_need_deferred_qs(struct task_struct *t);
static void pcu_preempt_deferred_qs(struct task_struct *t);
static void zero_cpu_stall_ticks(struct pcu_data *rdp);
static struct swait_queue_head *pcu_nocb_gp_get(struct pcu_node *rnp);
static void pcu_nocb_gp_cleanup(struct swait_queue_head *sq);
static void pcu_init_one_nocb(struct pcu_node *rnp);
static bool pcu_nocb_flush_bypass(struct pcu_data *rdp, struct pcu_head *rhp,
				  unsigned long j);
static bool pcu_nocb_try_bypass(struct pcu_data *rdp, struct pcu_head *rhp,
				bool *was_alldone, unsigned long flags);
static void __call_pcu_nocb_wake(struct pcu_data *rdp, bool was_empty,
				 unsigned long flags);
static int pcu_nocb_need_deferred_wakeup(struct pcu_data *rdp, int level);
static bool do_nocb_deferred_wakeup(struct pcu_data *rdp);
static void pcu_boot_init_nocb_percpu_data(struct pcu_data *rdp);
static void pcu_spawn_cpu_nocb_kthread(int cpu);
static void pcu_spawn_nocb_kthreads(void);
static void show_pcu_nocb_state(struct pcu_data *rdp);
static void pcu_nocb_lock(struct pcu_data *rdp);
static void pcu_nocb_unlock(struct pcu_data *rdp);
static void pcu_nocb_unlock_irqrestore(struct pcu_data *rdp,
				       unsigned long flags);
static void pcu_lockdep_assert_cblist_protected(struct pcu_data *rdp);
#ifdef CONFIG_RCU_NOCB_CPU
static void pcu_organize_nocb_kthreads(void);
#define pcu_nocb_lock_irqsave(rdp, flags)				\
do {									\
	if (!pcu_segcblist_is_offloaded(&(rdp)->cblist))		\
		local_irq_save(flags);					\
	else								\
		raw_spin_lock_irqsave(&(rdp)->nocb_lock, (flags));	\
} while (0)
#else /* #ifdef CONFIG_RCU_NOCB_CPU */
#define pcu_nocb_lock_irqsave(rdp, flags) local_irq_save(flags)
#endif /* #else #ifdef CONFIG_RCU_NOCB_CPU */

static void pcu_bind_gp_kthread(void);
static bool pcu_nohz_full_cpu(void);
static void pcu_dynticks_task_enter(void);
static void pcu_dynticks_task_exit(void);
static void pcu_dynticks_task_trace_enter(void);
static void pcu_dynticks_task_trace_exit(void);

/* Forward declarations for tree_stall.h */
static void record_gp_stall_check_time(void);
static void pcu_iw_handler(struct irq_work *iwp);
static void check_cpu_stall(struct pcu_data *rdp);
static void pcu_check_gp_start_stall(struct pcu_node *rnp, struct pcu_data *rdp,
				     const unsigned long gpssdelay);
