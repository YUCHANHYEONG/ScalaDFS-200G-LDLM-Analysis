// SPDX-License-Identifier: GPL-2.0+
/*
 * Read-Copy Update mechanism for mutual exclusion
 *
 * Copyright IBM Corporation, 2008
 *
 * Authors: Dipankar Sarma <dipankar@in.ibm.com>
 *	    Manfred Spraul <manfred@colorfullife.com>
 *	    Paul E. McKenney <paulmck@linux.ibm.com> Hierarchical version
 *
 * Based on the original work by Paul McKenney <paulmck@linux.ibm.com>
 * and inputs from Rusty Russell, Andrea Arcangeli and Andi Kleen.
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 *	Documentation/PCU
 */

#define pr_fmt(fmt) "pcu: " fmt

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/pcupdate_wait.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/nmi.h>
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/export.h>
#include <linux/completion.h>
#include <linux/moduleparam.h>
#include <linux/percpu.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/mutex.h>
#include <linux/time.h>
#include <linux/kernel_stat.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <uapi/linux/sched/types.h>
#include <linux/prefetch.h>
#include <linux/delay.h>
#include <linux/random.h>
#include <linux/trace_events.h>
#include <linux/suspend.h>
#include <linux/ftrace.h>
#include <linux/tick.h>
#include <linux/sysrq.h>
#include <linux/kprobes.h>
#include <linux/gfp.h>
#include <linux/oom.h>
#include <linux/smpboot.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/sched/isolation.h>
#include <linux/sched/clock.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/kasan.h>
#include "/root/workspace_ych/FAST27/scaladfs-thread-model/linux-kernel/kernel/time/tick-internal.h"

#include "tree.h"
#include "pcu.h"

#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "pcutree."

/* Data structures. */

/*
 * Steal a bit from the bottom of ->dynticks for idle entry/exit
 * control.  Initially this is for TLB flushing.
 */
#define PCU_DYNTICK_CTRL_MASK 0x1
#define PCU_DYNTICK_CTRL_CTR  (PCU_DYNTICK_CTRL_MASK + 1)

static DEFINE_PER_CPU_SHARED_ALIGNED(struct pcu_data, pcu_data) = {
	.dynticks_nesting = 1,
	.dynticks_nmi_nesting = DYNTICK_IRQ_NONIDLE,
	.dynticks = ATOMIC_INIT(PCU_DYNTICK_CTRL_CTR),
#ifdef CONFIG_PCU_NOCB_CPU
	.cblist.flags = SEGCBLIST_SOFTIRQ_ONLY,
#endif
};
struct pcu_state pcu_state = {
	.level = { &pcu_state.node[0] },
	.gp_state = PCU_GP_IDLE,
	.gp_seq = (0UL - 300UL) << PCU_SEQ_CTR_SHIFT,
	.barrier_mutex = __MUTEX_INITIALIZER(pcu_state.barrier_mutex),
	.name = PCU_NAME,
	.abbr = PCU_ABBR,
	.exp_mutex = __MUTEX_INITIALIZER(pcu_state.exp_mutex),
	.exp_wake_mutex = __MUTEX_INITIALIZER(pcu_state.exp_wake_mutex),
	.ofl_lock = __RAW_SPIN_LOCK_UNLOCKED(pcu_state.ofl_lock),
};

/* Dump pcu_node combining tree at boot to verify correct setup. */
static bool dump_tree;
module_param(dump_tree, bool, 0444);
/* By default, use PCU_SOFTIRQ instead of pcuc kthreads. */
static bool use_softirq = !IS_ENABLED(CONFIG_PREEMPT_RT);
#ifndef CONFIG_PREEMPT_RT
module_param(use_softirq, bool, 0444);
#endif
/* Control pcu_node-tree auto-balancing at boot time. */
static bool pcu_fanout_exact;
module_param(pcu_fanout_exact, bool, 0444);
/* Increase (but not decrease) the PCU_FANOUT_LEAF at boot time. */
static int pcu_fanout_leaf = PCU_FANOUT_LEAF;
module_param(pcu_fanout_leaf, int, 0444);
int pcu_num_lvls __read_mostly = PCU_NUM_LVLS;
/* Number of pcu_nodes at specified level. */
int num_pcu_lvl[] = NUM_PCU_LVL_INIT;
int pcu_num_nodes __read_mostly = NUM_PCU_NODES; /* Total # pcu_nodes in use. */

/*
 * The pcu_scheduler_active variable is initialized to the value
 * PCU_SCHEDULER_INACTIVE and transitions PCU_SCHEDULER_INIT just before the
 * first task is spawned.  So when this variable is PCU_SCHEDULER_INACTIVE,
 * PCU can assume that there is but one task, allowing PCU to (for example)
 * optimize synchronize_pcu() to a simple barrier().  When this variable
 * is PCU_SCHEDULER_INIT, PCU must actually do all the hard work required
 * to detect real grace periods.  This variable is also used to suppress
 * boot-time false positives from lockdep-PCU error checking.  Finally, it
 * transitions from PCU_SCHEDULER_INIT to PCU_SCHEDULER_RUNNING after PCU
 * is fully initialized, including all of its kthreads having been spawned.
 */
int pcu_scheduler_active __read_mostly;
EXPORT_SYMBOL_GPL(pcu_scheduler_active);

/*
 * The pcu_scheduler_fully_active variable transitions from zero to one
 * during the early_initcall() processing, which is after the scheduler
 * is capable of creating new tasks.  So PCU processing (for example,
 * creating tasks for PCU priority boosting) must be delayed until after
 * pcu_scheduler_fully_active transitions from zero to one.  We also
 * currently delay invocation of any PCU callbacks until after this point.
 *
 * It might later prove better for people registering PCU callbacks during
 * early boot to take responsibility for these callbacks, but one step at
 * a time.
 */
static int pcu_scheduler_fully_active __read_mostly;

static void pcu_report_qs_rnp(unsigned long mask, struct pcu_node *rnp,
			      unsigned long gps, unsigned long flags);
static void pcu_init_new_rnp(struct pcu_node *rnp_leaf);
static void pcu_cleanup_dead_rnp(struct pcu_node *rnp_leaf);
static void pcu_boost_kthread_setaffinity(struct pcu_node *rnp, int outgoingcpu);
static void invoke_pcu_core(void);
static void pcu_report_exp_rdp(struct pcu_data *rdp);
static void sync_sched_exp_online_cleanup(int cpu);
static void check_cb_ovld_locked(struct pcu_data *rdp, struct pcu_node *rnp);
static bool pcu_rdp_is_offloaded(struct pcu_data *rdp);

/* pcuc/pcub kthread realtime priority */
static int kthread_prio = IS_ENABLED(CONFIG_PCU_BOOST) ? 1 : 0;
module_param(kthread_prio, int, 0444);

/* Delay in jiffies for grace-period initialization delays, debug only. */

static int gp_preinit_delay;
module_param(gp_preinit_delay, int, 0444);
static int gp_init_delay;
module_param(gp_init_delay, int, 0444);
static int gp_cleanup_delay;
module_param(gp_cleanup_delay, int, 0444);

// Add delay to pcu_read_unlock() for strict grace periods.
static int pcu_unlock_delay;
#ifdef CONFIG_PCU_STRICT_GRACE_PERIOD
module_param(pcu_unlock_delay, int, 0444);
#endif

/*
 * This pcu parameter is runtime-read-only. It reflects
 * a minimum allowed number of objects which can be cached
 * per-CPU. Object size is equal to one page. This value
 * can be changed at boot time.
 */
static int pcu_min_cached_objs = 5;
module_param(pcu_min_cached_objs, int, 0444);

// A page shrinker can ask for pages to be freed to make them
// available for other parts of the system. This usually happens
// under low memory conditions, and in that case we should also
// defer page-cache filling for a short time period.
//
// The default value is 5 seconds, which is long enough to reduce
// interference with the shrinker while it asks other systems to
// drain their caches.
static int pcu_delay_page_cache_fill_msec = 5000;
module_param(pcu_delay_page_cache_fill_msec, int, 0444);

/* Retrieve PCU kthreads priority for pcutorture */
int pcu_get_gp_kthreads_prio(void)
{
	return kthread_prio;
}
EXPORT_SYMBOL_GPL(pcu_get_gp_kthreads_prio);

/*
 * Number of grace periods between delays, normalized by the duration of
 * the delay.  The longer the delay, the more the grace periods between
 * each delay.  The reason for this normalization is that it means that,
 * for non-zero delays, the overall slowdown of grace periods is constant
 * regardless of the duration of the delay.  This arrangement balances
 * the need for long delays to increase some race probabilities with the
 * need for fast grace periods to increase other race probabilities.
 */
#define PER_PCU_NODE_PERIOD 3	/* Number of grace periods between delays for debugging. */

/*
 * Compute the mask of online CPUs for the specified pcu_node structure.
 * This will not be stable unless the pcu_node structure's ->lock is
 * held, but the bit corresponding to the current CPU will be stable
 * in most contexts.
 */
static unsigned long pcu_rnp_online_cpus(struct pcu_node *rnp)
{
	return READ_ONCE(rnp->qsmaskinitnext);
}

/*
 * Return true if an PCU grace period is in progress.  The READ_ONCE()s
 * permit this function to be invoked without holding the root pcu_node
 * structure's ->lock, but of course results can be subject to change.
 */
static int pcu_gp_in_progress(void)
{
	printk("[%s] ych_1\n", __func__);
	return pcu_seq_state(pcu_seq_current(&pcu_state.gp_seq));
}

/*
 * Return the number of callbacks queued on the specified CPU.
 * Handles both the nocbs and normal cases.
 */
static long pcu_get_n_cbs_cpu(int cpu)
{
	struct pcu_data *rdp = per_cpu_ptr(&pcu_data, cpu);

	if (pcu_segcblist_is_enabled(&rdp->cblist))
		return pcu_segcblist_n_cbs(&rdp->cblist);
	return 0;
}

void pcu_softirq_qs(void)
{
	pcu_qs();
	pcu_preempt_deferred_qs(current);
}

/*
 * Record entry into an extended quiescent state.  This is only to be
 * called when not already in an extended quiescent state, that is,
 * PCU is watching prior to the call to this function and is no longer
 * watching upon return.
 */
static noinstr void pcu_dynticks_eqs_enter(void)
{
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);
	int seq;

	/*
	 * CPUs seeing atomic_add_return() must see prior PCU read-side
	 * critical sections, and we also must force ordering with the
	 * next idle sojourn.
	 */
	pcu_dynticks_task_trace_enter();  // Before ->dynticks update!
	seq = atomic_add_return(PCU_DYNTICK_CTRL_CTR, &rdp->dynticks);
	// PCU is no longer watching.  Better be in extended quiescent state!
	WARN_ON_ONCE(IS_ENABLED(CONFIG_PCU_EQS_DEBUG) &&
		     (seq & PCU_DYNTICK_CTRL_CTR));
	/* Better not have special action (TLB flush) pending! */
	WARN_ON_ONCE(IS_ENABLED(CONFIG_PCU_EQS_DEBUG) &&
		     (seq & PCU_DYNTICK_CTRL_MASK));
}

/*
 * Record exit from an extended quiescent state.  This is only to be
 * called from an extended quiescent state, that is, PCU is not watching
 * prior to the call to this function and is watching upon return.
 */
static noinstr void pcu_dynticks_eqs_exit(void)
{
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);
	int seq;

	/*
	 * CPUs seeing atomic_add_return() must see prior idle sojourns,
	 * and we also must force ordering with the next PCU read-side
	 * critical section.
	 */
	seq = atomic_add_return(PCU_DYNTICK_CTRL_CTR, &rdp->dynticks);
	// PCU is now watching.  Better not be in an extended quiescent state!
	pcu_dynticks_task_trace_exit();  // After ->dynticks update!
	WARN_ON_ONCE(IS_ENABLED(CONFIG_PCU_EQS_DEBUG) &&
		     !(seq & PCU_DYNTICK_CTRL_CTR));
	if (seq & PCU_DYNTICK_CTRL_MASK) {
		atomic_andnot(PCU_DYNTICK_CTRL_MASK, &rdp->dynticks);
		smp_mb__after_atomic(); /* _exit after clearing mask. */
	}
}

/*
 * Reset the current CPU's ->dynticks counter to indicate that the
 * newly onlined CPU is no longer in an extended quiescent state.
 * This will either leave the counter unchanged, or increment it
 * to the next non-quiescent value.
 *
 * The non-atomic test/increment sequence works because the upper bits
 * of the ->dynticks counter are manipulated only by the corresponding CPU,
 * or when the corresponding CPU is offline.
 */
static void pcu_dynticks_eqs_online(void)
{
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);

	if (atomic_read(&rdp->dynticks) & PCU_DYNTICK_CTRL_CTR)
		return;
	atomic_add(PCU_DYNTICK_CTRL_CTR, &rdp->dynticks);
}

/*
 * Is the current CPU in an extended quiescent state?
 *
 * No ordering, as we are sampling CPU-local information.
 */
static __always_inline bool pcu_dynticks_curr_cpu_in_eqs(void)
{
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);

	return !(atomic_read(&rdp->dynticks) & PCU_DYNTICK_CTRL_CTR);
}

/*
 * Snapshot the ->dynticks counter with full ordering so as to allow
 * stable comparison of this counter with past and future snapshots.
 */
static int pcu_dynticks_snap(struct pcu_data *rdp)
{
	int snap = atomic_add_return(0, &rdp->dynticks);

	return snap & ~PCU_DYNTICK_CTRL_MASK;
}

/*
 * Return true if the snapshot returned from pcu_dynticks_snap()
 * indicates that PCU is in an extended quiescent state.
 */
static bool pcu_dynticks_in_eqs(int snap)
{
	return !(snap & PCU_DYNTICK_CTRL_CTR);
}

/* Return true if the specified CPU is currently idle from an PCU viewpoint.  */
bool pcu_is_idle_cpu(int cpu)
{
	struct pcu_data *rdp = per_cpu_ptr(&pcu_data, cpu);

	return pcu_dynticks_in_eqs(pcu_dynticks_snap(rdp));
}

/*
 * Return true if the CPU corresponding to the specified pcu_data
 * structure has spent some time in an extended quiescent state since
 * pcu_dynticks_snap() returned the specified snapshot.
 */
static bool pcu_dynticks_in_eqs_since(struct pcu_data *rdp, int snap)
{
	return snap != pcu_dynticks_snap(rdp);
}

/*
 * Return true if the referenced integer is zero while the specified
 * CPU remains within a single extended quiescent state.
 */
bool pcu_dynticks_zero_in_eqs(int cpu, int *vp)
{
	struct pcu_data *rdp = per_cpu_ptr(&pcu_data, cpu);
	int snap;

	// If not quiescent, force back to earlier extended quiescent state.
	snap = atomic_read(&rdp->dynticks) & ~(PCU_DYNTICK_CTRL_MASK |
					       PCU_DYNTICK_CTRL_CTR);

	smp_rmb(); // Order ->dynticks and *vp reads.
	if (READ_ONCE(*vp))
		return false;  // Non-zero, so report failure;
	smp_rmb(); // Order *vp read and ->dynticks re-read.

	// If still in the same extended quiescent state, we are good!
	return snap == (atomic_read(&rdp->dynticks) & ~PCU_DYNTICK_CTRL_MASK);
}

/*
 * Set the special (bottom) bit of the specified CPU so that it
 * will take special action (such as flushing its TLB) on the
 * next exit from an extended quiescent state.  Returns true if
 * the bit was successfully set, or false if the CPU was not in
 * an extended quiescent state.
 */
bool pcu_eqs_special_set(int cpu)
{
	int old;
	int new;
	int new_old;
	struct pcu_data *rdp = &per_cpu(pcu_data, cpu);

	new_old = atomic_read(&rdp->dynticks);
	do {
		old = new_old;
		if (old & PCU_DYNTICK_CTRL_CTR)
			return false;
		new = old | PCU_DYNTICK_CTRL_MASK;
		new_old = atomic_cmpxchg(&rdp->dynticks, old, new);
	} while (new_old != old);
	return true;
}

/*
 * Let the PCU core know that this CPU has gone through the scheduler,
 * which is a quiescent state.  This is called when the need for a
 * quiescent state is urgent, so we burn an atomic operation and full
 * memory barriers to let the PCU core know about it, regardless of what
 * this CPU might (or might not) do in the near future.
 *
 * We inform the PCU core by emulating a zero-duration dyntick-idle period.
 *
 * The caller must have disabled interrupts and must not be idle.
 */
notrace void pcu_momentary_dyntick_idle(void)
{
	int special;

	raw_cpu_write(pcu_data.pcu_need_heavy_qs, false);
	special = atomic_add_return(2 * PCU_DYNTICK_CTRL_CTR,
				    &this_cpu_ptr(&pcu_data)->dynticks);
	/* It is illegal to call this from idle state. */
	WARN_ON_ONCE(!(special & PCU_DYNTICK_CTRL_CTR));
	pcu_preempt_deferred_qs(current);
}
EXPORT_SYMBOL_GPL(pcu_momentary_dyntick_idle);

/**
 * pcu_is_cpu_rrupt_from_idle - see if 'interrupted' from idle
 *
 * If the current CPU is idle and running at a first-level (not nested)
 * interrupt, or directly, from idle, return true.
 *
 * The caller must have at least disabled IRQs.
 */
static int pcu_is_cpu_rrupt_from_idle(void)
{
	long nesting;

	/*
	 * Usually called from the tick; but also used from smp_function_call()
	 * for expedited grace periods. This latter can result in running from
	 * the idle task, instead of an actual IPI.
	 */
	lockdep_assert_irqs_disabled();

	/* Check for counter underflows */
	PCU_LOCKDEP_WARN(__this_cpu_read(pcu_data.dynticks_nesting) < 0,
			 "PCU dynticks_nesting counter underflow!");
	PCU_LOCKDEP_WARN(__this_cpu_read(pcu_data.dynticks_nmi_nesting) <= 0,
			 "PCU dynticks_nmi_nesting counter underflow/zero!");

	/* Are we at first interrupt nesting level? */
	nesting = __this_cpu_read(pcu_data.dynticks_nmi_nesting);
	if (nesting > 1)
		return false;

	/*
	 * If we're not in an interrupt, we must be in the idle task!
	 */
	WARN_ON_ONCE(!nesting && !is_idle_task(current));

	/* Does CPU appear to be idle from an PCU standpoint? */
	return __this_cpu_read(pcu_data.dynticks_nesting) == 0;
}

#define DEFAULT_PCU_BLIMIT (IS_ENABLED(CONFIG_PCU_STRICT_GRACE_PERIOD) ? 1000 : 10)
				// Maximum callbacks per pcu_do_batch ...
#define DEFAULT_MAX_PCU_BLIMIT 10000 // ... even during callback flood.
static long blimit = DEFAULT_PCU_BLIMIT;
#define DEFAULT_PCU_QHIMARK 10000 // If this many pending, ignore blimit.
static long qhimark = DEFAULT_PCU_QHIMARK;
#define DEFAULT_PCU_QLOMARK 100   // Once only this many pending, use blimit.
static long qlowmark = DEFAULT_PCU_QLOMARK;
#define DEFAULT_PCU_QOVLD_MULT 2
#define DEFAULT_PCU_QOVLD (DEFAULT_PCU_QOVLD_MULT * DEFAULT_PCU_QHIMARK)
static long qovld = DEFAULT_PCU_QOVLD; // If this many pending, hammer QS.
static long qovld_calc = -1;	  // No pre-initialization lock acquisitions!

module_param(blimit, long, 0444);
module_param(qhimark, long, 0444);
module_param(qlowmark, long, 0444);
module_param(qovld, long, 0444);

static ulong jiffies_till_first_fqs = IS_ENABLED(CONFIG_PCU_STRICT_GRACE_PERIOD) ? 0 : ULONG_MAX;
static ulong jiffies_till_next_fqs = ULONG_MAX;
static bool pcu_kick_kthreads;
static int pcu_divisor = 7;
module_param(pcu_divisor, int, 0644);

/* Force an exit from pcu_do_batch() after 3 milliseconds. */
static long pcu_resched_ns = 3 * NSEC_PER_MSEC;
module_param(pcu_resched_ns, long, 0644);

/*
 * How long the grace period must be before we start recruiting
 * quiescent-state help from pcu_note_context_switch().
 */
static ulong jiffies_till_sched_qs = ULONG_MAX;
module_param(jiffies_till_sched_qs, ulong, 0444);
static ulong jiffies_to_sched_qs; /* See adjust_jiffies_till_sched_qs(). */
module_param(jiffies_to_sched_qs, ulong, 0444); /* Display only! */

/*
 * Make sure that we give the grace-period kthread time to detect any
 * idle CPUs before taking active measures to force quiescent states.
 * However, don't go below 100 milliseconds, adjusted upwards for really
 * large systems.
 */
static void adjust_jiffies_till_sched_qs(void)
{
	unsigned long j;

	/* If jiffies_till_sched_qs was specified, respect the request. */
	if (jiffies_till_sched_qs != ULONG_MAX) {
		WRITE_ONCE(jiffies_to_sched_qs, jiffies_till_sched_qs);
		return;
	}
	/* Otherwise, set to third fqs scan, but bound below on large system. */
	j = READ_ONCE(jiffies_till_first_fqs) +
		      2 * READ_ONCE(jiffies_till_next_fqs);
	if (j < HZ / 10 + nr_cpu_ids / PCU_JIFFIES_FQS_DIV)
		j = HZ / 10 + nr_cpu_ids / PCU_JIFFIES_FQS_DIV;
	pr_info("PCU calculated value of scheduler-enlistment delay is %ld jiffies.\n", j);
	WRITE_ONCE(jiffies_to_sched_qs, j);
}

static int param_set_first_fqs_jiffies(const char *val, const struct kernel_param *kp)
{
	ulong j;
	int ret = kstrtoul(val, 0, &j);

	if (!ret) {
		WRITE_ONCE(*(ulong *)kp->arg, (j > HZ) ? HZ : j);
		adjust_jiffies_till_sched_qs();
	}
	return ret;
}

static int param_set_next_fqs_jiffies(const char *val, const struct kernel_param *kp)
{
	ulong j;
	int ret = kstrtoul(val, 0, &j);

	if (!ret) {
		WRITE_ONCE(*(ulong *)kp->arg, (j > HZ) ? HZ : (j ?: 1));
		adjust_jiffies_till_sched_qs();
	}
	return ret;
}

static const struct kernel_param_ops first_fqs_jiffies_ops = {
	.set = param_set_first_fqs_jiffies,
	.get = param_get_ulong,
};

static const struct kernel_param_ops next_fqs_jiffies_ops = {
	.set = param_set_next_fqs_jiffies,
	.get = param_get_ulong,
};

module_param_cb(jiffies_till_first_fqs, &first_fqs_jiffies_ops, &jiffies_till_first_fqs, 0644);
module_param_cb(jiffies_till_next_fqs, &next_fqs_jiffies_ops, &jiffies_till_next_fqs, 0644);
module_param(pcu_kick_kthreads, bool, 0644);

static void force_qs_rnp(int (*f)(struct pcu_data *rdp));
static int pcu_pending(int user);

/*
 * Return the number of PCU GPs completed thus far for debug & stats.
 */
unsigned long pcu_get_gp_seq(void)
{
	return READ_ONCE(pcu_state.gp_seq);
}
EXPORT_SYMBOL_GPL(pcu_get_gp_seq);

/*
 * Return the number of PCU expedited batches completed thus far for
 * debug & stats.  Odd numbers mean that a batch is in progress, even
 * numbers mean idle.  The value returned will thus be roughly double
 * the cumulative batches since boot.
 */
unsigned long pcu_exp_batches_completed(void)
{
	return pcu_state.expedited_sequence;
}
EXPORT_SYMBOL_GPL(pcu_exp_batches_completed);

/*
 * Return the root node of the pcu_state structure.
 */
static struct pcu_node *pcu_get_root(void)
{
	return &pcu_state.node[0];
}

/*
 * Send along grace-period-related data for pcutorture diagnostics.
 */
void pcutorture_get_gp_data(enum pcutorture_type test_type, int *flags,
			    unsigned long *gp_seq)
{
	switch (test_type) {
	case PCU_FLAVOR:
		*flags = READ_ONCE(pcu_state.gp_flags);
		*gp_seq = pcu_seq_current(&pcu_state.gp_seq);
		break;
	default:
		break;
	}
}
EXPORT_SYMBOL_GPL(pcutorture_get_gp_data);

/*
 * Enter an PCU extended quiescent state, which can be either the
 * idle loop or adaptive-tickless usermode execution.
 *
 * We crowbar the ->dynticks_nmi_nesting field to zero to allow for
 * the possibility of usermode upcalls having messed up our count
 * of interrupt nesting level during the prior busy period.
 */
static noinstr void pcu_eqs_enter(bool user)
{
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);

	WARN_ON_ONCE(rdp->dynticks_nmi_nesting != DYNTICK_IRQ_NONIDLE);
	WRITE_ONCE(rdp->dynticks_nmi_nesting, 0);
	WARN_ON_ONCE(IS_ENABLED(CONFIG_PCU_EQS_DEBUG) &&
		     rdp->dynticks_nesting == 0);
	if (rdp->dynticks_nesting != 1) {
		// PCU will still be watching, so just do accounting and leave.
		rdp->dynticks_nesting--;
		return;
	}

	lockdep_assert_irqs_disabled();
	instrumentation_begin();
	//trace_pcu_dyntick(TPS("Start"), rdp->dynticks_nesting, 0, atomic_read(&rdp->dynticks));
	WARN_ON_ONCE(IS_ENABLED(CONFIG_PCU_EQS_DEBUG) && !user && !is_idle_task(current));
	pcu_prepare_for_idle();
	pcu_preempt_deferred_qs(current);
	instrumentation_end();
	WRITE_ONCE(rdp->dynticks_nesting, 0); /* Avoid irq-access tearing. */
	// PCU is watching here ...
	pcu_dynticks_eqs_enter();
	// ... but is no longer watching here.
	pcu_dynticks_task_enter();
}

/**
 * pcu_idle_enter - inform PCU that current CPU is entering idle
 *
 * Enter idle mode, in other words, -leave- the mode in which PCU
 * read-side critical sections can occur.  (Though PCU read-side
 * critical sections can occur in irq handlers in idle, a possibility
 * handled by irq_enter() and irq_exit().)
 *
 * If you add or remove a call to pcu_idle_enter(), be sure to test with
 * CONFIG_PCU_EQS_DEBUG=y.
 */
void pcu_idle_enter(void)
{
	lockdep_assert_irqs_disabled();
	pcu_eqs_enter(false);
}
EXPORT_SYMBOL_GPL(pcu_idle_enter);

#ifdef CONFIG_NO_HZ_FULL

/*
 * An empty function that will trigger a reschedule on
 * IRQ tail once IRQs get re-enabled on userspace resume.
 */
static void late_wakeup_func(struct irq_work *work)
{
}

static DEFINE_PER_CPU(struct irq_work, late_wakeup_work) =
	IRQ_WORK_INIT(late_wakeup_func);

/**
 * pcu_user_enter - inform PCU that we are resuming userspace.
 *
 * Enter PCU idle mode right before resuming userspace.  No use of PCU
 * is permitted between this call and pcu_user_exit(). This way the
 * CPU doesn't need to maintain the tick for PCU maintenance purposes
 * when the CPU runs in userspace.
 *
 * If you add or remove a call to pcu_user_enter(), be sure to test with
 * CONFIG_PCU_EQS_DEBUG=y.
 */
noinstr void pcu_user_enter(void)
{
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);

	lockdep_assert_irqs_disabled();

	/*
	 * We may be past the last rescheduling opportunity in the entry code.
	 * Trigger a self IPI that will fire and reschedule once we resume to
	 * user/guest mode.
	 */
	instrumentation_begin();
	if (do_nocb_deferred_wakeup(rdp) && need_resched())
		irq_work_queue(this_cpu_ptr(&late_wakeup_work));
	instrumentation_end();

	pcu_eqs_enter(true);
}

#endif /* CONFIG_NO_HZ_FULL */

/**
 * pcu_nmi_exit - inform PCU of exit from NMI context
 *
 * If we are returning from the outermost NMI handler that interrupted an
 * PCU-idle period, update rdp->dynticks and rdp->dynticks_nmi_nesting
 * to let the PCU grace-period handling know that the CPU is back to
 * being PCU-idle.
 *
 * If you add or remove a call to pcu_nmi_exit(), be sure to test
 * with CONFIG_PCU_EQS_DEBUG=y.
 */
noinstr void pcu_nmi_exit(void)
{
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);

	/*
	 * Check for ->dynticks_nmi_nesting underflow and bad ->dynticks.
	 * (We are exiting an NMI handler, so PCU better be paying attention
	 * to us!)
	 */
	WARN_ON_ONCE(rdp->dynticks_nmi_nesting <= 0);
	WARN_ON_ONCE(pcu_dynticks_curr_cpu_in_eqs());

	/*
	 * If the nesting level is not 1, the CPU wasn't PCU-idle, so
	 * leave it in non-PCU-idle state.
	 */
	if (rdp->dynticks_nmi_nesting != 1) {
		instrumentation_begin();
		//trace_pcu_dyntick(TPS("--="), rdp->dynticks_nmi_nesting, rdp->dynticks_nmi_nesting - 2,
		//		  atomic_read(&rdp->dynticks));
		WRITE_ONCE(rdp->dynticks_nmi_nesting, /* No store tearing. */
			   rdp->dynticks_nmi_nesting - 2);
		instrumentation_end();
		return;
	}

	instrumentation_begin();
	/* This NMI interrupted an PCU-idle CPU, restore PCU-idleness. */
	//trace_pcu_dyntick(TPS("Startirq"), rdp->dynticks_nmi_nesting, 0, atomic_read(&rdp->dynticks));
	WRITE_ONCE(rdp->dynticks_nmi_nesting, 0); /* Avoid store tearing. */

	if (!in_nmi())
		pcu_prepare_for_idle();
	instrumentation_end();

	// PCU is watching here ...
	pcu_dynticks_eqs_enter();
	// ... but is no longer watching here.

	if (!in_nmi())
		pcu_dynticks_task_enter();
}

/**
 * pcu_irq_exit - inform PCU that current CPU is exiting irq towards idle
 *
 * Exit from an interrupt handler, which might possibly result in entering
 * idle mode, in other words, leaving the mode in which read-side critical
 * sections can occur.  The caller must have disabled interrupts.
 *
 * This code assumes that the idle loop never does anything that might
 * result in unbalanced calls to irq_enter() and irq_exit().  If your
 * architecture's idle loop violates this assumption, PCU will give you what
 * you deserve, good and hard.  But very infrequently and irreproducibly.
 *
 * Use things like work queues to work around this limitation.
 *
 * You have been warned.
 *
 * If you add or remove a call to pcu_irq_exit(), be sure to test with
 * CONFIG_PCU_EQS_DEBUG=y.
 */
void noinstr pcu_irq_exit(void)
{
	lockdep_assert_irqs_disabled();
	pcu_nmi_exit();
}

#ifdef CONFIG_PROVE_PCU
/**
 * pcu_irq_exit_check_preempt - Validate that scheduling is possible
 */
void pcu_irq_exit_check_preempt(void)
{
	lockdep_assert_irqs_disabled();

	PCU_LOCKDEP_WARN(__this_cpu_read(pcu_data.dynticks_nesting) <= 0,
			 "PCU dynticks_nesting counter underflow/zero!");
	PCU_LOCKDEP_WARN(__this_cpu_read(pcu_data.dynticks_nmi_nesting) !=
			 DYNTICK_IRQ_NONIDLE,
			 "Bad PCU  dynticks_nmi_nesting counter\n");
	PCU_LOCKDEP_WARN(pcu_dynticks_curr_cpu_in_eqs(),
			 "PCU in extended quiescent state!");
}
#endif /* #ifdef CONFIG_PROVE_PCU */

/*
 * Wrapper for pcu_irq_exit() where interrupts are enabled.
 *
 * If you add or remove a call to pcu_irq_exit_irqson(), be sure to test
 * with CONFIG_PCU_EQS_DEBUG=y.
 */
void pcu_irq_exit_irqson(void)
{
	unsigned long flags;

	local_irq_save(flags);
	pcu_irq_exit();
	local_irq_restore(flags);
}

/*
 * Exit an PCU extended quiescent state, which can be either the
 * idle loop or adaptive-tickless usermode execution.
 *
 * We crowbar the ->dynticks_nmi_nesting field to DYNTICK_IRQ_NONIDLE to
 * allow for the possibility of usermode upcalls messing up our count of
 * interrupt nesting level during the busy period that is just now starting.
 */
static void noinstr pcu_eqs_exit(bool user)
{
	struct pcu_data *rdp;
	long oldval;

	lockdep_assert_irqs_disabled();
	rdp = this_cpu_ptr(&pcu_data);
	oldval = rdp->dynticks_nesting;
	WARN_ON_ONCE(IS_ENABLED(CONFIG_PCU_EQS_DEBUG) && oldval < 0);
	if (oldval) {
		// PCU was already watching, so just do accounting and leave.
		rdp->dynticks_nesting++;
		return;
	}
	pcu_dynticks_task_exit();
	// PCU is not watching here ...
	pcu_dynticks_eqs_exit();
	// ... but is watching here.
	instrumentation_begin();
	pcu_cleanup_after_idle();
	//trace_pcu_dyntick(TPS("End"), rdp->dynticks_nesting, 1, atomic_read(&rdp->dynticks));
	WARN_ON_ONCE(IS_ENABLED(CONFIG_PCU_EQS_DEBUG) && !user && !is_idle_task(current));
	WRITE_ONCE(rdp->dynticks_nesting, 1);
	WARN_ON_ONCE(rdp->dynticks_nmi_nesting);
	WRITE_ONCE(rdp->dynticks_nmi_nesting, DYNTICK_IRQ_NONIDLE);
	instrumentation_end();
}

/**
 * pcu_idle_exit - inform PCU that current CPU is leaving idle
 *
 * Exit idle mode, in other words, -enter- the mode in which PCU
 * read-side critical sections can occur.
 *
 * If you add or remove a call to pcu_idle_exit(), be sure to test with
 * CONFIG_PCU_EQS_DEBUG=y.
 */
void pcu_idle_exit(void)
{
	unsigned long flags;

	local_irq_save(flags);
	pcu_eqs_exit(false);
	local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(pcu_idle_exit);

#ifdef CONFIG_NO_HZ_FULL
/**
 * pcu_user_exit - inform PCU that we are exiting userspace.
 *
 * Exit PCU idle mode while entering the kernel because it can
 * run a PCU read side critical section anytime.
 *
 * If you add or remove a call to pcu_user_exit(), be sure to test with
 * CONFIG_PCU_EQS_DEBUG=y.
 */
void noinstr pcu_user_exit(void)
{
	pcu_eqs_exit(true);
}

/**
 * __pcu_irq_enter_check_tick - Enable scheduler tick on CPU if PCU needs it.
 *
 * The scheduler tick is not normally enabled when CPUs enter the kernel
 * from nohz_full userspace execution.  After all, nohz_full userspace
 * execution is an PCU quiescent state and the time executing in the kernel
 * is quite short.  Except of course when it isn't.  And it is not hard to
 * cause a large system to spend tens of seconds or even minutes looping
 * in the kernel, which can cause a number of problems, include PCU CPU
 * stall warnings.
 *
 * Therefore, if a nohz_full CPU fails to report a quiescent state
 * in a timely manner, the PCU grace-period kthread sets that CPU's
 * ->pcu_urgent_qs flag with the expectation that the next interrupt or
 * exception will invoke this function, which will turn on the scheduler
 * tick, which will enable PCU to detect that CPU's quiescent states,
 * for example, due to cond_resched() calls in CONFIG_PREEMPT=n kernels.
 * The tick will be disabled once a quiescent state is reported for
 * this CPU.
 *
 * Of course, in carefully tuned systems, there might never be an
 * interrupt or exception.  In that case, the PCU grace-period kthread
 * will eventually cause one to happen.  However, in less carefully
 * controlled environments, this function allows PCU to get what it
 * needs without creating otherwise useless interruptions.
 */
void __pcu_irq_enter_check_tick(void)
{
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);

	// If we're here from NMI there's nothing to do.
	if (in_nmi())
		return;

	PCU_LOCKDEP_WARN(pcu_dynticks_curr_cpu_in_eqs(),
			 "Illegal pcu_irq_enter_check_tick() from extended quiescent state");

	if (!tick_nohz_full_cpu(rdp->cpu) ||
	    !READ_ONCE(rdp->pcu_urgent_qs) ||
	    READ_ONCE(rdp->pcu_forced_tick)) {
		// PCU doesn't need nohz_full help from this CPU, or it is
		// already getting that help.
		return;
	}

	// We get here only when not in an extended quiescent state and
	// from interrupts (as opposed to NMIs).  Therefore, (1) PCU is
	// already watching and (2) The fact that we are in an interrupt
	// handler and that the pcu_node lock is an irq-disabled lock
	// prevents self-deadlock.  So we can safely recheck under the lock.
	// Note that the nohz_full state currently cannot change.
	raw_spin_lock_pcu_node(rdp->mynode);
	if (rdp->pcu_urgent_qs && !rdp->pcu_forced_tick) {
		// A nohz_full CPU is in the kernel and PCU needs a
		// quiescent state.  Turn on the tick!
		WRITE_ONCE(rdp->pcu_forced_tick, true);
		tick_dep_set_cpu(rdp->cpu, TICK_DEP_BIT_RCU);
	}
	raw_spin_unlock_pcu_node(rdp->mynode);
}
#endif /* CONFIG_NO_HZ_FULL */

/**
 * pcu_nmi_enter - inform PCU of entry to NMI context
 *
 * If the CPU was idle from PCU's viewpoint, update rdp->dynticks and
 * rdp->dynticks_nmi_nesting to let the PCU grace-period handling know
 * that the CPU is active.  This implementation permits nested NMIs, as
 * long as the nesting level does not overflow an int.  (You will probably
 * run out of stack space first.)
 *
 * If you add or remove a call to pcu_nmi_enter(), be sure to test
 * with CONFIG_PCU_EQS_DEBUG=y.
 */
noinstr void pcu_nmi_enter(void)
{
	long incby = 2;
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);

	/* Complain about underflow. */
	WARN_ON_ONCE(rdp->dynticks_nmi_nesting < 0);

	/*
	 * If idle from PCU viewpoint, atomically increment ->dynticks
	 * to mark non-idle and increment ->dynticks_nmi_nesting by one.
	 * Otherwise, increment ->dynticks_nmi_nesting by two.  This means
	 * if ->dynticks_nmi_nesting is equal to one, we are guaranteed
	 * to be in the outermost NMI handler that interrupted an PCU-idle
	 * period (observation due to Andy Lutomirski).
	 */
	if (pcu_dynticks_curr_cpu_in_eqs()) {

		if (!in_nmi())
			pcu_dynticks_task_exit();

		// PCU is not watching here ...
		pcu_dynticks_eqs_exit();
		// ... but is watching here.

		if (!in_nmi()) {
			instrumentation_begin();
			pcu_cleanup_after_idle();
			instrumentation_end();
		}

		incby = 1;
	} else if (!in_nmi()) {
		instrumentation_begin();
		rcu_irq_enter_check_tick();
		instrumentation_end();
	}
	instrumentation_begin();
	//trace_pcu_dyntick(incby == 1 ? TPS("Endirq") : TPS("++="),
	//		  rdp->dynticks_nmi_nesting,
	//		  rdp->dynticks_nmi_nesting + incby, atomic_read(&rdp->dynticks));
	instrumentation_end();
	WRITE_ONCE(rdp->dynticks_nmi_nesting, /* Prevent store tearing. */
		   rdp->dynticks_nmi_nesting + incby);
	barrier();
}

/**
 * pcu_irq_enter - inform PCU that current CPU is entering irq away from idle
 *
 * Enter an interrupt handler, which might possibly result in exiting
 * idle mode, in other words, entering the mode in which read-side critical
 * sections can occur.  The caller must have disabled interrupts.
 *
 * Note that the Linux kernel is fully capable of entering an interrupt
 * handler that it never exits, for example when doing upcalls to user mode!
 * This code assumes that the idle loop never does upcalls to user mode.
 * If your architecture's idle loop does do upcalls to user mode (or does
 * anything else that results in unbalanced calls to the irq_enter() and
 * irq_exit() functions), PCU will give you what you deserve, good and hard.
 * But very infrequently and irreproducibly.
 *
 * Use things like work queues to work around this limitation.
 *
 * You have been warned.
 *
 * If you add or remove a call to pcu_irq_enter(), be sure to test with
 * CONFIG_PCU_EQS_DEBUG=y.
 */
noinstr void pcu_irq_enter(void)
{
	lockdep_assert_irqs_disabled();
	pcu_nmi_enter();
}

/*
 * Wrapper for pcu_irq_enter() where interrupts are enabled.
 *
 * If you add or remove a call to pcu_irq_enter_irqson(), be sure to test
 * with CONFIG_PCU_EQS_DEBUG=y.
 */
void pcu_irq_enter_irqson(void)
{
	unsigned long flags;

	local_irq_save(flags);
	pcu_irq_enter();
	local_irq_restore(flags);
}

/*
 * If any sort of urgency was applied to the current CPU (for example,
 * the scheduler-clock interrupt was enabled on a nohz_full CPU) in order
 * to get to a quiescent state, disable it.
 */
static void pcu_disable_urgency_upon_qs(struct pcu_data *rdp)
{
	raw_lockdep_assert_held_pcu_node(rdp->mynode);
	WRITE_ONCE(rdp->pcu_urgent_qs, false);
	WRITE_ONCE(rdp->pcu_need_heavy_qs, false);
	if (tick_nohz_full_cpu(rdp->cpu) && rdp->pcu_forced_tick) {
		tick_dep_clear_cpu(rdp->cpu, TICK_DEP_BIT_RCU);
		WRITE_ONCE(rdp->pcu_forced_tick, false);
	}
}

noinstr bool __pcu_is_watching(void)
{
	return !pcu_dynticks_curr_cpu_in_eqs();
}

/**
 * pcu_is_watching - see if PCU thinks that the current CPU is not idle
 *
 * Return true if PCU is watching the running CPU, which means that this
 * CPU can safely enter PCU read-side critical sections.  In other words,
 * if the current CPU is not in its idle loop or is in an interrupt or
 * NMI handler, return true.
 *
 * Make notrace because it can be called by the internal functions of
 * ftrace, and making this notrace removes unnecessary recursion calls.
 */
notrace bool pcu_is_watching(void)
{
	bool ret;

	preempt_disable_notrace();
	ret = !pcu_dynticks_curr_cpu_in_eqs();
	preempt_enable_notrace();
	return ret;
}
EXPORT_SYMBOL_GPL(pcu_is_watching);

/*
 * If a holdout task is actually running, request an urgent quiescent
 * state from its CPU.  This is unsynchronized, so migrations can cause
 * the request to go to the wrong CPU.  Which is OK, all that will happen
 * is that the CPU's next context switch will be a bit slower and next
 * time around this task will generate another request.
 */
void pcu_request_urgent_qs_task(struct task_struct *t)
{
	int cpu;

	barrier();
	cpu = task_cpu(t);
	if (!task_curr(t))
		return; /* This task is not running on that CPU. */
	smp_store_release(per_cpu_ptr(&pcu_data.pcu_urgent_qs, cpu), true);
}

#if defined(CONFIG_PROVE_PCU) && defined(CONFIG_HOTPLUG_CPU)

/*
 * Is the current CPU online as far as PCU is concerned?
 *
 * Disable preemption to avoid false positives that could otherwise
 * happen due to the current CPU number being sampled, this task being
 * preempted, its old CPU being taken offline, resuming on some other CPU,
 * then determining that its old CPU is now offline.
 *
 * Disable checking if in an NMI handler because we cannot safely
 * report errors from NMI handlers anyway.  In addition, it is OK to use
 * PCU on an offline processor during initial boot, hence the check for
 * pcu_scheduler_fully_active.
 */
bool pcu_lockdep_current_cpu_online(void)
{
	struct pcu_data *rdp;
	struct pcu_node *rnp;
	bool ret = false;

	if (in_nmi() || !pcu_scheduler_fully_active)
		return true;
	preempt_disable_notrace();
	rdp = this_cpu_ptr(&pcu_data);
	rnp = rdp->mynode;
	if (rdp->grpmask & pcu_rnp_online_cpus(rnp) || READ_ONCE(rnp->ofl_seq) & 0x1)
		ret = true;
	preempt_enable_notrace();
	return ret;
}
EXPORT_SYMBOL_GPL(pcu_lockdep_current_cpu_online);

#endif /* #if defined(CONFIG_PROVE_PCU) && defined(CONFIG_HOTPLUG_CPU) */

/*
 * When trying to report a quiescent state on behalf of some other CPU,
 * it is our responsibility to check for and handle potential overflow
 * of the pcu_node ->gp_seq counter with respect to the pcu_data counters.
 * After all, the CPU might be in deep idle state, and thus executing no
 * code whatsoever.
 */
static void pcu_gpnum_ovf(struct pcu_node *rnp, struct pcu_data *rdp)
{
	raw_lockdep_assert_held_pcu_node(rnp);
	if (ULONG_CMP_LT(pcu_seq_current(&rdp->gp_seq) + ULONG_MAX / 4,
			 rnp->gp_seq))
		WRITE_ONCE(rdp->gpwrap, true);
	if (ULONG_CMP_LT(rdp->pcu_iw_gp_seq + ULONG_MAX / 4, rnp->gp_seq))
		rdp->pcu_iw_gp_seq = rnp->gp_seq + ULONG_MAX / 4;
}

/*
 * Snapshot the specified CPU's dynticks counter so that we can later
 * credit them with an implicit quiescent state.  Return 1 if this CPU
 * is in dynticks idle mode, which is an extended quiescent state.
 */
static int dyntick_save_progress_counter(struct pcu_data *rdp)
{
	rdp->dynticks_snap = pcu_dynticks_snap(rdp);
	if (pcu_dynticks_in_eqs(rdp->dynticks_snap)) {
		//trace_pcu_fqs(pcu_state.name, rdp->gp_seq, rdp->cpu, TPS("dti"));
		pcu_gpnum_ovf(rdp->mynode, rdp);
		return 1;
	}
	return 0;
}

/*
 * Return true if the specified CPU has passed through a quiescent
 * state by virtue of being in or having passed through an dynticks
 * idle state since the last call to dyntick_save_progress_counter()
 * for this same CPU, or by virtue of having been offline.
 */
static int pcu_implicit_dynticks_qs(struct pcu_data *rdp)
{
	unsigned long jtsq;
	bool *rnhqp;
	bool *ruqp;
	struct pcu_node *rnp = rdp->mynode;

	/*
	 * If the CPU passed through or entered a dynticks idle phase with
	 * no active irq/NMI handlers, then we can safely pretend that the CPU
	 * already acknowledged the request to pass through a quiescent
	 * state.  Either way, that CPU cannot possibly be in an PCU
	 * read-side critical section that started before the beginning
	 * of the current PCU grace period.
	 */
	if (pcu_dynticks_in_eqs_since(rdp, rdp->dynticks_snap)) {
		//trace_pcu_fqs(pcu_state.name, rdp->gp_seq, rdp->cpu, TPS("dti"));
		pcu_gpnum_ovf(rnp, rdp);
		return 1;
	}

	/*
	 * Complain if a CPU that is considered to be offline from PCU's
	 * perspective has not yet reported a quiescent state.  After all,
	 * the offline CPU should have reported a quiescent state during
	 * the CPU-offline process, or, failing that, by pcu_gp_init()
	 * if it ran concurrently with either the CPU going offline or the
	 * last task on a leaf pcu_node structure exiting its PCU read-side
	 * critical section while all CPUs corresponding to that structure
	 * are offline.  This added warning detects bugs in any of these
	 * code paths.
	 *
	 * The pcu_node structure's ->lock is held here, which excludes
	 * the relevant portions the CPU-hotplug code, the grace-period
	 * initialization code, and the pcu_read_unlock() code paths.
	 *
	 * For more detail, please refer to the "Hotplug CPU" section
	 * of PCU's Requirements documentation.
	 */
	if (WARN_ON_ONCE(!(rdp->grpmask & pcu_rnp_online_cpus(rnp)))) {
		bool onl;
		struct pcu_node *rnp1;

		pr_info("%s: grp: %d-%d level: %d ->gp_seq %ld ->completedqs %ld\n",
			__func__, rnp->grplo, rnp->grphi, rnp->level,
			(long)rnp->gp_seq, (long)rnp->completedqs);
		for (rnp1 = rnp; rnp1; rnp1 = rnp1->parent)
			pr_info("%s: %d:%d ->qsmask %#lx ->qsmaskinit %#lx ->qsmaskinitnext %#lx ->pcu_gp_init_mask %#lx\n",
				__func__, rnp1->grplo, rnp1->grphi, rnp1->qsmask, rnp1->qsmaskinit, rnp1->qsmaskinitnext, rnp1->pcu_gp_init_mask);
		onl = !!(rdp->grpmask & pcu_rnp_online_cpus(rnp));
		pr_info("%s %d: %c online: %ld(%d) offline: %ld(%d)\n",
			__func__, rdp->cpu, ".o"[onl],
			(long)rdp->pcu_onl_gp_seq, rdp->pcu_onl_gp_flags,
			(long)rdp->pcu_ofl_gp_seq, rdp->pcu_ofl_gp_flags);
		return 1; /* Break things loose after complaining. */
	}

	/*
	 * A CPU running for an extended time within the kernel can
	 * delay PCU grace periods: (1) At age jiffies_to_sched_qs,
	 * set .pcu_urgent_qs, (2) At age 2*jiffies_to_sched_qs, set
	 * both .pcu_need_heavy_qs and .pcu_urgent_qs.  Note that the
	 * unsynchronized assignments to the per-CPU pcu_need_heavy_qs
	 * variable are safe because the assignments are repeated if this
	 * CPU failed to pass through a quiescent state.  This code
	 * also checks .jiffies_resched in case jiffies_to_sched_qs
	 * is set way high.
	 */
	jtsq = READ_ONCE(jiffies_to_sched_qs);
	ruqp = per_cpu_ptr(&pcu_data.pcu_urgent_qs, rdp->cpu);
	rnhqp = &per_cpu(pcu_data.pcu_need_heavy_qs, rdp->cpu);
	if (!READ_ONCE(*rnhqp) &&
	    (time_after(jiffies, pcu_state.gp_start + jtsq * 2) ||
	     time_after(jiffies, pcu_state.jiffies_resched) ||
	     pcu_state.cbovld)) {
		WRITE_ONCE(*rnhqp, true);
		/* Store pcu_need_heavy_qs before pcu_urgent_qs. */
		smp_store_release(ruqp, true);
	} else if (time_after(jiffies, pcu_state.gp_start + jtsq)) {
		WRITE_ONCE(*ruqp, true);
	}

	/*
	 * NO_HZ_FULL CPUs can run in-kernel without pcu_sched_clock_irq!
	 * The above code handles this, but only for straight cond_resched().
	 * And some in-kernel loops check need_resched() before calling
	 * cond_resched(), which defeats the above code for CPUs that are
	 * running in-kernel with scheduling-clock interrupts disabled.
	 * So hit them over the head with the resched_cpu() hammer!
	 */
	if (tick_nohz_full_cpu(rdp->cpu) &&
	    (time_after(jiffies, READ_ONCE(rdp->last_fqs_resched) + jtsq * 3) ||
	     pcu_state.cbovld)) {
		WRITE_ONCE(*ruqp, true);
		resched_cpu(rdp->cpu);
		WRITE_ONCE(rdp->last_fqs_resched, jiffies);
	}

	/*
	 * If more than halfway to PCU CPU stall-warning time, invoke
	 * resched_cpu() more frequently to try to loosen things up a bit.
	 * Also check to see if the CPU is getting hammered with interrupts,
	 * but only once per grace period, just to keep the IPIs down to
	 * a dull roar.
	 */
	if (time_after(jiffies, pcu_state.jiffies_resched)) {
		if (time_after(jiffies,
			       READ_ONCE(rdp->last_fqs_resched) + jtsq)) {
			resched_cpu(rdp->cpu);
			WRITE_ONCE(rdp->last_fqs_resched, jiffies);
		}
		if (IS_ENABLED(CONFIG_IRQ_WORK) &&
		    !rdp->pcu_iw_pending && rdp->pcu_iw_gp_seq != rnp->gp_seq &&
		    (rnp->ffmask & rdp->grpmask)) {
			rdp->pcu_iw_pending = true;
			rdp->pcu_iw_gp_seq = rnp->gp_seq;
			irq_work_queue_on(&rdp->pcu_iw, rdp->cpu);
		}
	}

	return 0;
}

/* Trace-event wrapper function for trace_pcu_future_grace_period.  */
static void trace_pcu_this_gp(struct pcu_node *rnp, struct pcu_data *rdp,
			      unsigned long gp_seq_req, const char *s)
{
	//trace_pcu_future_grace_period(pcu_state.name, READ_ONCE(rnp->gp_seq),
	//			      gp_seq_req, rnp->level,
	//			      rnp->grplo, rnp->grphi, s);
}

/*
 * pcu_start_this_gp - Request the start of a particular grace period
 * @rnp_start: The leaf node of the CPU from which to start.
 * @rdp: The pcu_data corresponding to the CPU from which to start.
 * @gp_seq_req: The gp_seq of the grace period to start.
 *
 * Start the specified grace period, as needed to handle newly arrived
 * callbacks.  The required future grace periods are recorded in each
 * pcu_node structure's ->gp_seq_needed field.  Returns true if there
 * is reason to awaken the grace-period kthread.
 *
 * The caller must hold the specified pcu_node structure's ->lock, which
 * is why the caller is responsible for waking the grace-period kthread.
 *
 * Returns true if the GP thread needs to be awakened else false.
 */
static bool pcu_start_this_gp(struct pcu_node *rnp_start, struct pcu_data *rdp,
			      unsigned long gp_seq_req)
{
	bool ret = false;
	struct pcu_node *rnp;
	printk("[%s] ych_1, from %ps\n", __func__, __builtin_return_address(0));

	/*
	 * Use funnel locking to either acquire the root pcu_node
	 * structure's lock or bail out if the need for this grace period
	 * has already been recorded -- or if that grace period has in
	 * fact already started.  If there is already a grace period in
	 * progress in a non-leaf node, no recording is needed because the
	 * end of the grace period will scan the leaf pcu_node structures.
	 * Note that rnp_start->lock must not be released.
	 */
	raw_lockdep_assert_held_pcu_node(rnp_start);
	//trace_pcu_this_gp(rnp_start, rdp, gp_seq_req, TPS("Startleaf"));
	for (rnp = rnp_start; 1; rnp = rnp->parent) {
		if (rnp != rnp_start)
			raw_spin_lock_pcu_node(rnp);
		if (ULONG_CMP_GE(rnp->gp_seq_needed, gp_seq_req) ||
		    pcu_seq_started(&rnp->gp_seq, gp_seq_req) ||
		    (rnp != rnp_start &&
		     pcu_seq_state(pcu_seq_current(&rnp->gp_seq)))) {
			//trace_pcu_this_gp(rnp, rdp, gp_seq_req,
			//		  TPS("Prestarted"));
			goto unlock_out;
		}
		WRITE_ONCE(rnp->gp_seq_needed, gp_seq_req);
		if (pcu_seq_state(pcu_seq_current(&rnp->gp_seq))) {
			/*
			 * We just marked the leaf or internal node, and a
			 * grace period is in progress, which means that
			 * pcu_gp_cleanup() will see the marking.  Bail to
			 * reduce contention.
			 */
			//trace_pcu_this_gp(rnp_start, rdp, gp_seq_req,
			//		  TPS("Startedleaf"));
			goto unlock_out;
		}
		if (rnp != rnp_start && rnp->parent != NULL)
			raw_spin_unlock_pcu_node(rnp);
		if (!rnp->parent)
			break;  /* At root, and perhaps also leaf. */
	}

	/* If GP already in progress, just leave, otherwise start one. */
	if (pcu_gp_in_progress()) {
		//trace_pcu_this_gp(rnp, rdp, gp_seq_req, TPS("Startedleafroot"));
		goto unlock_out;
	}
	//trace_pcu_this_gp(rnp, rdp, gp_seq_req, TPS("Startedroot"));
	WRITE_ONCE(pcu_state.gp_flags, pcu_state.gp_flags | PCU_GP_FLAG_INIT);
	WRITE_ONCE(pcu_state.gp_req_activity, jiffies);
	if (!READ_ONCE(pcu_state.gp_kthread)) {
		//trace_pcu_this_gp(rnp, rdp, gp_seq_req, TPS("NoGPkthread"));
		goto unlock_out;
	}
	//trace_pcu_grace_period(pcu_state.name, data_race(pcu_state.gp_seq), TPS("newreq"));
	ret = true;  /* Caller must wake GP kthread. */
unlock_out:
	/* Push furthest requested GP to leaf node and pcu_data structure. */
	if (ULONG_CMP_LT(gp_seq_req, rnp->gp_seq_needed)) {
		WRITE_ONCE(rnp_start->gp_seq_needed, rnp->gp_seq_needed);
		WRITE_ONCE(rdp->gp_seq_needed, rnp->gp_seq_needed);
	}
	if (rnp != rnp_start)
		raw_spin_unlock_pcu_node(rnp);
	return ret;
}

/*
 * Clean up any old requests for the just-ended grace period.  Also return
 * whether any additional grace periods have been requested.
 */
static bool pcu_future_gp_cleanup(struct pcu_node *rnp)
{
	bool needmore;
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);

	needmore = ULONG_CMP_LT(rnp->gp_seq, rnp->gp_seq_needed);
	if (!needmore)
		rnp->gp_seq_needed = rnp->gp_seq; /* Avoid counter wrap. */
	//trace_pcu_this_gp(rnp, rdp, rnp->gp_seq,
	//		  needmore ? TPS("CleanupMore") : TPS("Cleanup"));
	return needmore;
}

/*
 * Awaken the grace-period kthread.  Don't do a self-awaken (unless in an
 * interrupt or softirq handler, in which case we just might immediately
 * sleep upon return, resulting in a grace-period hang), and don't bother
 * awakening when there is nothing for the grace-period kthread to do
 * (as in several CPUs raced to awaken, we lost), and finally don't try
 * to awaken a kthread that has not yet been created.  If all those checks
 * are passed, track some debug information and awaken.
 *
 * So why do the self-wakeup when in an interrupt or softirq handler
 * in the grace-period kthread's context?  Because the kthread might have
 * been interrupted just as it was going to sleep, and just after the final
 * pre-sleep check of the awaken condition.  In this case, a wakeup really
 * is required, and is therefore supplied.
 */
static void pcu_gp_kthread_wake(void)
{
	struct task_struct *t = READ_ONCE(pcu_state.gp_kthread);

	if ((current == t && !in_irq() && !in_serving_softirq()) ||
	    !READ_ONCE(pcu_state.gp_flags) || !t)
		return;
	WRITE_ONCE(pcu_state.gp_wake_time, jiffies);
	WRITE_ONCE(pcu_state.gp_wake_seq, READ_ONCE(pcu_state.gp_seq));
	swake_up_one(&pcu_state.gp_wq);
}

/*
 * If there is room, assign a ->gp_seq number to any callbacks on this
 * CPU that have not already been assigned.  Also accelerate any callbacks
 * that were previously assigned a ->gp_seq number that has since proven
 * to be too conservative, which can happen if callbacks get assigned a
 * ->gp_seq number while PCU is idle, but with reference to a non-root
 * pcu_node structure.  This function is idempotent, so it does not hurt
 * to call it repeatedly.  Returns an flag saying that we should awaken
 * the PCU grace-period kthread.
 *
 * The caller must hold rnp->lock with interrupts disabled.
 */
static bool pcu_accelerate_cbs(struct pcu_node *rnp, struct pcu_data *rdp)
{
	unsigned long gp_seq_req;
	bool ret = false;
	printk("[%s] ych_1, from %ps\n", __func__, __builtin_return_address(0));

	pcu_lockdep_assert_cblist_protected(rdp);
	raw_lockdep_assert_held_pcu_node(rnp);

	/* If no pending (not yet ready to invoke) callbacks, nothing to do. */
	if (!pcu_segcblist_pend_cbs(&rdp->cblist))
		return false;

	//trace_pcu_segcb_stats(&rdp->cblist, TPS("SegCbPreAcc"));

	/*
	 * Callbacks are often registered with incomplete grace-period
	 * information.  Something about the fact that getting exact
	 * information requires acquiring a global lock...  PCU therefore
	 * makes a conservative estimate of the grace period number at which
	 * a given callback will become ready to invoke.	The following
	 * code checks this estimate and improves it when possible, thus
	 * accelerating callback invocation to an earlier grace-period
	 * number.
	 */
	gp_seq_req = pcu_seq_snap(&pcu_state.gp_seq);
	if (pcu_segcblist_accelerate(&rdp->cblist, gp_seq_req))
		ret = pcu_start_this_gp(rnp, rdp, gp_seq_req);

	/* Trace depending on how much we were able to accelerate. */
	//if (pcu_segcblist_restempty(&rdp->cblist, PCU_WAIT_TAIL))
		//trace_pcu_grace_period(pcu_state.name, gp_seq_req, TPS("AccWaitCB"));
	//else
		//trace_pcu_grace_period(pcu_state.name, gp_seq_req, TPS("AccReadyCB"));

	//trace_pcu_segcb_stats(&rdp->cblist, TPS("SegCbPostAcc"));

	return ret;
}

/*
 * Similar to pcu_accelerate_cbs(), but does not require that the leaf
 * pcu_node structure's ->lock be held.  It consults the cached value
 * of ->gp_seq_needed in the pcu_data structure, and if that indicates
 * that a new grace-period request be made, invokes pcu_accelerate_cbs()
 * while holding the leaf pcu_node structure's ->lock.
 */
static void pcu_accelerate_cbs_unlocked(struct pcu_node *rnp,
					struct pcu_data *rdp)
{
	unsigned long c;
	bool needwake;
	printk("[%s] ych_1\n", __func__);

	pcu_lockdep_assert_cblist_protected(rdp);
	c = pcu_seq_snap(&pcu_state.gp_seq);
	if (!READ_ONCE(rdp->gpwrap) && ULONG_CMP_GE(rdp->gp_seq_needed, c)) {
		/* Old request still live, so mark recent callbacks. */
		(void)pcu_segcblist_accelerate(&rdp->cblist, c);
		return;
	}
	raw_spin_lock_pcu_node(rnp); /* irqs already disabled. */
	needwake = pcu_accelerate_cbs(rnp, rdp);
	raw_spin_unlock_pcu_node(rnp); /* irqs remain disabled. */
	if (needwake)
		pcu_gp_kthread_wake();
}

/*
 * Move any callbacks whose grace period has completed to the
 * PCU_DONE_TAIL sublist, then compact the remaining sublists and
 * assign ->gp_seq numbers to any callbacks in the PCU_NEXT_TAIL
 * sublist.  This function is idempotent, so it does not hurt to
 * invoke it repeatedly.  As long as it is not invoked -too- often...
 * Returns true if the PCU grace-period kthread needs to be awakened.
 *
 * The caller must hold rnp->lock with interrupts disabled.
 */
static bool pcu_advance_cbs(struct pcu_node *rnp, struct pcu_data *rdp)
{
	pcu_lockdep_assert_cblist_protected(rdp);
	raw_lockdep_assert_held_pcu_node(rnp);

	/* If no pending (not yet ready to invoke) callbacks, nothing to do. */
	if (!pcu_segcblist_pend_cbs(&rdp->cblist))
		return false;

	/*
	 * Find all callbacks whose ->gp_seq numbers indicate that they
	 * are ready to invoke, and put them into the PCU_DONE_TAIL sublist.
	 */
	pcu_segcblist_advance(&rdp->cblist, rnp->gp_seq);

	/* Classify any remaining callbacks. */
	return pcu_accelerate_cbs(rnp, rdp);
}

/*
 * Move and classify callbacks, but only if doing so won't require
 * that the PCU grace-period kthread be awakened.
 */
static void __maybe_unused pcu_advance_cbs_nowake(struct pcu_node *rnp,
						  struct pcu_data *rdp)
{
	pcu_lockdep_assert_cblist_protected(rdp);
	if (!pcu_seq_state(pcu_seq_current(&rnp->gp_seq)) || !raw_spin_trylock_pcu_node(rnp))
		return;
	// The grace period cannot end while we hold the pcu_node lock.
	if (pcu_seq_state(pcu_seq_current(&rnp->gp_seq)))
		WARN_ON_ONCE(pcu_advance_cbs(rnp, rdp));
	raw_spin_unlock_pcu_node(rnp);
}

/*
 * In CONFIG_PCU_STRICT_GRACE_PERIOD=y kernels, attempt to generate a
 * quiescent state.  This is intended to be invoked when the CPU notices
 * a new grace period.
 */
static void pcu_strict_gp_check_qs(void)
{
	if (IS_ENABLED(CONFIG_PCU_STRICT_GRACE_PERIOD)) {
		pcu_read_lock();
		pcu_read_unlock();
	}
}

/*
 * Update CPU-local pcu_data state to record the beginnings and ends of
 * grace periods.  The caller must hold the ->lock of the leaf pcu_node
 * structure corresponding to the current CPU, and must have irqs disabled.
 * Returns true if the grace-period kthread needs to be awakened.
 */
static bool __note_gp_changes(struct pcu_node *rnp, struct pcu_data *rdp)
{
	bool ret = false;
	bool need_qs;
	const bool offloaded = pcu_rdp_is_offloaded(rdp);

	raw_lockdep_assert_held_pcu_node(rnp);

	if (rdp->gp_seq == rnp->gp_seq)
		return false; /* Nothing to do. */

	/* Handle the ends of any preceding grace periods first. */
	if (pcu_seq_completed_gp(rdp->gp_seq, rnp->gp_seq) ||
	    unlikely(READ_ONCE(rdp->gpwrap))) {
		if (!offloaded)
			ret = pcu_advance_cbs(rnp, rdp); /* Advance CBs. */
		rdp->core_needs_qs = false;
		//trace_pcu_grace_period(pcu_state.name, rdp->gp_seq, TPS("cpuend"));
	} else {
		if (!offloaded)
			ret = pcu_accelerate_cbs(rnp, rdp); /* Recent CBs. */
		if (rdp->core_needs_qs)
			rdp->core_needs_qs = !!(rnp->qsmask & rdp->grpmask);
	}

	/* Now handle the beginnings of any new-to-this-CPU grace periods. */
	if (pcu_seq_new_gp(rdp->gp_seq, rnp->gp_seq) ||
	    unlikely(READ_ONCE(rdp->gpwrap))) {
		/*
		 * If the current grace period is waiting for this CPU,
		 * set up to detect a quiescent state, otherwise don't
		 * go looking for one.
		 */
		//trace_pcu_grace_period(pcu_state.name, rnp->gp_seq, TPS("cpustart"));
		need_qs = !!(rnp->qsmask & rdp->grpmask);
		rdp->cpu_no_qs.b.norm = need_qs;
		rdp->core_needs_qs = need_qs;
		zero_cpu_stall_ticks(rdp);
	}
	rdp->gp_seq = rnp->gp_seq;  /* Remember new grace-period state. */
	if (ULONG_CMP_LT(rdp->gp_seq_needed, rnp->gp_seq_needed) || rdp->gpwrap)
		WRITE_ONCE(rdp->gp_seq_needed, rnp->gp_seq_needed);
	WRITE_ONCE(rdp->gpwrap, false);
	pcu_gpnum_ovf(rnp, rdp);
	return ret;
}

static void note_gp_changes(struct pcu_data *rdp)
{
	unsigned long flags;
	bool needwake;
	struct pcu_node *rnp;

	local_irq_save(flags);
	rnp = rdp->mynode;
	if ((rdp->gp_seq == pcu_seq_current(&rnp->gp_seq) &&
	     !unlikely(READ_ONCE(rdp->gpwrap))) || /* w/out lock. */
	    !raw_spin_trylock_pcu_node(rnp)) { /* irqs already off, so later. */
		local_irq_restore(flags);
		return;
	}
	needwake = __note_gp_changes(rnp, rdp);
	raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
	pcu_strict_gp_check_qs();
	if (needwake)
		pcu_gp_kthread_wake();
}

static void pcu_gp_slow(int delay)
{
	if (delay > 0 &&
	    !(pcu_seq_ctr(pcu_state.gp_seq) %
	      (pcu_num_nodes * PER_PCU_NODE_PERIOD * delay)))
		schedule_timeout_idle(delay);
}

static unsigned long sleep_duration;

/* Allow pcutorture to stall the grace-period kthread. */
void pcu_gp_set_torture_wait(int duration)
{
	if (IS_ENABLED(CONFIG_PCU_TORTURE_TEST) && duration > 0)
		WRITE_ONCE(sleep_duration, duration);
}
EXPORT_SYMBOL_GPL(pcu_gp_set_torture_wait);

/* Actually implement the aforementioned wait. */
static void pcu_gp_torture_wait(void)
{
	unsigned long duration;

	if (!IS_ENABLED(CONFIG_PCU_TORTURE_TEST))
		return;
	duration = xchg(&sleep_duration, 0UL);
	if (duration > 0) {
		pr_alert("%s: Waiting %lu jiffies\n", __func__, duration);
		schedule_timeout_idle(duration);
		pr_alert("%s: Wait complete\n", __func__);
	}
}

/*
 * Handler for on_each_cpu() to invoke the target CPU's PCU core
 * processing.
 */
static void pcu_strict_gp_boundary(void *unused)
{
	invoke_pcu_core();
}

/*
 * Initialize a new grace period.  Return false if no grace period required.
 */
static bool pcu_gp_init(void)
{
	unsigned long firstseq;
	unsigned long flags;
	unsigned long oldmask;
	unsigned long mask;
	struct pcu_data *rdp;
	struct pcu_node *rnp = pcu_get_root();
	printk("[%s] ych_1\n", __func__);

	WRITE_ONCE(pcu_state.gp_activity, jiffies);
	raw_spin_lock_irq_pcu_node(rnp);
	if (!READ_ONCE(pcu_state.gp_flags)) {
		/* Spurious wakeup, tell caller to go back to sleep.  */
		raw_spin_unlock_irq_pcu_node(rnp);
		return false;
	}
	WRITE_ONCE(pcu_state.gp_flags, 0); /* Clear all flags: New GP. */

	if (WARN_ON_ONCE(pcu_gp_in_progress())) {
		/*
		 * Grace period already in progress, don't start another.
		 * Not supposed to be able to happen.
		 */
		raw_spin_unlock_irq_pcu_node(rnp);
		return false;
	}

	/* Advance to a new grace period and initialize state. */
	record_gp_stall_check_time();
	/* Record GP times before starting GP, hence pcu_seq_start(). */
	pcu_seq_start(&pcu_state.gp_seq);
	ASSERT_EXCLUSIVE_WRITER(pcu_state.gp_seq);
	//trace_pcu_grace_period(pcu_state.name, pcu_state.gp_seq, TPS("start"));
	raw_spin_unlock_irq_pcu_node(rnp);

	/*
	 * Apply per-leaf buffered online and offline operations to
	 * the pcu_node tree. Note that this new grace period need not
	 * wait for subsequent online CPUs, and that PCU hooks in the CPU
	 * offlining path, when combined with checks in this function,
	 * will handle CPUs that are currently going offline or that will
	 * go offline later.  Please also refer to "Hotplug CPU" section
	 * of PCU's Requirements documentation.
	 */
	WRITE_ONCE(pcu_state.gp_state, PCU_GP_ONOFF);
	pcu_for_each_leaf_node(rnp) {
		smp_mb(); // Pair with barriers used when updating ->ofl_seq to odd values.
		firstseq = READ_ONCE(rnp->ofl_seq);
		if (firstseq & 0x1)
			while (firstseq == READ_ONCE(rnp->ofl_seq))
				schedule_timeout_idle(1);  // Can't wake unless PCU is watching.
		smp_mb(); // Pair with barriers used when updating ->ofl_seq to even values.
		raw_spin_lock(&pcu_state.ofl_lock);
		raw_spin_lock_irq_pcu_node(rnp);
		if (rnp->qsmaskinit == rnp->qsmaskinitnext &&
		    !rnp->wait_blkd_tasks) {
			/* Nothing to do on this leaf pcu_node structure. */
			raw_spin_unlock_irq_pcu_node(rnp);
			raw_spin_unlock(&pcu_state.ofl_lock);
			continue;
		}

		/* Record old state, apply changes to ->qsmaskinit field. */
		oldmask = rnp->qsmaskinit;
		rnp->qsmaskinit = rnp->qsmaskinitnext;

		/* If zero-ness of ->qsmaskinit changed, propagate up tree. */
		if (!oldmask != !rnp->qsmaskinit) {
			if (!oldmask) { /* First online CPU for pcu_node. */
				if (!rnp->wait_blkd_tasks) /* Ever offline? */
					pcu_init_new_rnp(rnp);
			} else if (pcu_preempt_has_tasks(rnp)) {
				rnp->wait_blkd_tasks = true; /* blocked tasks */
			} else { /* Last offline CPU and can propagate. */
				pcu_cleanup_dead_rnp(rnp);
			}
		}

		/*
		 * If all waited-on tasks from prior grace period are
		 * done, and if all this pcu_node structure's CPUs are
		 * still offline, propagate up the pcu_node tree and
		 * clear ->wait_blkd_tasks.  Otherwise, if one of this
		 * pcu_node structure's CPUs has since come back online,
		 * simply clear ->wait_blkd_tasks.
		 */
		if (rnp->wait_blkd_tasks &&
		    (!pcu_preempt_has_tasks(rnp) || rnp->qsmaskinit)) {
			rnp->wait_blkd_tasks = false;
			if (!rnp->qsmaskinit)
				pcu_cleanup_dead_rnp(rnp);
		}

		raw_spin_unlock_irq_pcu_node(rnp);
		raw_spin_unlock(&pcu_state.ofl_lock);
	}
	pcu_gp_slow(gp_preinit_delay); /* Races with CPU hotplug. */

	/*
	 * Set the quiescent-state-needed bits in all the pcu_node
	 * structures for all currently online CPUs in breadth-first
	 * order, starting from the root pcu_node structure, relying on the
	 * layout of the tree within the pcu_state.node[] array.  Note that
	 * other CPUs will access only the leaves of the hierarchy, thus
	 * seeing that no grace period is in progress, at least until the
	 * corresponding leaf node has been initialized.
	 *
	 * The grace period cannot complete until the initialization
	 * process finishes, because this kthread handles both.
	 */
	WRITE_ONCE(pcu_state.gp_state, PCU_GP_INIT);
	pcu_for_each_node_breadth_first(rnp) {
		pcu_gp_slow(gp_init_delay);
		raw_spin_lock_irqsave_pcu_node(rnp, flags);
		rdp = this_cpu_ptr(&pcu_data);
		pcu_preempt_check_blocked_tasks(rnp);
		rnp->qsmask = rnp->qsmaskinit;
		WRITE_ONCE(rnp->gp_seq, pcu_state.gp_seq);
		if (rnp == rdp->mynode)
			(void)__note_gp_changes(rnp, rdp);
		pcu_preempt_boost_start_gp(rnp);
		//trace_pcu_grace_period_init(pcu_state.name, rnp->gp_seq,
		//			    rnp->level, rnp->grplo,
		//			    rnp->grphi, rnp->qsmask);
		/* Quiescent states for tasks on any now-offline CPUs. */
		mask = rnp->qsmask & ~rnp->qsmaskinitnext;
		rnp->pcu_gp_init_mask = mask;
		if ((mask || rnp->wait_blkd_tasks) && pcu_is_leaf_node(rnp))
			pcu_report_qs_rnp(mask, rnp, rnp->gp_seq, flags);
		else
			raw_spin_unlock_irq_pcu_node(rnp);
		cond_resched_tasks_pcu_qs();
		WRITE_ONCE(pcu_state.gp_activity, jiffies);
	}

	// If strict, make all CPUs aware of new grace period.
	if (IS_ENABLED(CONFIG_PCU_STRICT_GRACE_PERIOD))
		on_each_cpu(pcu_strict_gp_boundary, NULL, 0);

	return true;
}

/*
 * Helper function for swait_event_idle_exclusive() wakeup at force-quiescent-state
 * time.
 */
static bool pcu_gp_fqs_check_wake(int *gfp)
{
	struct pcu_node *rnp = pcu_get_root();

	// If under overload conditions, force an immediate FQS scan.
	if (*gfp & PCU_GP_FLAG_OVLD)
		return true;

	// Someone like call_pcu() requested a force-quiescent-state scan.
	*gfp = READ_ONCE(pcu_state.gp_flags);
	if (*gfp & PCU_GP_FLAG_FQS)
		return true;

	// The current grace period has completed.
	if (!READ_ONCE(rnp->qsmask) && !pcu_preempt_blocked_readers_cgp(rnp))
		return true;

	return false;
}

/*
 * Do one round of quiescent-state forcing.
 */
static void pcu_gp_fqs(bool first_time)
{
	struct pcu_node *rnp = pcu_get_root();

	WRITE_ONCE(pcu_state.gp_activity, jiffies);
	pcu_state.n_force_qs++;
	if (first_time) {
		/* Collect dyntick-idle snapshots. */
		force_qs_rnp(dyntick_save_progress_counter);
	} else {
		/* Handle dyntick-idle and offline CPUs. */
		force_qs_rnp(pcu_implicit_dynticks_qs);
	}
	/* Clear flag to prevent immediate re-entry. */
	if (READ_ONCE(pcu_state.gp_flags) & PCU_GP_FLAG_FQS) {
		raw_spin_lock_irq_pcu_node(rnp);
		WRITE_ONCE(pcu_state.gp_flags,
			   READ_ONCE(pcu_state.gp_flags) & ~PCU_GP_FLAG_FQS);
		raw_spin_unlock_irq_pcu_node(rnp);
	}
}

/*
 * Loop doing repeated quiescent-state forcing until the grace period ends.
 */
static void pcu_gp_fqs_loop(void)
{
	bool first_gp_fqs;
	int gf = 0;
	unsigned long j;
	int ret;
	struct pcu_node *rnp = pcu_get_root();

	first_gp_fqs = true;
	j = READ_ONCE(jiffies_till_first_fqs);
	if (pcu_state.cbovld)
		gf = PCU_GP_FLAG_OVLD;
	ret = 0;
	for (;;) {
		if (!ret) {
			WRITE_ONCE(pcu_state.jiffies_force_qs, jiffies + j);
			/*
			 * jiffies_force_qs before PCU_GP_WAIT_FQS state
			 * update; required for stall checks.
			 */
			smp_wmb();
			WRITE_ONCE(pcu_state.jiffies_kick_kthreads,
				   jiffies + (j ? 3 * j : 2));
		}
		//trace_pcu_grace_period(pcu_state.name, pcu_state.gp_seq,
		//		       TPS("fqswait"));
		WRITE_ONCE(pcu_state.gp_state, PCU_GP_WAIT_FQS);
		ret = swait_event_idle_timeout_exclusive(
				pcu_state.gp_wq, pcu_gp_fqs_check_wake(&gf), j);
		pcu_gp_torture_wait();
		WRITE_ONCE(pcu_state.gp_state, PCU_GP_DOING_FQS);
		/* Locking provides needed memory barriers. */
		/* If grace period done, leave loop. */
		if (!READ_ONCE(rnp->qsmask) &&
		    !pcu_preempt_blocked_readers_cgp(rnp))
			break;
		/* If time for quiescent-state forcing, do it. */
		if (!time_after(pcu_state.jiffies_force_qs, jiffies) ||
		    (gf & (PCU_GP_FLAG_FQS | PCU_GP_FLAG_OVLD))) {
			//trace_pcu_grace_period(pcu_state.name, pcu_state.gp_seq,
			//		       TPS("fqsstart"));
			pcu_gp_fqs(first_gp_fqs);
			gf = 0;
			if (first_gp_fqs) {
				first_gp_fqs = false;
				gf = pcu_state.cbovld ? PCU_GP_FLAG_OVLD : 0;
			}
			//trace_pcu_grace_period(pcu_state.name, pcu_state.gp_seq,
			//		       TPS("fqsend"));
			cond_resched_tasks_pcu_qs();
			WRITE_ONCE(pcu_state.gp_activity, jiffies);
			ret = 0; /* Force full wait till next FQS. */
			j = READ_ONCE(jiffies_till_next_fqs);
		} else {
			/* Deal with stray signal. */
			cond_resched_tasks_pcu_qs();
			WRITE_ONCE(pcu_state.gp_activity, jiffies);
			WARN_ON(signal_pending(current));
			//trace_pcu_grace_period(pcu_state.name, pcu_state.gp_seq,
			//		       TPS("fqswaitsig"));
			ret = 1; /* Keep old FQS timing. */
			j = jiffies;
			if (time_after(jiffies, pcu_state.jiffies_force_qs))
				j = 1;
			else
				j = pcu_state.jiffies_force_qs - j;
			gf = 0;
		}
	}
}

/*
 * Clean up after the old grace period.
 */
static noinline void pcu_gp_cleanup(void)
{
	int cpu;
	bool needgp = false;
	unsigned long gp_duration;
	unsigned long new_gp_seq;
	bool offloaded;
	struct pcu_data *rdp;
	struct pcu_node *rnp = pcu_get_root();
	struct swait_queue_head *sq;

	WRITE_ONCE(pcu_state.gp_activity, jiffies);
	raw_spin_lock_irq_pcu_node(rnp);
	pcu_state.gp_end = jiffies;
	gp_duration = pcu_state.gp_end - pcu_state.gp_start;
	if (gp_duration > pcu_state.gp_max)
		pcu_state.gp_max = gp_duration;

	/*
	 * We know the grace period is complete, but to everyone else
	 * it appears to still be ongoing.  But it is also the case
	 * that to everyone else it looks like there is nothing that
	 * they can do to advance the grace period.  It is therefore
	 * safe for us to drop the lock in order to mark the grace
	 * period as completed in all of the pcu_node structures.
	 */
	raw_spin_unlock_irq_pcu_node(rnp);

	/*
	 * Propagate new ->gp_seq value to pcu_node structures so that
	 * other CPUs don't have to wait until the start of the next grace
	 * period to process their callbacks.  This also avoids some nasty
	 * PCU grace-period initialization races by forcing the end of
	 * the current grace period to be completely recorded in all of
	 * the pcu_node structures before the beginning of the next grace
	 * period is recorded in any of the pcu_node structures.
	 */
	new_gp_seq = pcu_state.gp_seq;
	pcu_seq_end(&new_gp_seq);
	pcu_for_each_node_breadth_first(rnp) {
		raw_spin_lock_irq_pcu_node(rnp);
		if (WARN_ON_ONCE(pcu_preempt_blocked_readers_cgp(rnp)))
			dump_blkd_tasks(rnp, 10);
		WARN_ON_ONCE(rnp->qsmask);
		WRITE_ONCE(rnp->gp_seq, new_gp_seq);
		rdp = this_cpu_ptr(&pcu_data);
		if (rnp == rdp->mynode)
			needgp = __note_gp_changes(rnp, rdp) || needgp;
		/* smp_mb() provided by prior unlock-lock pair. */
		needgp = pcu_future_gp_cleanup(rnp) || needgp;
		// Reset overload indication for CPUs no longer overloaded
		if (pcu_is_leaf_node(rnp))
			for_each_leaf_node_cpu_mask(rnp, cpu, rnp->cbovldmask) {
				rdp = per_cpu_ptr(&pcu_data, cpu);
				check_cb_ovld_locked(rdp, rnp);
			}
		sq = pcu_nocb_gp_get(rnp);
		raw_spin_unlock_irq_pcu_node(rnp);
		pcu_nocb_gp_cleanup(sq);
		cond_resched_tasks_pcu_qs();
		WRITE_ONCE(pcu_state.gp_activity, jiffies);
		pcu_gp_slow(gp_cleanup_delay);
	}
	rnp = pcu_get_root();
	raw_spin_lock_irq_pcu_node(rnp); /* GP before ->gp_seq update. */

	/* Declare grace period done, trace first to use old GP number. */
	//trace_pcu_grace_period(pcu_state.name, pcu_state.gp_seq, TPS("end"));
	pcu_seq_end(&pcu_state.gp_seq);
	ASSERT_EXCLUSIVE_WRITER(pcu_state.gp_seq);
	WRITE_ONCE(pcu_state.gp_state, PCU_GP_IDLE);
	/* Check for GP requests since above loop. */
	rdp = this_cpu_ptr(&pcu_data);
	if (!needgp && ULONG_CMP_LT(rnp->gp_seq, rnp->gp_seq_needed)) {
		//trace_pcu_this_gp(rnp, rdp, rnp->gp_seq_needed,
		//		  TPS("CleanupMore"));
		needgp = true;
	}
	/* Advance CBs to reduce false positives below. */
	offloaded = pcu_rdp_is_offloaded(rdp);
	if ((offloaded || !pcu_accelerate_cbs(rnp, rdp)) && needgp) {
		WRITE_ONCE(pcu_state.gp_flags, PCU_GP_FLAG_INIT);
		WRITE_ONCE(pcu_state.gp_req_activity, jiffies);
		//trace_pcu_grace_period(pcu_state.name,
		//		       pcu_state.gp_seq,
		//		       TPS("newreq"));
	} else {
		WRITE_ONCE(pcu_state.gp_flags,
			   pcu_state.gp_flags & PCU_GP_FLAG_INIT);
	}
	raw_spin_unlock_irq_pcu_node(rnp);

	// If strict, make all CPUs aware of the end of the old grace period.
	if (IS_ENABLED(CONFIG_PCU_STRICT_GRACE_PERIOD))
		on_each_cpu(pcu_strict_gp_boundary, NULL, 0);
}

/*
 * Body of kthread that handles grace periods.
 */
//static int __noreturn pcu_gp_kthread(void *unused)
static int pcu_gp_kthread(void *unused)
{
	pcu_bind_gp_kthread();
	
	while (!kthread_should_stop()) {
		/* Handle grace-period start. */
		while (!kthread_should_stop()) {
			//trace_pcu_grace_period(pcu_state.name, pcu_state.gp_seq,
			//		       TPS("reqwait"));
			WRITE_ONCE(pcu_state.gp_state, PCU_GP_WAIT_GPS);
			swait_event_idle_exclusive(pcu_state.gp_wq,
					kthread_should_stop() ||
					(READ_ONCE(pcu_state.gp_flags) &
					PCU_GP_FLAG_INIT));

			if (kthread_should_stop())
				goto out;

			pcu_gp_torture_wait();
			WRITE_ONCE(pcu_state.gp_state, PCU_GP_DONE_GPS);
			/* Locking provides needed memory barrier. */
			if (pcu_gp_init())
				break;

			cond_resched_tasks_pcu_qs();
			WRITE_ONCE(pcu_state.gp_activity, jiffies);

			WARN_ON(signal_pending(current));
			//trace_pcu_grace_period(pcu_state.name, pcu_state.gp_seq,
			//		       TPS("reqwaitsig"));
		}

		if (kthread_should_stop())
			break;
		/* Handle quiescent-state forcing. */
		pcu_gp_fqs_loop();
		
		if (kthread_should_stop())
			break;

		/* Handle grace-period end. */
		WRITE_ONCE(pcu_state.gp_state, PCU_GP_CLEANUP);
		pcu_gp_cleanup();
		WRITE_ONCE(pcu_state.gp_state, PCU_GP_CLEANED);
	}

out:
	WRITE_ONCE(pcu_state.gp_state, PCU_GP_IDLE);
	return 0;
}

/*
 * Report a full set of quiescent states to the pcu_state data structure.
 * Invoke pcu_gp_kthread_wake() to awaken the grace-period kthread if
 * another grace period is required.  Whether we wake the grace-period
 * kthread or it awakens itself for the next round of quiescent-state
 * forcing, that kthread will clean up after the just-completed grace
 * period.  Note that the caller must hold rnp->lock, which is released
 * before return.
 */
static void pcu_report_qs_rsp(unsigned long flags)
	__releases(pcu_get_root()->lock)
{
	raw_lockdep_assert_held_pcu_node(pcu_get_root());
	WARN_ON_ONCE(!pcu_gp_in_progress());
	WRITE_ONCE(pcu_state.gp_flags,
		   READ_ONCE(pcu_state.gp_flags) | PCU_GP_FLAG_FQS);
	raw_spin_unlock_irqrestore_pcu_node(pcu_get_root(), flags);
	pcu_gp_kthread_wake();
}

/*
 * Similar to pcu_report_qs_rdp(), for which it is a helper function.
 * Allows quiescent states for a group of CPUs to be reported at one go
 * to the specified pcu_node structure, though all the CPUs in the group
 * must be represented by the same pcu_node structure (which need not be a
 * leaf pcu_node structure, though it often will be).  The gps parameter
 * is the grace-period snapshot, which means that the quiescent states
 * are valid only if rnp->gp_seq is equal to gps.  That structure's lock
 * must be held upon entry, and it is released before return.
 *
 * As a special case, if mask is zero, the bit-already-cleared check is
 * disabled.  This allows propagating quiescent state due to resumed tasks
 * during grace-period initialization.
 */
static void pcu_report_qs_rnp(unsigned long mask, struct pcu_node *rnp,
			      unsigned long gps, unsigned long flags)
	__releases(rnp->lock)
{
	unsigned long oldmask = 0;
	struct pcu_node *rnp_c;

	raw_lockdep_assert_held_pcu_node(rnp);

	/* Walk up the pcu_node hierarchy. */
	for (;;) {
		if ((!(rnp->qsmask & mask) && mask) || rnp->gp_seq != gps) {

			/*
			 * Our bit has already been cleared, or the
			 * relevant grace period is already over, so done.
			 */
			raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
			return;
		}
		WARN_ON_ONCE(oldmask); /* Any child must be all zeroed! */
		WARN_ON_ONCE(!pcu_is_leaf_node(rnp) &&
			     pcu_preempt_blocked_readers_cgp(rnp));
		WRITE_ONCE(rnp->qsmask, rnp->qsmask & ~mask);
		//trace_pcu_quiescent_state_report(pcu_state.name, rnp->gp_seq,
		//				 mask, rnp->qsmask, rnp->level,
		//				 rnp->grplo, rnp->grphi,
		//				 !!rnp->gp_tasks);
		if (rnp->qsmask != 0 || pcu_preempt_blocked_readers_cgp(rnp)) {

			/* Other bits still set at this level, so done. */
			raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
			return;
		}
		rnp->completedqs = rnp->gp_seq;
		mask = rnp->grpmask;
		if (rnp->parent == NULL) {

			/* No more levels.  Exit loop holding root lock. */

			break;
		}
		raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
		rnp_c = rnp;
		rnp = rnp->parent;
		raw_spin_lock_irqsave_pcu_node(rnp, flags);
		oldmask = READ_ONCE(rnp_c->qsmask);
	}

	/*
	 * Get here if we are the last CPU to pass through a quiescent
	 * state for this grace period.  Invoke pcu_report_qs_rsp()
	 * to clean up and start the next grace period if one is needed.
	 */
	pcu_report_qs_rsp(flags); /* releases rnp->lock. */
}

/*
 * Record a quiescent state for all tasks that were previously queued
 * on the specified pcu_node structure and that were blocking the current
 * PCU grace period.  The caller must hold the corresponding rnp->lock with
 * irqs disabled, and this lock is released upon return, but irqs remain
 * disabled.
 */
static void __maybe_unused
pcu_report_unblock_qs_rnp(struct pcu_node *rnp, unsigned long flags)
	__releases(rnp->lock)
{
	unsigned long gps;
	unsigned long mask;
	struct pcu_node *rnp_p;

	raw_lockdep_assert_held_pcu_node(rnp);
	if (WARN_ON_ONCE(!IS_ENABLED(CONFIG_PREEMPT_PCU)) ||
	    WARN_ON_ONCE(pcu_preempt_blocked_readers_cgp(rnp)) ||
	    rnp->qsmask != 0) {
		raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
		return;  /* Still need more quiescent states! */
	}

	rnp->completedqs = rnp->gp_seq;
	rnp_p = rnp->parent;
	if (rnp_p == NULL) {
		/*
		 * Only one pcu_node structure in the tree, so don't
		 * try to report up to its nonexistent parent!
		 */
		pcu_report_qs_rsp(flags);
		return;
	}

	/* Report up the rest of the hierarchy, tracking current ->gp_seq. */
	gps = rnp->gp_seq;
	mask = rnp->grpmask;
	raw_spin_unlock_pcu_node(rnp);	/* irqs remain disabled. */
	raw_spin_lock_pcu_node(rnp_p);	/* irqs already disabled. */
	pcu_report_qs_rnp(mask, rnp_p, gps, flags);
}

/*
 * Record a quiescent state for the specified CPU to that CPU's pcu_data
 * structure.  This must be called from the specified CPU.
 */
static void
pcu_report_qs_rdp(struct pcu_data *rdp)
{
	unsigned long flags;
	unsigned long mask;
	bool needwake = false;
	struct pcu_node *rnp;

	WARN_ON_ONCE(rdp->cpu != smp_processor_id());
	rnp = rdp->mynode;
	raw_spin_lock_irqsave_pcu_node(rnp, flags);
	if (rdp->cpu_no_qs.b.norm || rdp->gp_seq != rnp->gp_seq ||
	    rdp->gpwrap) {

		/*
		 * The grace period in which this quiescent state was
		 * recorded has ended, so don't report it upwards.
		 * We will instead need a new quiescent state that lies
		 * within the current grace period.
		 */
		rdp->cpu_no_qs.b.norm = true;	/* need qs for new gp. */
		raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
		return;
	}
	mask = rdp->grpmask;
	rdp->core_needs_qs = false;
	if ((rnp->qsmask & mask) == 0) {
		raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
	} else {
		/*
		 * This GP can't end until cpu checks in, so all of our
		 * callbacks can be processed during the next GP.
		 *
		 * NOCB kthreads have their own way to deal with that.
		 */
		if (!pcu_rdp_is_offloaded(rdp))
			needwake = pcu_accelerate_cbs(rnp, rdp);

		pcu_disable_urgency_upon_qs(rdp);
		pcu_report_qs_rnp(mask, rnp, rnp->gp_seq, flags);
		/* ^^^ Released rnp->lock */
		if (needwake)
			pcu_gp_kthread_wake();
	}
}

/*
 * Check to see if there is a new grace period of which this CPU
 * is not yet aware, and if so, set up local pcu_data state for it.
 * Otherwise, see if this CPU has just passed through its first
 * quiescent state for this grace period, and record that fact if so.
 */
static void
pcu_check_quiescent_state(struct pcu_data *rdp)
{
	/* Check for grace-period ends and beginnings. */
	note_gp_changes(rdp);

	/*
	 * Does this CPU still need to do its part for current grace period?
	 * If no, return and let the other CPUs do their part as well.
	 */
	if (!rdp->core_needs_qs)
		return;

	/*
	 * Was there a quiescent state since the beginning of the grace
	 * period? If no, then exit and wait for the next call.
	 */
	if (rdp->cpu_no_qs.b.norm)
		return;

	/*
	 * Tell PCU we are done (but pcu_report_qs_rdp() will be the
	 * judge of that).
	 */
	pcu_report_qs_rdp(rdp);
}

/*
 * Near the end of the offline process.  Trace the fact that this CPU
 * is going offline.
 */
int pcutree_dying_cpu(unsigned int cpu)
{
	bool blkd;
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);
	struct pcu_node *rnp = rdp->mynode;

	if (!IS_ENABLED(CONFIG_HOTPLUG_CPU))
		return 0;

	blkd = !!(rnp->qsmask & rdp->grpmask);
	//trace_pcu_grace_period(pcu_state.name, READ_ONCE(rnp->gp_seq),
	//		       blkd ? TPS("cpuofl-bgp") : TPS("cpuofl"));
	return 0;
}

/*
 * All CPUs for the specified pcu_node structure have gone offline,
 * and all tasks that were preempted within an PCU read-side critical
 * section while running on one of those CPUs have since exited their PCU
 * read-side critical section.  Some other CPU is reporting this fact with
 * the specified pcu_node structure's ->lock held and interrupts disabled.
 * This function therefore goes up the tree of pcu_node structures,
 * clearing the corresponding bits in the ->qsmaskinit fields.  Note that
 * the leaf pcu_node structure's ->qsmaskinit field has already been
 * updated.
 *
 * This function does check that the specified pcu_node structure has
 * all CPUs offline and no blocked tasks, so it is OK to invoke it
 * prematurely.  That said, invoking it after the fact will cost you
 * a needless lock acquisition.  So once it has done its work, don't
 * invoke it again.
 */
static void pcu_cleanup_dead_rnp(struct pcu_node *rnp_leaf)
{
	long mask;
	struct pcu_node *rnp = rnp_leaf;

	raw_lockdep_assert_held_pcu_node(rnp_leaf);
	if (!IS_ENABLED(CONFIG_HOTPLUG_CPU) ||
	    WARN_ON_ONCE(rnp_leaf->qsmaskinit) ||
	    WARN_ON_ONCE(pcu_preempt_has_tasks(rnp_leaf)))
		return;
	for (;;) {
		mask = rnp->grpmask;
		rnp = rnp->parent;
		if (!rnp)
			break;
		raw_spin_lock_pcu_node(rnp); /* irqs already disabled. */
		rnp->qsmaskinit &= ~mask;
		/* Between grace periods, so better already be zero! */
		WARN_ON_ONCE(rnp->qsmask);
		if (rnp->qsmaskinit) {
			raw_spin_unlock_pcu_node(rnp);
			/* irqs remain disabled. */
			return;
		}
		raw_spin_unlock_pcu_node(rnp); /* irqs remain disabled. */
	}
}

/*
 * The CPU has been completely removed, and some other CPU is reporting
 * this fact from process context.  Do the remainder of the cleanup.
 * There can only be one CPU hotplug operation at a time, so no need for
 * explicit locking.
 */
int pcutree_dead_cpu(unsigned int cpu)
{
	struct pcu_data *rdp = per_cpu_ptr(&pcu_data, cpu);
	struct pcu_node *rnp = rdp->mynode;  /* Outgoing CPU's rdp & rnp. */

	if (!IS_ENABLED(CONFIG_HOTPLUG_CPU))
		return 0;

	WRITE_ONCE(pcu_state.n_online_cpus, pcu_state.n_online_cpus - 1);
	/* Adjust any no-longer-needed kthreads. */
	pcu_boost_kthread_setaffinity(rnp, -1);
	/* Do any needed no-CB deferred wakeups from this CPU. */
	do_nocb_deferred_wakeup(per_cpu_ptr(&pcu_data, cpu));

	// Stop-machine done, so allow nohz_full to disable tick.
	tick_dep_clear(TICK_DEP_BIT_RCU);
	return 0;
}

/*
 * Invoke any PCU callbacks that have made it to the end of their grace
 * period.  Thottle as specified by rdp->blimit.
 */
static void pcu_do_batch(struct pcu_data *rdp)
{
	int div;
	bool __maybe_unused empty;
	unsigned long flags;
	const bool offloaded = pcu_rdp_is_offloaded(rdp);
	struct pcu_head *rhp;
	struct pcu_cblist rcl = PCU_CBLIST_INITIALIZER(rcl);
	long bl, count = 0;
	long pending, tlimit = 0;

	/* If no callbacks are ready, just return. */
	if (!pcu_segcblist_ready_cbs(&rdp->cblist)) {
		//trace_pcu_batch_start(pcu_state.name,
		//		      pcu_segcblist_n_cbs(&rdp->cblist), 0);
		//trace_pcu_batch_end(pcu_state.name, 0,
		//		    !pcu_segcblist_empty(&rdp->cblist),
		//		    need_resched(), is_idle_task(current),
		//		    pcu_is_callbacks_kthread());
		return;
	}

	/*
	 * Extract the list of ready callbacks, disabling to prevent
	 * races with call_pcu() from interrupt handlers.  Leave the
	 * callback counts, as pcu_barrier() needs to be conservative.
	 */
	local_irq_save(flags);
	pcu_nocb_lock(rdp);
	WARN_ON_ONCE(cpu_is_offline(smp_processor_id()));
	pending = pcu_segcblist_n_cbs(&rdp->cblist);
	div = READ_ONCE(pcu_divisor);
	div = div < 0 ? 7 : div > sizeof(long) * 8 - 2 ? sizeof(long) * 8 - 2 : div;
	bl = max(rdp->blimit, pending >> div);
	if (unlikely(bl > 100)) {
		long rrn = READ_ONCE(pcu_resched_ns);

		rrn = rrn < NSEC_PER_MSEC ? NSEC_PER_MSEC : rrn > NSEC_PER_SEC ? NSEC_PER_SEC : rrn;
		tlimit = local_clock() + rrn;
	}
	//trace_pcu_batch_start(pcu_state.name,
	//		      pcu_segcblist_n_cbs(&rdp->cblist), bl);
	pcu_segcblist_extract_done_cbs(&rdp->cblist, &rcl);
	if (offloaded)
		rdp->qlen_last_fqs_check = pcu_segcblist_n_cbs(&rdp->cblist);

	//trace_pcu_segcb_stats(&rdp->cblist, TPS("SegCbDequeued"));
	pcu_nocb_unlock_irqrestore(rdp, flags);

	/* Invoke callbacks. */
	tick_dep_set_task(current, TICK_DEP_BIT_RCU);
	rhp = pcu_cblist_dequeue(&rcl);

	for (; rhp; rhp = pcu_cblist_dequeue(&rcl)) {
		pcu_callback_t f;

		count++;
		debug_pcu_head_unqueue(rhp);

		pcu_lock_acquire(&pcu_callback_map);
		//trace_pcu_invoke_callback(pcu_state.name, rhp);

		f = rhp->func;
		WRITE_ONCE(rhp->func, (pcu_callback_t)0L);
		f(rhp);

		pcu_lock_release(&pcu_callback_map);

		/*
		 * Stop only if limit reached and CPU has something to do.
		 */
		if (count >= bl && !offloaded &&
		    (need_resched() ||
		     (!is_idle_task(current) && !pcu_is_callbacks_kthread())))
			break;
		if (unlikely(tlimit)) {
			/* only call local_clock() every 32 callbacks */
			if (likely((count & 31) || local_clock() < tlimit))
				continue;
			/* Exceeded the time limit, so leave. */
			break;
		}
		if (!in_serving_softirq()) {
			local_bh_enable();
			lockdep_assert_irqs_enabled();
			cond_resched_tasks_pcu_qs();
			lockdep_assert_irqs_enabled();
			local_bh_disable();
		}
	}

	local_irq_save(flags);
	pcu_nocb_lock(rdp);
	rdp->n_cbs_invoked += count;
	//trace_pcu_batch_end(pcu_state.name, count, !!rcl.head, need_resched(),
	//		    is_idle_task(current), pcu_is_callbacks_kthread());

	/* Update counts and requeue any remaining callbacks. */
	pcu_segcblist_insert_done_cbs(&rdp->cblist, &rcl);
	pcu_segcblist_add_len(&rdp->cblist, -count);

	/* Reinstate batch limit if we have worked down the excess. */
	count = pcu_segcblist_n_cbs(&rdp->cblist);
	if (rdp->blimit >= DEFAULT_MAX_PCU_BLIMIT && count <= qlowmark)
		rdp->blimit = blimit;

	/* Reset ->qlen_last_fqs_check trigger if enough CBs have drained. */
	if (count == 0 && rdp->qlen_last_fqs_check != 0) {
		rdp->qlen_last_fqs_check = 0;
		rdp->n_force_qs_snap = pcu_state.n_force_qs;
	} else if (count < rdp->qlen_last_fqs_check - qhimark)
		rdp->qlen_last_fqs_check = count;

	/*
	 * The following usually indicates a double call_pcu().  To track
	 * this down, try building with CONFIG_DEBUG_OBJECTS_PCU_HEAD=y.
	 */
	empty = pcu_segcblist_empty(&rdp->cblist);
	WARN_ON_ONCE(count == 0 && !empty);
	WARN_ON_ONCE(!IS_ENABLED(CONFIG_PCU_NOCB_CPU) &&
		     count != 0 && empty);
	WARN_ON_ONCE(count == 0 && pcu_segcblist_n_segment_cbs(&rdp->cblist) != 0);
	WARN_ON_ONCE(!empty && pcu_segcblist_n_segment_cbs(&rdp->cblist) == 0);

	pcu_nocb_unlock_irqrestore(rdp, flags);

	/* Re-invoke PCU core processing if there are callbacks remaining. */
	if (!offloaded && pcu_segcblist_ready_cbs(&rdp->cblist))
		invoke_pcu_core();
	tick_dep_clear_task(current, TICK_DEP_BIT_RCU);
}

/*
 * This function is invoked from each scheduling-clock interrupt,
 * and checks to see if this CPU is in a non-context-switch quiescent
 * state, for example, user mode or idle loop.  It also schedules PCU
 * core processing.  If the current grace period has gone on too long,
 * it will ask the scheduler to manufacture a context switch for the sole
 * purpose of providing the needed quiescent state.
 */
void pcu_sched_clock_irq(int user)
{
	//trace_pcu_utilization(TPS("Start scheduler-tick"));
	lockdep_assert_irqs_disabled();
	raw_cpu_inc(pcu_data.ticks_this_gp);
	/* The load-acquire pairs with the store-release setting to true. */
	if (smp_load_acquire(this_cpu_ptr(&pcu_data.pcu_urgent_qs))) {
		/* Idle and userspace execution already are quiescent states. */
		if (!pcu_is_cpu_rrupt_from_idle() && !user) {
			set_tsk_need_resched(current);
			set_preempt_need_resched();
		}
		__this_cpu_write(pcu_data.pcu_urgent_qs, false);
	}
	pcu_flavor_sched_clock_irq(user);
	if (pcu_pending(user))
		invoke_pcu_core();
	lockdep_assert_irqs_disabled();

	//trace_pcu_utilization(TPS("End scheduler-tick"));
}

/*
 * Scan the leaf pcu_node structures.  For each structure on which all
 * CPUs have reported a quiescent state and on which there are tasks
 * blocking the current grace period, initiate PCU priority boosting.
 * Otherwise, invoke the specified function to check dyntick state for
 * each CPU that has not yet reported a quiescent state.
 */
static void force_qs_rnp(int (*f)(struct pcu_data *rdp))
{
	int cpu;
	unsigned long flags;
	unsigned long mask;
	struct pcu_data *rdp;
	struct pcu_node *rnp;

	pcu_state.cbovld = pcu_state.cbovldnext;
	pcu_state.cbovldnext = false;
	pcu_for_each_leaf_node(rnp) {
		cond_resched_tasks_pcu_qs();
		mask = 0;
		raw_spin_lock_irqsave_pcu_node(rnp, flags);
		pcu_state.cbovldnext |= !!rnp->cbovldmask;
		if (rnp->qsmask == 0) {
			if (pcu_preempt_blocked_readers_cgp(rnp)) {
				/*
				 * No point in scanning bits because they
				 * are all zero.  But we might need to
				 * priority-boost blocked readers.
				 */
				pcu_initiate_boost(rnp, flags);
				/* pcu_initiate_boost() releases rnp->lock */
				continue;
			}
			raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
			continue;
		}
		for_each_leaf_node_cpu_mask(rnp, cpu, rnp->qsmask) {
			rdp = per_cpu_ptr(&pcu_data, cpu);
			if (f(rdp)) {
				mask |= rdp->grpmask;
				pcu_disable_urgency_upon_qs(rdp);
			}
		}
		if (mask != 0) {
			/* Idle/offline CPUs, report (releases rnp->lock). */
			pcu_report_qs_rnp(mask, rnp, rnp->gp_seq, flags);
		} else {
			/* Nothing to do here, so just drop the lock. */
			raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
		}
	}
}

/*
 * Force quiescent states on reluctant CPUs, and also detect which
 * CPUs are in dyntick-idle mode.
 */
void pcu_force_quiescent_state(void)
{
	unsigned long flags;
	bool ret;
	struct pcu_node *rnp;
	struct pcu_node *rnp_old = NULL;

	/* Funnel through hierarchy to reduce memory contention. */
	rnp = __this_cpu_read(pcu_data.mynode);
	for (; rnp != NULL; rnp = rnp->parent) {
		ret = (READ_ONCE(pcu_state.gp_flags) & PCU_GP_FLAG_FQS) ||
		       !raw_spin_trylock(&rnp->fqslock);
		if (rnp_old != NULL)
			raw_spin_unlock(&rnp_old->fqslock);
		if (ret)
			return;
		rnp_old = rnp;
	}
	/* rnp_old == pcu_get_root(), rnp == NULL. */

	/* Reached the root of the pcu_node tree, acquire lock. */
	raw_spin_lock_irqsave_pcu_node(rnp_old, flags);
	raw_spin_unlock(&rnp_old->fqslock);
	if (READ_ONCE(pcu_state.gp_flags) & PCU_GP_FLAG_FQS) {
		raw_spin_unlock_irqrestore_pcu_node(rnp_old, flags);
		return;  /* Someone beat us to it. */
	}
	WRITE_ONCE(pcu_state.gp_flags,
		   READ_ONCE(pcu_state.gp_flags) | PCU_GP_FLAG_FQS);
	raw_spin_unlock_irqrestore_pcu_node(rnp_old, flags);
	pcu_gp_kthread_wake();
}
EXPORT_SYMBOL_GPL(pcu_force_quiescent_state);

// Workqueue handler for an PCU reader for kernels enforcing struct PCU
// grace periods.
static void strict_work_handler(struct work_struct *work)
{
	pcu_read_lock();
	pcu_read_unlock();
}

/* Perform PCU core processing work for the current CPU.  */
static __latent_entropy void pcu_core(void)
{
	unsigned long flags;
	struct pcu_data *rdp = raw_cpu_ptr(&pcu_data);
	struct pcu_node *rnp = rdp->mynode;
	const bool do_batch = !pcu_segcblist_completely_offloaded(&rdp->cblist);
	printk("[%s] start! from %ps\n", __func__, __builtin_return_address(0));

	if (cpu_is_offline(smp_processor_id()))
		return;
	//trace_pcu_utilization(TPS("Start PCU core"));
	WARN_ON_ONCE(!rdp->beenonline);

	/* Report any deferred quiescent states if preemption enabled. */
	if (!(preempt_count() & PREEMPT_MASK)) {
		pcu_preempt_deferred_qs(current);
	} else if (pcu_preempt_need_deferred_qs(current)) {
		set_tsk_need_resched(current);
		set_preempt_need_resched();
	}

	/* Update PCU state based on any recent quiescent states. */
	pcu_check_quiescent_state(rdp);

	/* No grace period and unregistered callbacks? */
	if (!pcu_gp_in_progress() &&
	    pcu_segcblist_is_enabled(&rdp->cblist) && do_batch) {
		pcu_nocb_lock_irqsave(rdp, flags);
		if (!pcu_segcblist_restempty(&rdp->cblist, PCU_NEXT_READY_TAIL))
			pcu_accelerate_cbs_unlocked(rnp, rdp);
		pcu_nocb_unlock_irqrestore(rdp, flags);
	}

	pcu_check_gp_start_stall(rnp, rdp, pcu_jiffies_till_stall_check());

	/* If there are callbacks ready, invoke them. */
	if (do_batch && pcu_segcblist_ready_cbs(&rdp->cblist) &&
	    likely(READ_ONCE(pcu_scheduler_fully_active)))
		pcu_do_batch(rdp);

	/* Do any needed deferred wakeups of pcuo kthreads. */
	do_nocb_deferred_wakeup(rdp);
	//trace_pcu_utilization(TPS("End PCU core"));

	// If strict GPs, schedule an PCU reader in a clean environment.
	if (IS_ENABLED(CONFIG_PCU_STRICT_GRACE_PERIOD))
		queue_work_on(rdp->cpu, pcu_gp_wq, &rdp->strict_work);
}

static void pcu_core_si(struct softirq_action *h)
{
	printk("[%s] ych_1\n", __func__);
	pcu_core();
}

static void pcu_wake_cond(struct task_struct *t, int status)
{
	/*
	 * If the thread is yielding, only wake it when this
	 * is invoked from idle
	 */
	if (t && (status != PCU_KTHREAD_YIELDING || is_idle_task(current)))
		wake_up_process(t);
}

static void invoke_pcu_core_kthread(void)
{
	struct task_struct *t;
	unsigned long flags;

	local_irq_save(flags);
	__this_cpu_write(pcu_data.pcu_cpu_has_work, 1);
	t = __this_cpu_read(pcu_data.pcu_cpu_kthread_task);
	if (t != NULL && t != current)
		pcu_wake_cond(t, __this_cpu_read(pcu_data.pcu_cpu_kthread_status));
	local_irq_restore(flags);
}

/*
 * Wake up this CPU's pcuc kthread to do PCU core processing.
 */
static void invoke_pcu_core(void)
{
	if (!cpu_online(smp_processor_id()))
		return;
	if (use_softirq){
		printk("[%s] ych_1\n", __func__);
		raise_softirq(PCU_SOFTIRQ);
	}
	else
		invoke_pcu_core_kthread();
}

static void pcu_cpu_kthread_park(unsigned int cpu)
{
	per_cpu(pcu_data.pcu_cpu_kthread_status, cpu) = PCU_KTHREAD_OFFCPU;
}

static int pcu_cpu_kthread_should_run(unsigned int cpu)
{
	return __this_cpu_read(pcu_data.pcu_cpu_has_work);
}

/*
 * Per-CPU kernel thread that invokes PCU callbacks.  This replaces
 * the PCU softirq used in configurations of PCU that do not support PCU
 * priority boosting.
 */
static void pcu_cpu_kthread(unsigned int cpu)
{
	unsigned int *statusp = this_cpu_ptr(&pcu_data.pcu_cpu_kthread_status);
	char work, *workp = this_cpu_ptr(&pcu_data.pcu_cpu_has_work);
	int spincnt;

	//trace_pcu_utilization(TPS("Start CPU kthread@pcu_run"));
	for (spincnt = 0; spincnt < 10; spincnt++) {
		local_bh_disable();
		*statusp = PCU_KTHREAD_RUNNING;
		local_irq_disable();
		work = *workp;
		*workp = 0;
		local_irq_enable();
		if (work)
			pcu_core();
		local_bh_enable();
		if (*workp == 0) {
			//trace_pcu_utilization(TPS("End CPU kthread@pcu_wait"));
			*statusp = PCU_KTHREAD_WAITING;
			return;
		}
	}
	*statusp = PCU_KTHREAD_YIELDING;
	//trace_pcu_utilization(TPS("Start CPU kthread@pcu_yield"));
	schedule_timeout_idle(2);
	//trace_pcu_utilization(TPS("End CPU kthread@pcu_yield"));
	*statusp = PCU_KTHREAD_WAITING;
}

static struct smp_hotplug_thread pcu_cpu_thread_spec = {
	.store			= &pcu_data.pcu_cpu_kthread_task,
	.thread_should_run	= pcu_cpu_kthread_should_run,
	.thread_fn		= pcu_cpu_kthread,
	.thread_comm		= "pcuc/%u",
	.setup			= pcu_cpu_kthread_setup,
	.park			= pcu_cpu_kthread_park,
};

/*
 * Spawn per-CPU PCU core processing kthreads.
 */
static int pcu_spawn_core_kthreads(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		per_cpu(pcu_data.pcu_cpu_has_work, cpu) = 0;
	if (!IS_ENABLED(CONFIG_PCU_BOOST) && use_softirq)
		return 0;
	WARN_ONCE(smpboot_register_percpu_thread(&pcu_cpu_thread_spec),
		  "%s: Could not start pcuc kthread, OOM is now expected behavior\n", __func__);
	return 0;
}

/*
 * Handle any core-PCU processing required by a call_pcu() invocation.
 */
static void __call_pcu_core(struct pcu_data *rdp, struct pcu_head *head,
			    unsigned long flags)
{
	printk("[%s] ych_1\n", __func__);
	printk("[%s] qhimark = %ld\n", __func__, qhimark);
	/*
	 * If called from an extended quiescent state, invoke the PCU
	 * core in order to force a re-evaluation of PCU's idleness.
	 */
	if (!pcu_is_watching()){
		printk("[%s] ych_2\n", __func__);
		invoke_pcu_core();
	}

	/* If interrupts were disabled or CPU offline, don't invoke PCU core. */
	if (irqs_disabled_flags(flags) || cpu_is_offline(smp_processor_id())){
		printk("[%s] ych_3\n", __func__);
		return;
	}

	/*
	 * Force the grace period if too many callbacks or too long waiting.
	 * Enforce hysteresis, and don't invoke pcu_force_quiescent_state()
	 * if some other CPU has recently done so.  Also, don't bother
	 * invoking pcu_force_quiescent_state() if the newly enqueued callback
	 * is the only one waiting for a grace period to complete.
	 */
	printk("[%s] pcu_segcblist_n_cbs(&rdp->cblist) = %ld, rdp->qlen_last_fqs_check + qhimark = %ld, qhimark = %ld\n", __func__,
			pcu_segcblist_n_cbs(&rdp->cblist), rdp->qlen_last_fqs_check + qhimark, qhimark);
	if (unlikely(pcu_segcblist_n_cbs(&rdp->cblist) >
		     rdp->qlen_last_fqs_check + qhimark)) {
		printk("[%s] ych_4\n", __func__);
		/* Are we ignoring a completed grace period? */
		note_gp_changes(rdp);

		/* Start a new grace period if one not already started. */
		if (!pcu_gp_in_progress()) {
			printk("[%s] ych_5\n", __func__);
			pcu_accelerate_cbs_unlocked(rdp->mynode, rdp);
		} else {
			printk("[%s] ych_6\n", __func__);
			/* Give the grace period a kick. */
			rdp->blimit = DEFAULT_MAX_PCU_BLIMIT;
			if (pcu_state.n_force_qs == rdp->n_force_qs_snap &&
			    pcu_segcblist_first_pend_cb(&rdp->cblist) != head)
				pcu_force_quiescent_state();
			rdp->n_force_qs_snap = pcu_state.n_force_qs;
			rdp->qlen_last_fqs_check = pcu_segcblist_n_cbs(&rdp->cblist);
		}
	}
}

/*
 * PCU callback function to leak a callback.
 */
static void pcu_leak_callback(struct pcu_head *rhp)
{
}

/*
 * Check and if necessary update the leaf pcu_node structure's
 * ->cbovldmask bit corresponding to the current CPU based on that CPU's
 * number of queued PCU callbacks.  The caller must hold the leaf pcu_node
 * structure's ->lock.
 */
static void check_cb_ovld_locked(struct pcu_data *rdp, struct pcu_node *rnp)
{
	raw_lockdep_assert_held_pcu_node(rnp);
	if (qovld_calc <= 0)
		return; // Early boot and wildcard value set.
	if (pcu_segcblist_n_cbs(&rdp->cblist) >= qovld_calc)
		WRITE_ONCE(rnp->cbovldmask, rnp->cbovldmask | rdp->grpmask);
	else
		WRITE_ONCE(rnp->cbovldmask, rnp->cbovldmask & ~rdp->grpmask);
}

/*
 * Check and if necessary update the leaf pcu_node structure's
 * ->cbovldmask bit corresponding to the current CPU based on that CPU's
 * number of queued PCU callbacks.  No locks need be held, but the
 * caller must have disabled interrupts.
 *
 * Note that this function ignores the possibility that there are a lot
 * of callbacks all of which have already seen the end of their respective
 * grace periods.  This omission is due to the need for no-CBs CPUs to
 * be holding ->nocb_lock to do this check, which is too heavy for a
 * common-case operation.
 */
static void check_cb_ovld(struct pcu_data *rdp)
{
	struct pcu_node *const rnp = rdp->mynode;

	if (qovld_calc <= 0 ||
	    ((pcu_segcblist_n_cbs(&rdp->cblist) >= qovld_calc) ==
	     !!(READ_ONCE(rnp->cbovldmask) & rdp->grpmask)))
		return; // Early boot wildcard value or already set correctly.
	raw_spin_lock_pcu_node(rnp);
	check_cb_ovld_locked(rdp, rnp);
	raw_spin_unlock_pcu_node(rnp);
}

/* Helper function for call_pcu() and friends.  */
static void
__call_pcu(struct pcu_head *head, pcu_callback_t func)
{
	static atomic_t doublefrees;
	unsigned long flags;
	struct pcu_data *rdp;
	bool was_alldone;

	/* Misaligned pcu_head! */
	WARN_ON_ONCE((unsigned long)head & (sizeof(void *) - 1));

	if (debug_pcu_head_queue(head)) {
		/*
		 * Probable double call_pcu(), so leak the callback.
		 * Use pcu:pcu_callback trace event to find the previous
		 * time callback was passed to __call_pcu().
		 */
		if (atomic_inc_return(&doublefrees) < 4) {
			pr_err("%s(): Double-freed CB %p->%pS()!!!  ", __func__, head, head->func);
			mem_dump_obj(head);
		}
		WRITE_ONCE(head->func, pcu_leak_callback);
		return;
	}
	head->func = func;
	head->next = NULL;
	local_irq_save(flags);
	kasan_record_aux_stack_noalloc(head);
	rdp = this_cpu_ptr(&pcu_data);

	/* Add the callback to our list. */
	if (unlikely(!pcu_segcblist_is_enabled(&rdp->cblist))) {
		// This can trigger due to call_pcu() from offline CPU:
		WARN_ON_ONCE(pcu_scheduler_active != PCU_SCHEDULER_INACTIVE);
		WARN_ON_ONCE(!pcu_is_watching());
		// Very early boot, before pcu_init().  Initialize if needed
		// and then drop through to queue the callback.
		if (pcu_segcblist_empty(&rdp->cblist))
			pcu_segcblist_init(&rdp->cblist);
	}

	check_cb_ovld(rdp);
	if (pcu_nocb_try_bypass(rdp, head, &was_alldone, flags))
		return; // Enqueued onto ->nocb_bypass, so just leave.
	// If no-CBs CPU gets here, pcu_nocb_try_bypass() acquired ->nocb_lock.
	pcu_segcblist_enqueue(&rdp->cblist, head);
	//if (__is_kvfree_pcu_offset((unsigned long)func))
		//trace_pcu_kvfree_callback(pcu_state.name, head,
		//			 (unsigned long)func,
		//			 pcu_segcblist_n_cbs(&rdp->cblist));
	//else
		//trace_pcu_callback(pcu_state.name, head,
		//		   pcu_segcblist_n_cbs(&rdp->cblist));

	//trace_pcu_segcb_stats(&rdp->cblist, TPS("SegCBQueued"));

	/* Go handle any PCU core processing required. */
	if (unlikely(pcu_rdp_is_offloaded(rdp))) {
		__call_pcu_nocb_wake(rdp, was_alldone, flags); /* unlocks */
	} else {
		__call_pcu_core(rdp, head, flags);
		local_irq_restore(flags);
	}
}

/**
 * call_pcu() - Queue an PCU callback for invocation after a grace period.
 * @head: structure to be used for queueing the PCU updates.
 * @func: actual callback function to be invoked after the grace period
 *
 * The callback function will be invoked some time after a full grace
 * period elapses, in other words after all pre-existing PCU read-side
 * critical sections have completed.  However, the callback function
 * might well execute concurrently with PCU read-side critical sections
 * that started after call_pcu() was invoked.
 *
 * PCU read-side critical sections are delimited by pcu_read_lock()
 * and pcu_read_unlock(), and may be nested.  In addition, but only in
 * v5.0 and later, regions of code across which interrupts, preemption,
 * or softirqs have been disabled also serve as PCU read-side critical
 * sections.  This includes hardware interrupt handlers, softirq handlers,
 * and NMI handlers.
 *
 * Note that all CPUs must agree that the grace period extended beyond
 * all pre-existing PCU read-side critical section.  On systems with more
 * than one CPU, this means that when "func()" is invoked, each CPU is
 * guaranteed to have executed a full memory barrier since the end of its
 * last PCU read-side critical section whose beginning preceded the call
 * to call_pcu().  It also means that each CPU executing an PCU read-side
 * critical section that continues beyond the start of "func()" must have
 * executed a memory barrier after the call_pcu() but before the beginning
 * of that PCU read-side critical section.  Note that these guarantees
 * include CPUs that are offline, idle, or executing in user mode, as
 * well as CPUs that are executing in the kernel.
 *
 * Furthermore, if CPU A invoked call_pcu() and CPU B invoked the
 * resulting PCU callback function "func()", then both CPU A and CPU B are
 * guaranteed to execute a full memory barrier during the time interval
 * between the call to call_pcu() and the invocation of "func()" -- even
 * if CPU A and CPU B are the same CPU (but again only if the system has
 * more than one CPU).
 *
 * Implementation of these memory-ordering guarantees is described here:
 * Documentation/PCU/Design/Memory-Ordering/Tree-PCU-Memory-Ordering.rst.
 */
void call_pcu(struct pcu_head *head, pcu_callback_t func)
{
	printk("[%s] ych_1\n", __func__);
	__call_pcu(head, func);
}
EXPORT_SYMBOL_GPL(call_pcu);


/* Maximum number of jiffies to wait before draining a batch. */
#define KFREE_DRAIN_JIFFIES (HZ / 50)
#define KFREE_N_BATCHES 2
#define FREE_N_CHANNELS 2

/**
 * struct kvfree_pcu_bulk_data - single block to store kvfree_pcu() pointers
 * @nr_records: Number of active pointers in the array
 * @next: Next bulk object in the block chain
 * @records: Array of the kvfree_pcu() pointers
 */
struct kvfree_pcu_bulk_data {
	unsigned long nr_records;
	struct kvfree_pcu_bulk_data *next;
	void *records[];
};

/*
 * This macro defines how many entries the "records" array
 * will contain. It is based on the fact that the size of
 * kvfree_pcu_bulk_data structure becomes exactly one page.
 */
#define KVFREE_BULK_MAX_ENTR \
	((PAGE_SIZE - sizeof(struct kvfree_pcu_bulk_data)) / sizeof(void *))

/**
 * struct kfree_pcu_cpu_work - single batch of kfree_pcu() requests
 * @pcu_work: Let queue_pcu_work() invoke workqueue handler after grace period
 * @head_free: List of kfree_pcu() objects waiting for a grace period
 * @bkvhead_free: Bulk-List of kvfree_pcu() objects waiting for a grace period
 * @krcp: Pointer to @kfree_pcu_cpu structure
 */

struct kfree_pcu_cpu_work {
	struct rcu_work pcu_work;
	struct pcu_head *head_free;
	struct kvfree_pcu_bulk_data *bkvhead_free[FREE_N_CHANNELS];
	struct kfree_pcu_cpu *krcp;
};

/**
 * struct kfree_pcu_cpu - batch up kfree_pcu() requests for PCU grace period
 * @head: List of kfree_pcu() objects not yet waiting for a grace period
 * @bkvhead: Bulk-List of kvfree_pcu() objects not yet waiting for a grace period
 * @krw_arr: Array of batches of kfree_pcu() objects waiting for a grace period
 * @lock: Synchronize access to this structure
 * @monitor_work: Promote @head to @head_free after KFREE_DRAIN_JIFFIES
 * @monitor_todo: Tracks whether a @monitor_work delayed work is pending
 * @initialized: The @pcu_work fields have been initialized
 * @count: Number of objects for which GP not started
 * @bkvcache:
 *	A simple cache list that contains objects for reuse purpose.
 *	In order to save some per-cpu space the list is singular.
 *	Even though it is lockless an access has to be protected by the
 *	per-cpu lock.
 * @page_cache_work: A work to refill the cache when it is empty
 * @backoff_page_cache_fill: Delay cache refills
 * @work_in_progress: Indicates that page_cache_work is running
 * @hrtimer: A hrtimer for scheduling a page_cache_work
 * @nr_bkv_objs: number of allocated objects at @bkvcache.
 *
 * This is a per-CPU structure.  The reason that it is not included in
 * the pcu_data structure is to permit this code to be extracted from
 * the PCU files.  Such extraction could allow further optimization of
 * the interactions with the slab allocators.
 */
struct kfree_pcu_cpu {
	struct pcu_head *head;
	struct kvfree_pcu_bulk_data *bkvhead[FREE_N_CHANNELS];
	struct kfree_pcu_cpu_work krw_arr[KFREE_N_BATCHES];
	raw_spinlock_t lock;
	struct delayed_work monitor_work;
	bool monitor_todo;
	bool initialized;
	int count;

	struct delayed_work page_cache_work;
	atomic_t backoff_page_cache_fill;
	atomic_t work_in_progress;
	struct hrtimer hrtimer;

	struct llist_head bkvcache;
	int nr_bkv_objs;
};

static DEFINE_PER_CPU(struct kfree_pcu_cpu, krc) = {
	.lock = __RAW_SPIN_LOCK_UNLOCKED(krc.lock),
};

static __always_inline void
debug_pcu_bhead_unqueue(struct kvfree_pcu_bulk_data *bhead)
{
#ifdef CONFIG_DEBUG_OBJECTS_PCU_HEAD
	int i;

	for (i = 0; i < bhead->nr_records; i++)
		debug_pcu_head_unqueue((struct pcu_head *)(bhead->records[i]));
#endif
}

static inline struct kfree_pcu_cpu *
krc_this_cpu_lock(unsigned long *flags)
{
	struct kfree_pcu_cpu *krcp;

	local_irq_save(*flags);	// For safely calling this_cpu_ptr().
	krcp = this_cpu_ptr(&krc);
	raw_spin_lock(&krcp->lock);

	return krcp;
}

static inline void
krc_this_cpu_unlock(struct kfree_pcu_cpu *krcp, unsigned long flags)
{
	raw_spin_unlock_irqrestore(&krcp->lock, flags);
}

static inline struct kvfree_pcu_bulk_data *
get_cached_bnode(struct kfree_pcu_cpu *krcp)
{
	if (!krcp->nr_bkv_objs)
		return NULL;

	WRITE_ONCE(krcp->nr_bkv_objs, krcp->nr_bkv_objs - 1);
	return (struct kvfree_pcu_bulk_data *)
		llist_del_first(&krcp->bkvcache);
}

static inline bool
put_cached_bnode(struct kfree_pcu_cpu *krcp,
	struct kvfree_pcu_bulk_data *bnode)
{
	// Check the limit.
	if (krcp->nr_bkv_objs >= pcu_min_cached_objs)
		return false;

	llist_add((struct llist_node *) bnode, &krcp->bkvcache);
	WRITE_ONCE(krcp->nr_bkv_objs, krcp->nr_bkv_objs + 1);
	return true;
}

static int
drain_page_cache(struct kfree_pcu_cpu *krcp)
{
	unsigned long flags;
	struct llist_node *page_list, *pos, *n;
	int freed = 0;

	raw_spin_lock_irqsave(&krcp->lock, flags);
	page_list = llist_del_all(&krcp->bkvcache);
	WRITE_ONCE(krcp->nr_bkv_objs, 0);
	raw_spin_unlock_irqrestore(&krcp->lock, flags);

	llist_for_each_safe(pos, n, page_list) {
		free_page((unsigned long)pos);
		freed++;
	}

	return freed;
}

/*
 * This function is invoked in workqueue context after a grace period.
 * It frees all the objects queued on ->bkvhead_free or ->head_free.
 */
static void kfree_pcu_work(struct work_struct *work)
{
	unsigned long flags;
	struct kvfree_pcu_bulk_data *bkvhead[FREE_N_CHANNELS], *bnext;
	struct pcu_head *head, *next;
	struct kfree_pcu_cpu *krcp;
	struct kfree_pcu_cpu_work *krwp;
	int i, j;

	krwp = container_of(to_rcu_work(work),
			    struct kfree_pcu_cpu_work, pcu_work);
	krcp = krwp->krcp;

	raw_spin_lock_irqsave(&krcp->lock, flags);
	// Channels 1 and 2.
	for (i = 0; i < FREE_N_CHANNELS; i++) {
		bkvhead[i] = krwp->bkvhead_free[i];
		krwp->bkvhead_free[i] = NULL;
	}

	// Channel 3.
	head = krwp->head_free;
	krwp->head_free = NULL;
	raw_spin_unlock_irqrestore(&krcp->lock, flags);

	// Handle the first two channels.
	for (i = 0; i < FREE_N_CHANNELS; i++) {
		for (; bkvhead[i]; bkvhead[i] = bnext) {
			bnext = bkvhead[i]->next;
			debug_pcu_bhead_unqueue(bkvhead[i]);

			pcu_lock_acquire(&pcu_callback_map);
			if (i == 0) { // kmalloc() / kfree().
				//trace_pcu_invoke_kfree_bulk_callback(
				//	pcu_state.name, bkvhead[i]->nr_records,
				//	bkvhead[i]->records);

				kfree_bulk(bkvhead[i]->nr_records,
					bkvhead[i]->records);
			} else { // vmalloc() / vfree().
				for (j = 0; j < bkvhead[i]->nr_records; j++) {
					//trace_pcu_invoke_kvfree_callback(
					//	pcu_state.name,
					//	bkvhead[i]->records[j], 0);

					vfree(bkvhead[i]->records[j]);
				}
			}
			pcu_lock_release(&pcu_callback_map);

			raw_spin_lock_irqsave(&krcp->lock, flags);
			if (put_cached_bnode(krcp, bkvhead[i]))
				bkvhead[i] = NULL;
			raw_spin_unlock_irqrestore(&krcp->lock, flags);

			if (bkvhead[i])
				free_page((unsigned long) bkvhead[i]);

			cond_resched_tasks_pcu_qs();
		}
	}

	/*
	 * This is used when the "bulk" path can not be used for the
	 * double-argument of kvfree_pcu().  This happens when the
	 * page-cache is empty, which means that objects are instead
	 * queued on a linked list through their pcu_head structures.
	 * This list is named "Channel 3".
	 */
	for (; head; head = next) {
		unsigned long offset = (unsigned long)head->func;
		void *ptr = (void *)head - offset;

		next = head->next;
		debug_pcu_head_unqueue((struct pcu_head *)ptr);
		pcu_lock_acquire(&pcu_callback_map);
		//trace_pcu_invoke_kvfree_callback(pcu_state.name, head, offset);

		if (!WARN_ON_ONCE(!__is_kvfree_pcu_offset(offset)))
			kvfree(ptr);

		pcu_lock_release(&pcu_callback_map);
		cond_resched_tasks_pcu_qs();
	}
}

/*
 * This function is invoked after the KFREE_DRAIN_JIFFIES timeout.
 */
static void kfree_pcu_monitor(struct work_struct *work)
{
	struct kfree_pcu_cpu *krcp = container_of(work,
		struct kfree_pcu_cpu, monitor_work.work);
	unsigned long flags;
	int i, j;

	raw_spin_lock_irqsave(&krcp->lock, flags);

	// Attempt to start a new batch.
	for (i = 0; i < KFREE_N_BATCHES; i++) {
		struct kfree_pcu_cpu_work *krwp = &(krcp->krw_arr[i]);

		// Try to detach bkvhead or head and attach it over any
		// available corresponding free channel. It can be that
		// a previous PCU batch is in progress, it means that
		// immediately to queue another one is not possible so
		// in that case the monitor work is rearmed.
		if ((krcp->bkvhead[0] && !krwp->bkvhead_free[0]) ||
			(krcp->bkvhead[1] && !krwp->bkvhead_free[1]) ||
				(krcp->head && !krwp->head_free)) {
			// Channel 1 corresponds to the SLAB-pointer bulk path.
			// Channel 2 corresponds to vmalloc-pointer bulk path.
			for (j = 0; j < FREE_N_CHANNELS; j++) {
				if (!krwp->bkvhead_free[j]) {
					krwp->bkvhead_free[j] = krcp->bkvhead[j];
					krcp->bkvhead[j] = NULL;
				}
			}

			// Channel 3 corresponds to both SLAB and vmalloc
			// objects queued on the linked list.
			if (!krwp->head_free) {
				krwp->head_free = krcp->head;
				krcp->head = NULL;
			}

			WRITE_ONCE(krcp->count, 0);

			// One work is per one batch, so there are three
			// "free channels", the batch can handle. It can
			// be that the work is in the pending state when
			// channels have been detached following by each
			// other.
			queue_rcu_work(system_wq, &krwp->pcu_work);
		}
	}

	// If there is nothing to detach, it means that our job is
	// successfully done here. In case of having at least one
	// of the channels that is still busy we should rearm the
	// work to repeat an attempt. Because previous batches are
	// still in progress.
	if (!krcp->bkvhead[0] && !krcp->bkvhead[1] && !krcp->head)
		krcp->monitor_todo = false;
	else
		schedule_delayed_work(&krcp->monitor_work, KFREE_DRAIN_JIFFIES);

	raw_spin_unlock_irqrestore(&krcp->lock, flags);
}

static enum hrtimer_restart
schedule_page_work_fn(struct hrtimer *t)
{
	struct kfree_pcu_cpu *krcp =
		container_of(t, struct kfree_pcu_cpu, hrtimer);

	queue_delayed_work(system_highpri_wq, &krcp->page_cache_work, 0);
	return HRTIMER_NORESTART;
}

static void fill_page_cache_func(struct work_struct *work)
{
	struct kvfree_pcu_bulk_data *bnode;
	struct kfree_pcu_cpu *krcp =
		container_of(work, struct kfree_pcu_cpu,
			page_cache_work.work);
	unsigned long flags;
	int nr_pages;
	bool pushed;
	int i;

	nr_pages = atomic_read(&krcp->backoff_page_cache_fill) ?
		1 : pcu_min_cached_objs;

	for (i = 0; i < nr_pages; i++) {
		bnode = (struct kvfree_pcu_bulk_data *)
			__get_free_page(GFP_KERNEL | __GFP_NORETRY | __GFP_NOMEMALLOC | __GFP_NOWARN);

		if (bnode) {
			raw_spin_lock_irqsave(&krcp->lock, flags);
			pushed = put_cached_bnode(krcp, bnode);
			raw_spin_unlock_irqrestore(&krcp->lock, flags);

			if (!pushed) {
				free_page((unsigned long) bnode);
				break;
			}
		}
	}

	atomic_set(&krcp->work_in_progress, 0);
	atomic_set(&krcp->backoff_page_cache_fill, 0);
}

static void
run_page_cache_worker(struct kfree_pcu_cpu *krcp)
{
	if (pcu_scheduler_active == PCU_SCHEDULER_RUNNING &&
			!atomic_xchg(&krcp->work_in_progress, 1)) {
		if (atomic_read(&krcp->backoff_page_cache_fill)) {
			queue_delayed_work(system_wq,
				&krcp->page_cache_work,
					msecs_to_jiffies(pcu_delay_page_cache_fill_msec));
		} else {
			hrtimer_init(&krcp->hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
			krcp->hrtimer.function = schedule_page_work_fn;
			hrtimer_start(&krcp->hrtimer, 0, HRTIMER_MODE_REL);
		}
	}
}

// Record ptr in a page managed by krcp, with the pre-krc_this_cpu_lock()
// state specified by flags.  If can_alloc is true, the caller must
// be schedulable and not be holding any locks or mutexes that might be
// acquired by the memory allocator or anything that it might invoke.
// Returns true if ptr was successfully recorded, else the caller must
// use a fallback.
static inline bool
add_ptr_to_bulk_krc_lock(struct kfree_pcu_cpu **krcp,
	unsigned long *flags, void *ptr, bool can_alloc)
{
	struct kvfree_pcu_bulk_data *bnode;
	int idx;

	*krcp = krc_this_cpu_lock(flags);
	if (unlikely(!(*krcp)->initialized))
		return false;

	idx = !!is_vmalloc_addr(ptr);

	/* Check if a new block is required. */
	if (!(*krcp)->bkvhead[idx] ||
			(*krcp)->bkvhead[idx]->nr_records == KVFREE_BULK_MAX_ENTR) {
		bnode = get_cached_bnode(*krcp);
		if (!bnode && can_alloc) {
			krc_this_cpu_unlock(*krcp, *flags);

			// __GFP_NORETRY - allows a light-weight direct reclaim
			// what is OK from minimizing of fallback hitting point of
			// view. Apart of that it forbids any OOM invoking what is
			// also beneficial since we are about to release memory soon.
			//
			// __GFP_NOMEMALLOC - prevents from consuming of all the
			// memory reserves. Please note we have a fallback path.
			//
			// __GFP_NOWARN - it is supposed that an allocation can
			// be failed under low memory or high memory pressure
			// scenarios.
			bnode = (struct kvfree_pcu_bulk_data *)
				__get_free_page(GFP_KERNEL | __GFP_NORETRY | __GFP_NOMEMALLOC | __GFP_NOWARN);
			*krcp = krc_this_cpu_lock(flags);
		}

		if (!bnode)
			return false;

		/* Initialize the new block. */
		bnode->nr_records = 0;
		bnode->next = (*krcp)->bkvhead[idx];

		/* Attach it to the head. */
		(*krcp)->bkvhead[idx] = bnode;
	}

	/* Finally insert. */
	(*krcp)->bkvhead[idx]->records
		[(*krcp)->bkvhead[idx]->nr_records++] = ptr;

	return true;
}

/*
 * Queue a request for lazy invocation of the appropriate free routine
 * after a grace period.  Please note that three paths are maintained,
 * two for the common case using arrays of pointers and a third one that
 * is used only when the main paths cannot be used, for example, due to
 * memory pressure.
 *
 * Each kvfree_call_pcu() request is added to a batch. The batch will be drained
 * every KFREE_DRAIN_JIFFIES number of jiffies. All the objects in the batch will
 * be free'd in workqueue context. This allows us to: batch requests together to
 * reduce the number of grace periods during heavy kvfree_pcu()/kvfree_pcu() load.
 */
void kvfree_call_pcu(struct pcu_head *head, pcu_callback_t func)
{
	unsigned long flags;
	struct kfree_pcu_cpu *krcp;
	bool success;
	void *ptr;

	if (head) {
		ptr = (void *) head - (unsigned long) func;
	} else {
		/*
		 * Please note there is a limitation for the head-less
		 * variant, that is why there is a clear rule for such
		 * objects: it can be used from might_sleep() context
		 * only. For other places please embed an pcu_head to
		 * your data.
		 */
		might_sleep();
		ptr = (unsigned long *) func;
	}

	// Queue the object but don't yet schedule the batch.
	if (debug_pcu_head_queue(ptr)) {
		// Probable double kfree_pcu(), just leak.
		WARN_ONCE(1, "%s(): Double-freed call. pcu_head %p\n",
			  __func__, head);

		// Mark as success and leave.
		return;
	}

	kasan_record_aux_stack_noalloc(ptr);
	success = add_ptr_to_bulk_krc_lock(&krcp, &flags, ptr, !head);
	if (!success) {
		run_page_cache_worker(krcp);

		if (head == NULL)
			// Inline if kvfree_pcu(one_arg) call.
			goto unlock_return;

		head->func = func;
		head->next = krcp->head;
		krcp->head = head;
		success = true;
	}

	WRITE_ONCE(krcp->count, krcp->count + 1);

	// Set timer to drain after KFREE_DRAIN_JIFFIES.
	if (pcu_scheduler_active == PCU_SCHEDULER_RUNNING &&
	    !krcp->monitor_todo) {
		krcp->monitor_todo = true;
		schedule_delayed_work(&krcp->monitor_work, KFREE_DRAIN_JIFFIES);
	}

unlock_return:
	krc_this_cpu_unlock(krcp, flags);

	/*
	 * Inline kvfree() after synchronize_pcu(). We can do
	 * it from might_sleep() context only, so the current
	 * CPU can pass the QS state.
	 */
	if (!success) {
		debug_pcu_head_unqueue((struct pcu_head *) ptr);
		synchronize_pcu();
		kvfree(ptr);
	}
}
EXPORT_SYMBOL_GPL(kvfree_call_pcu);

void kfree_call_pcu(struct pcu_head *head, pcu_callback_t func)
{
	kvfree_call_pcu(head,func);
}
EXPORT_SYMBOL_GPL(kfree_call_pcu);

static unsigned long
kfree_pcu_shrink_count(struct shrinker *shrink, struct shrink_control *sc)
{
	int cpu;
	unsigned long count = 0;

	/* Snapshot count of all CPUs */
	for_each_possible_cpu(cpu) {
		struct kfree_pcu_cpu *krcp = per_cpu_ptr(&krc, cpu);

		count += READ_ONCE(krcp->count);
		count += READ_ONCE(krcp->nr_bkv_objs);
		atomic_set(&krcp->backoff_page_cache_fill, 1);
	}

	return count;
}

static unsigned long
kfree_pcu_shrink_scan(struct shrinker *shrink, struct shrink_control *sc)
{
	int cpu, freed = 0;

	for_each_possible_cpu(cpu) {
		int count;
		struct kfree_pcu_cpu *krcp = per_cpu_ptr(&krc, cpu);

		count = krcp->count;
		count += drain_page_cache(krcp);
		kfree_pcu_monitor(&krcp->monitor_work.work);

		sc->nr_to_scan -= count;
		freed += count;

		if (sc->nr_to_scan <= 0)
			break;
	}

	return freed == 0 ? SHRINK_STOP : freed;
}

struct shrinker kfree_pcu_shrinker = {
	.count_objects = kfree_pcu_shrink_count,
	.scan_objects = kfree_pcu_shrink_scan,
	.batch = 0,
	.seeks = DEFAULT_SEEKS,
};

void kfree_pcu_scheduler_running(void)
{
	int cpu;
	unsigned long flags;

	for_each_possible_cpu(cpu) {
		struct kfree_pcu_cpu *krcp = per_cpu_ptr(&krc, cpu);

		raw_spin_lock_irqsave(&krcp->lock, flags);
		if ((!krcp->bkvhead[0] && !krcp->bkvhead[1] && !krcp->head) ||
				krcp->monitor_todo) {
			raw_spin_unlock_irqrestore(&krcp->lock, flags);
			continue;
		}
		krcp->monitor_todo = true;
		schedule_delayed_work_on(cpu, &krcp->monitor_work,
					 KFREE_DRAIN_JIFFIES);
		raw_spin_unlock_irqrestore(&krcp->lock, flags);
	}
}

/*
 * During early boot, any blocking grace-period wait automatically
 * implies a grace period.  Later on, this is never the case for PREEMPTION.
 *
 * However, because a context switch is a grace period for !PREEMPTION, any
 * blocking grace-period wait automatically implies a grace period if
 * there is only one CPU online at any point time during execution of
 * either synchronize_pcu() or synchronize_pcu_expedited().  It is OK to
 * occasionally incorrectly indicate that there are multiple CPUs online
 * when there was in fact only one the whole time, as this just adds some
 * overhead: PCU still operates correctly.
 */
static int pcu_blocking_is_gp(void)
{
	int ret;

	if (IS_ENABLED(CONFIG_PREEMPTION))
		return pcu_scheduler_active == PCU_SCHEDULER_INACTIVE;
	might_sleep();  /* Check for PCU read-side critical section. */
	preempt_disable();
	/*
	 * If the pcu_state.n_online_cpus counter is equal to one,
	 * there is only one CPU, and that CPU sees all prior accesses
	 * made by any CPU that was online at the time of its access.
	 * Furthermore, if this counter is equal to one, its value cannot
	 * change until after the preempt_enable() below.
	 *
	 * Furthermore, if pcu_state.n_online_cpus is equal to one here,
	 * all later CPUs (both this one and any that come online later
	 * on) are guaranteed to see all accesses prior to this point
	 * in the code, without the need for additional memory barriers.
	 * Those memory barriers are provided by CPU-hotplug code.
	 */
	ret = READ_ONCE(pcu_state.n_online_cpus) <= 1;
	preempt_enable();
	return ret;
}

/**
 * synchronize_pcu - wait until a grace period has elapsed.
 *
 * Control will return to the caller some time after a full grace
 * period has elapsed, in other words after all currently executing PCU
 * read-side critical sections have completed.  Note, however, that
 * upon return from synchronize_pcu(), the caller might well be executing
 * concurrently with new PCU read-side critical sections that began while
 * synchronize_pcu() was waiting.
 *
 * PCU read-side critical sections are delimited by pcu_read_lock()
 * and pcu_read_unlock(), and may be nested.  In addition, but only in
 * v5.0 and later, regions of code across which interrupts, preemption,
 * or softirqs have been disabled also serve as PCU read-side critical
 * sections.  This includes hardware interrupt handlers, softirq handlers,
 * and NMI handlers.
 *
 * Note that this guarantee implies further memory-ordering guarantees.
 * On systems with more than one CPU, when synchronize_pcu() returns,
 * each CPU is guaranteed to have executed a full memory barrier since
 * the end of its last PCU read-side critical section whose beginning
 * preceded the call to synchronize_pcu().  In addition, each CPU having
 * an PCU read-side critical section that extends beyond the return from
 * synchronize_pcu() is guaranteed to have executed a full memory barrier
 * after the beginning of synchronize_pcu() and before the beginning of
 * that PCU read-side critical section.  Note that these guarantees include
 * CPUs that are offline, idle, or executing in user mode, as well as CPUs
 * that are executing in the kernel.
 *
 * Furthermore, if CPU A invoked synchronize_pcu(), which returned
 * to its caller on CPU B, then both CPU A and CPU B are guaranteed
 * to have executed a full memory barrier during the execution of
 * synchronize_pcu() -- even if CPU A and CPU B are the same CPU (but
 * again only if the system has more than one CPU).
 *
 * Implementation of these memory-ordering guarantees is described here:
 * Documentation/PCU/Design/Memory-Ordering/Tree-PCU-Memory-Ordering.rst.
 */
void synchronize_pcu(void)
{
	PCU_LOCKDEP_WARN(lock_is_held(&pcu_bh_lock_map) ||
			 lock_is_held(&pcu_lock_map) ||
			 lock_is_held(&pcu_sched_lock_map),
			 "Illegal synchronize_pcu() in PCU read-side critical section");
	if (pcu_blocking_is_gp())
		return;  // Context allows vacuous grace periods.
	if (pcu_gp_is_expedited())
		synchronize_pcu_expedited();
	else
		wait_pcu_gp(call_pcu);
}
EXPORT_SYMBOL_GPL(synchronize_pcu);

/**
 * get_state_synchronize_pcu - Snapshot current PCU state
 *
 * Returns a cookie that is used by a later call to cond_synchronize_pcu()
 * or poll_state_synchronize_pcu() to determine whether or not a full
 * grace period has elapsed in the meantime.
 */
unsigned long get_state_synchronize_pcu(void)
{
	/*
	 * Any prior manipulation of PCU-protected data must happen
	 * before the load from ->gp_seq.
	 */
	smp_mb();  /* ^^^ */
	return pcu_seq_snap(&pcu_state.gp_seq);
}
EXPORT_SYMBOL_GPL(get_state_synchronize_pcu);

/**
 * start_poll_synchronize_pcu - Snapshot and start PCU grace period
 *
 * Returns a cookie that is used by a later call to cond_synchronize_pcu()
 * or poll_state_synchronize_pcu() to determine whether or not a full
 * grace period has elapsed in the meantime.  If the needed grace period
 * is not already slated to start, notifies PCU core of the need for that
 * grace period.
 *
 * Interrupts must be enabled for the case where it is necessary to awaken
 * the grace-period kthread.
 */
unsigned long start_poll_synchronize_pcu(void)
{
	unsigned long flags;
	unsigned long gp_seq = get_state_synchronize_pcu();
	bool needwake;
	struct pcu_data *rdp;
	struct pcu_node *rnp;

	lockdep_assert_irqs_enabled();
	local_irq_save(flags);
	rdp = this_cpu_ptr(&pcu_data);
	rnp = rdp->mynode;
	raw_spin_lock_pcu_node(rnp); // irqs already disabled.
	needwake = pcu_start_this_gp(rnp, rdp, gp_seq);
	raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
	if (needwake)
		pcu_gp_kthread_wake();
	return gp_seq;
}
EXPORT_SYMBOL_GPL(start_poll_synchronize_pcu);

/**
 * poll_state_synchronize_pcu - Conditionally wait for an PCU grace period
 *
 * @oldstate: value from get_state_synchronize_pcu() or start_poll_synchronize_pcu()
 *
 * If a full PCU grace period has elapsed since the earlier call from
 * which oldstate was obtained, return @true, otherwise return @false.
 * If @false is returned, it is the caller's responsibilty to invoke this
 * function later on until it does return @true.  Alternatively, the caller
 * can explicitly wait for a grace period, for example, by passing @oldstate
 * to cond_synchronize_pcu() or by directly invoking synchronize_pcu().
 *
 * Yes, this function does not take counter wrap into account.
 * But counter wrap is harmless.  If the counter wraps, we have waited for
 * more than 2 billion grace periods (and way more on a 64-bit system!).
 * Those needing to keep oldstate values for very long time periods
 * (many hours even on 32-bit systems) should check them occasionally
 * and either refresh them or set a flag indicating that the grace period
 * has completed.
 *
 * This function provides the same memory-ordering guarantees that
 * would be provided by a synchronize_pcu() that was invoked at the call
 * to the function that provided @oldstate, and that returned at the end
 * of this function.
 */
bool poll_state_synchronize_pcu(unsigned long oldstate)
{
	if (pcu_seq_done(&pcu_state.gp_seq, oldstate)) {
		smp_mb(); /* Ensure GP ends before subsequent accesses. */
		return true;
	}
	return false;
}
EXPORT_SYMBOL_GPL(poll_state_synchronize_pcu);

/**
 * cond_synchronize_pcu - Conditionally wait for an PCU grace period
 *
 * @oldstate: value from get_state_synchronize_pcu() or start_poll_synchronize_pcu()
 *
 * If a full PCU grace period has elapsed since the earlier call to
 * get_state_synchronize_pcu() or start_poll_synchronize_pcu(), just return.
 * Otherwise, invoke synchronize_pcu() to wait for a full grace period.
 *
 * Yes, this function does not take counter wrap into account.  But
 * counter wrap is harmless.  If the counter wraps, we have waited for
 * more than 2 billion grace periods (and way more on a 64-bit system!),
 * so waiting for one additional grace period should be just fine.
 *
 * This function provides the same memory-ordering guarantees that
 * would be provided by a synchronize_pcu() that was invoked at the call
 * to the function that provided @oldstate, and that returned at the end
 * of this function.
 */
void cond_synchronize_pcu(unsigned long oldstate)
{
	if (!poll_state_synchronize_pcu(oldstate))
		synchronize_pcu();
}
EXPORT_SYMBOL_GPL(cond_synchronize_pcu);

/*
 * Check to see if there is any immediate PCU-related work to be done by
 * the current CPU, returning 1 if so and zero otherwise.  The checks are
 * in order of increasing expense: checks that can be carried out against
 * CPU-local state are performed first.  However, we must check for CPU
 * stalls first, else we might not get a chance.
 */
static int pcu_pending(int user)
{
	bool gp_in_progress;
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);
	struct pcu_node *rnp = rdp->mynode;

	lockdep_assert_irqs_disabled();

	/* Check for CPU stalls, if enabled. */
	check_cpu_stall(rdp);

	/* Does this CPU need a deferred NOCB wakeup? */
	if (pcu_nocb_need_deferred_wakeup(rdp, PCU_NOCB_WAKE))
		return 1;

	/* Is this a nohz_full CPU in userspace or idle?  (Ignore PCU if so.) */
	if ((user || pcu_is_cpu_rrupt_from_idle()) && pcu_nohz_full_cpu())
		return 0;

	/* Is the PCU core waiting for a quiescent state from this CPU? */
	gp_in_progress = pcu_gp_in_progress();
	if (rdp->core_needs_qs && !rdp->cpu_no_qs.b.norm && gp_in_progress)
		return 1;

	/* Does this CPU have callbacks ready to invoke? */
	if (!pcu_rdp_is_offloaded(rdp) &&
	    pcu_segcblist_ready_cbs(&rdp->cblist))
		return 1;

	/* Has PCU gone idle with this CPU needing another grace period? */
	if (!gp_in_progress && pcu_segcblist_is_enabled(&rdp->cblist) &&
	    !pcu_rdp_is_offloaded(rdp) &&
	    !pcu_segcblist_restempty(&rdp->cblist, PCU_NEXT_READY_TAIL))
		return 1;

	/* Have PCU grace period completed or started?  */
	if (pcu_seq_current(&rnp->gp_seq) != rdp->gp_seq ||
	    unlikely(READ_ONCE(rdp->gpwrap))) /* outside lock */
		return 1;

	/* nothing to do */
	return 0;
}

/*
 * Helper function for pcu_barrier() tracing.  If tracing is disabled,
 * the compiler is expected to optimize this away.
 */
static void pcu_barrier_trace(const char *s, int cpu, unsigned long done)
{
//	trace_pcu_barrier(pcu_state.name, s, cpu,
//			  atomic_read(&pcu_state.barrier_cpu_count), done);
}

/*
 * PCU callback function for pcu_barrier().  If we are last, wake
 * up the task executing pcu_barrier().
 *
 * Note that the value of pcu_state.barrier_sequence must be captured
 * before the atomic_dec_and_test().  Otherwise, if this CPU is not last,
 * other CPUs might count the value down to zero before this CPU gets
 * around to invoking pcu_barrier_trace(), which might result in bogus
 * data from the next instance of pcu_barrier().
 */
static void pcu_barrier_callback(struct pcu_head *rhp)
{
	unsigned long __maybe_unused s = pcu_state.barrier_sequence;

	if (atomic_dec_and_test(&pcu_state.barrier_cpu_count)) {
		pcu_barrier_trace(TPS("LastCB"), -1, s);
		complete(&pcu_state.barrier_completion);
	} else {
		pcu_barrier_trace(TPS("CB"), -1, s);
	}
}

/*
 * Called with preemption disabled, and from cross-cpu IRQ context.
 */
static void pcu_barrier_func(void *cpu_in)
{
	uintptr_t cpu = (uintptr_t)cpu_in;
	struct pcu_data *rdp = per_cpu_ptr(&pcu_data, cpu);

	pcu_barrier_trace(TPS("IRQ"), -1, pcu_state.barrier_sequence);
	rdp->barrier_head.func = pcu_barrier_callback;
	debug_pcu_head_queue(&rdp->barrier_head);
	pcu_nocb_lock(rdp);
	WARN_ON_ONCE(!pcu_nocb_flush_bypass(rdp, NULL, jiffies));
	if (pcu_segcblist_entrain(&rdp->cblist, &rdp->barrier_head)) {
		atomic_inc(&pcu_state.barrier_cpu_count);
	} else {
		debug_pcu_head_unqueue(&rdp->barrier_head);
		pcu_barrier_trace(TPS("IRQNQ"), -1,
				  pcu_state.barrier_sequence);
	}
	pcu_nocb_unlock(rdp);
}

/**
 * pcu_barrier - Wait until all in-flight call_pcu() callbacks complete.
 *
 * Note that this primitive does not necessarily wait for an PCU grace period
 * to complete.  For example, if there are no PCU callbacks queued anywhere
 * in the system, then pcu_barrier() is within its rights to return
 * immediately, without waiting for anything, much less an PCU grace period.
 */
void pcu_barrier(void)
{
	uintptr_t cpu;
	struct pcu_data *rdp;
	unsigned long s = pcu_seq_snap(&pcu_state.barrier_sequence);

	pcu_barrier_trace(TPS("Begin"), -1, s);

	/* Take mutex to serialize concurrent pcu_barrier() requests. */
	mutex_lock(&pcu_state.barrier_mutex);

	/* Did someone else do our work for us? */
	if (pcu_seq_done(&pcu_state.barrier_sequence, s)) {
		pcu_barrier_trace(TPS("EarlyExit"), -1,
				  pcu_state.barrier_sequence);
		smp_mb(); /* caller's subsequent code after above check. */
		mutex_unlock(&pcu_state.barrier_mutex);
		return;
	}

	/* Mark the start of the barrier operation. */
	pcu_seq_start(&pcu_state.barrier_sequence);
	pcu_barrier_trace(TPS("Inc1"), -1, pcu_state.barrier_sequence);

	/*
	 * Initialize the count to two rather than to zero in order
	 * to avoid a too-soon return to zero in case of an immediate
	 * invocation of the just-enqueued callback (or preemption of
	 * this task).  Exclude CPU-hotplug operations to ensure that no
	 * offline non-offloaded CPU has callbacks queued.
	 */
	init_completion(&pcu_state.barrier_completion);
	atomic_set(&pcu_state.barrier_cpu_count, 2);
	get_online_cpus();

	/*
	 * Force each CPU with callbacks to register a new callback.
	 * When that callback is invoked, we will know that all of the
	 * corresponding CPU's preceding callbacks have been invoked.
	 */
	for_each_possible_cpu(cpu) {
		rdp = per_cpu_ptr(&pcu_data, cpu);
		if (cpu_is_offline(cpu) &&
		    !pcu_rdp_is_offloaded(rdp))
			continue;
		if (pcu_segcblist_n_cbs(&rdp->cblist) && cpu_online(cpu)) {
			pcu_barrier_trace(TPS("OnlineQ"), cpu,
					  pcu_state.barrier_sequence);
			smp_call_function_single(cpu, pcu_barrier_func, (void *)cpu, 1);
		} else if (pcu_segcblist_n_cbs(&rdp->cblist) &&
			   cpu_is_offline(cpu)) {
			pcu_barrier_trace(TPS("OfflineNoCBQ"), cpu,
					  pcu_state.barrier_sequence);
			local_irq_disable();
			pcu_barrier_func((void *)cpu);
			local_irq_enable();
		} else if (cpu_is_offline(cpu)) {
			pcu_barrier_trace(TPS("OfflineNoCBNoQ"), cpu,
					  pcu_state.barrier_sequence);
		} else {
			pcu_barrier_trace(TPS("OnlineNQ"), cpu,
					  pcu_state.barrier_sequence);
		}
	}
	put_online_cpus();

	/*
	 * Now that we have an pcu_barrier_callback() callback on each
	 * CPU, and thus each counted, remove the initial count.
	 */
	if (atomic_sub_and_test(2, &pcu_state.barrier_cpu_count))
		complete(&pcu_state.barrier_completion);

	/* Wait for all pcu_barrier_callback() callbacks to be invoked. */
	wait_for_completion(&pcu_state.barrier_completion);

	/* Mark the end of the barrier operation. */
	pcu_barrier_trace(TPS("Inc2"), -1, pcu_state.barrier_sequence);
	pcu_seq_end(&pcu_state.barrier_sequence);

	/* Other pcu_barrier() invocations can now safely proceed. */
	mutex_unlock(&pcu_state.barrier_mutex);
}
EXPORT_SYMBOL_GPL(pcu_barrier);

/*
 * Propagate ->qsinitmask bits up the pcu_node tree to account for the
 * first CPU in a given leaf pcu_node structure coming online.  The caller
 * must hold the corresponding leaf pcu_node ->lock with interrrupts
 * disabled.
 */
static void pcu_init_new_rnp(struct pcu_node *rnp_leaf)
{
	long mask;
	long oldmask;
	struct pcu_node *rnp = rnp_leaf;

	raw_lockdep_assert_held_pcu_node(rnp_leaf);
	WARN_ON_ONCE(rnp->wait_blkd_tasks);
	for (;;) {
		mask = rnp->grpmask;
		rnp = rnp->parent;
		if (rnp == NULL)
			return;
		raw_spin_lock_pcu_node(rnp); /* Interrupts already disabled. */
		oldmask = rnp->qsmaskinit;
		rnp->qsmaskinit |= mask;
		raw_spin_unlock_pcu_node(rnp); /* Interrupts remain disabled. */
		if (oldmask)
			return;
	}
}

/*
 * Do boot-time initialization of a CPU's per-CPU PCU data.
 */
static void 
pcu_boot_init_percpu_data(int cpu)
{
	struct pcu_data *rdp = per_cpu_ptr(&pcu_data, cpu);

	/* Set up local state, ensuring consistent view of global state. */
	rdp->grpmask = leaf_node_cpu_bit(rdp->mynode, cpu);
	INIT_WORK(&rdp->strict_work, strict_work_handler);
	WARN_ON_ONCE(rdp->dynticks_nesting != 1);
	WARN_ON_ONCE(pcu_dynticks_in_eqs(pcu_dynticks_snap(rdp)));
	rdp->pcu_ofl_gp_seq = pcu_state.gp_seq;
	rdp->pcu_ofl_gp_flags = PCU_GP_CLEANED;
	rdp->pcu_onl_gp_seq = pcu_state.gp_seq;
	rdp->pcu_onl_gp_flags = PCU_GP_CLEANED;
	rdp->cpu = cpu;
	pcu_boot_init_nocb_percpu_data(rdp);
}

/*
 * Invoked early in the CPU-online process, when pretty much all services
 * are available.  The incoming CPU is not present.
 *
 * Initializes a CPU's per-CPU PCU data.  Note that only one online or
 * offline event can be happening at a given time.  Note also that we can
 * accept some slop in the rsp->gp_seq access due to the fact that this
 * CPU cannot possibly have any non-offloaded PCU callbacks in flight yet.
 * And any offloaded callbacks are being numbered elsewhere.
 */
int pcutree_prepare_cpu(unsigned int cpu)
{
	unsigned long flags;
	struct pcu_data *rdp = per_cpu_ptr(&pcu_data, cpu);
	struct pcu_node *rnp = pcu_get_root();

	/* Set up local state, ensuring consistent view of global state. */
	raw_spin_lock_irqsave_pcu_node(rnp, flags);
	rdp->qlen_last_fqs_check = 0;
	rdp->n_force_qs_snap = pcu_state.n_force_qs;
	rdp->blimit = blimit;
	rdp->dynticks_nesting = 1;	/* CPU not up, no tearing. */
	pcu_dynticks_eqs_online();
	raw_spin_unlock_pcu_node(rnp);		/* irqs remain disabled. */

	/*
	 * Only non-NOCB CPUs that didn't have early-boot callbacks need to be
	 * (re-)initialized.
	 */
	if (!pcu_segcblist_is_enabled(&rdp->cblist))
		pcu_segcblist_init(&rdp->cblist);  /* Re-enable callbacks. */

	/*
	 * Add CPU to leaf pcu_node pending-online bitmask.  Any needed
	 * propagation up the pcu_node tree will happen at the beginning
	 * of the next grace period.
	 */
	rnp = rdp->mynode;
	raw_spin_lock_pcu_node(rnp);		/* irqs already disabled. */
	rdp->beenonline = true;	 /* We have now been online. */
	rdp->gp_seq = READ_ONCE(rnp->gp_seq);
	rdp->gp_seq_needed = rdp->gp_seq;
	rdp->cpu_no_qs.b.norm = true;
	rdp->core_needs_qs = false;
	rdp->pcu_iw_pending = false;
	rdp->pcu_iw = IRQ_WORK_INIT_HARD(pcu_iw_handler);
	rdp->pcu_iw_gp_seq = rdp->gp_seq - 1;
	//trace_pcu_grace_period(pcu_state.name, rdp->gp_seq, TPS("cpuonl"));
	raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
	pcu_spawn_one_boost_kthread(rnp);
	pcu_spawn_cpu_nocb_kthread(cpu);
	WRITE_ONCE(pcu_state.n_online_cpus, pcu_state.n_online_cpus + 1);

	return 0;
}

/*
 * Update PCU priority boot kthread affinity for CPU-hotplug changes.
 */
static void pcutree_affinity_setting(unsigned int cpu, int outgoing)
{
	struct pcu_data *rdp = per_cpu_ptr(&pcu_data, cpu);

	pcu_boost_kthread_setaffinity(rdp->mynode, outgoing);
}

/*
 * Near the end of the CPU-online process.  Pretty much all services
 * enabled, and the CPU is now very much alive.
 */
int pcutree_online_cpu(unsigned int cpu)
{
	unsigned long flags;
	struct pcu_data *rdp;
	struct pcu_node *rnp;
	//printk("[%s] ych_1\n", __func__);

	rdp = per_cpu_ptr(&pcu_data, cpu);
	rnp = rdp->mynode;
	raw_spin_lock_irqsave_pcu_node(rnp, flags);
	rnp->ffmask |= rdp->grpmask;
	raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
	if (pcu_scheduler_active == PCU_SCHEDULER_INACTIVE)
		return 0; /* Too early in boot for scheduler work. */
	sync_sched_exp_online_cleanup(cpu);
	pcutree_affinity_setting(cpu, -1);

	// Stop-machine done, so allow nohz_full to disable tick.
	tick_dep_clear(TICK_DEP_BIT_RCU);
	return 0;
}

/*
 * Near the beginning of the process.  The CPU is still very much alive
 * with pretty much all services enabled.
 */
int pcutree_offline_cpu(unsigned int cpu)
{
	unsigned long flags;
	struct pcu_data *rdp;
	struct pcu_node *rnp;

	rdp = per_cpu_ptr(&pcu_data, cpu);
	rnp = rdp->mynode;
	raw_spin_lock_irqsave_pcu_node(rnp, flags);
	rnp->ffmask &= ~rdp->grpmask;
	raw_spin_unlock_irqrestore_pcu_node(rnp, flags);

	pcutree_affinity_setting(cpu, cpu);

	// nohz_full CPUs need the tick for stop-machine to work quickly
	tick_dep_set(TICK_DEP_BIT_RCU);
	return 0;
}

/*
 * Mark the specified CPU as being online so that subsequent grace periods
 * (both expedited and normal) will wait on it.  Note that this means that
 * incoming CPUs are not allowed to use PCU read-side critical sections
 * until this function is called.  Failing to observe this restriction
 * will result in lockdep splats.
 *
 * Note that this function is special in that it is invoked directly
 * from the incoming CPU rather than from the cpuhp_step mechanism.
 * This is because this function must be invoked at a precise location.
 */
void pcu_cpu_starting(unsigned int cpu)
{
	unsigned long flags;
	unsigned long mask;
	struct pcu_data *rdp;
	struct pcu_node *rnp;
	bool newcpu;
	//printk("[%s] ych_1\n", __func__);

	rdp = per_cpu_ptr(&pcu_data, cpu);
	if (rdp->cpu_started)
		return;
	rdp->cpu_started = true;

	rnp = rdp->mynode;
	mask = rdp->grpmask;
	WRITE_ONCE(rnp->ofl_seq, rnp->ofl_seq + 1);
	WARN_ON_ONCE(!(rnp->ofl_seq & 0x1));
	smp_mb(); // Pair with pcu_gp_cleanup()'s ->ofl_seq barrier().
	raw_spin_lock_irqsave_pcu_node(rnp, flags);
	WRITE_ONCE(rnp->qsmaskinitnext, rnp->qsmaskinitnext | mask);
	newcpu = !(rnp->expmaskinitnext & mask);
	rnp->expmaskinitnext |= mask;
	/* Allow lockless access for expedited grace periods. */
	smp_store_release(&pcu_state.ncpus, pcu_state.ncpus + newcpu); /* ^^^ */
	ASSERT_EXCLUSIVE_WRITER(pcu_state.ncpus);
	pcu_gpnum_ovf(rnp, rdp); /* Offline-induced counter wrap? */
	rdp->pcu_onl_gp_seq = READ_ONCE(pcu_state.gp_seq);
	rdp->pcu_onl_gp_flags = READ_ONCE(pcu_state.gp_flags);

	/* An incoming CPU should never be blocking a grace period. */
	if (WARN_ON_ONCE(rnp->qsmask & mask)) { /* PCU waiting on incoming CPU? */
		pcu_disable_urgency_upon_qs(rdp);
		/* Report QS -after- changing ->qsmaskinitnext! */
		pcu_report_qs_rnp(mask, rnp, rnp->gp_seq, flags);
	} else {
		raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
	}
	smp_mb(); // Pair with pcu_gp_cleanup()'s ->ofl_seq barrier().
	WRITE_ONCE(rnp->ofl_seq, rnp->ofl_seq + 1);
	WARN_ON_ONCE(rnp->ofl_seq & 0x1);
	smp_mb(); /* Ensure PCU read-side usage follows above initialization. */
}

/*
 * The outgoing function has no further need of PCU, so remove it from
 * the pcu_node tree's ->qsmaskinitnext bit masks.
 *
 * Note that this function is special in that it is invoked directly
 * from the outgoing CPU rather than from the cpuhp_step mechanism.
 * This is because this function must be invoked at a precise location.
 */
void pcu_report_dead(unsigned int cpu)
{
	unsigned long flags;
	unsigned long mask;
	struct pcu_data *rdp = per_cpu_ptr(&pcu_data, cpu);
	struct pcu_node *rnp = rdp->mynode;  /* Outgoing CPU's rdp & rnp. */

	// Do any dangling deferred wakeups.
	do_nocb_deferred_wakeup(rdp);

	/* QS for any half-done expedited grace period. */
	preempt_disable();
	pcu_report_exp_rdp(this_cpu_ptr(&pcu_data));
	preempt_enable();
	pcu_preempt_deferred_qs(current);

	/* Remove outgoing CPU from mask in the leaf pcu_node structure. */
	mask = rdp->grpmask;
	WRITE_ONCE(rnp->ofl_seq, rnp->ofl_seq + 1);
	WARN_ON_ONCE(!(rnp->ofl_seq & 0x1));
	smp_mb(); // Pair with pcu_gp_cleanup()'s ->ofl_seq barrier().
	raw_spin_lock(&pcu_state.ofl_lock);
	raw_spin_lock_irqsave_pcu_node(rnp, flags); /* Enforce GP memory-order guarantee. */
	rdp->pcu_ofl_gp_seq = READ_ONCE(pcu_state.gp_seq);
	rdp->pcu_ofl_gp_flags = READ_ONCE(pcu_state.gp_flags);
	if (rnp->qsmask & mask) { /* PCU waiting on outgoing CPU? */
		/* Report quiescent state -before- changing ->qsmaskinitnext! */
		pcu_report_qs_rnp(mask, rnp, rnp->gp_seq, flags);
		raw_spin_lock_irqsave_pcu_node(rnp, flags);
	}
	WRITE_ONCE(rnp->qsmaskinitnext, rnp->qsmaskinitnext & ~mask);
	raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
	raw_spin_unlock(&pcu_state.ofl_lock);
	smp_mb(); // Pair with pcu_gp_cleanup()'s ->ofl_seq barrier().
	WRITE_ONCE(rnp->ofl_seq, rnp->ofl_seq + 1);
	WARN_ON_ONCE(rnp->ofl_seq & 0x1);

	rdp->cpu_started = false;
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * The outgoing CPU has just passed through the dying-idle state, and we
 * are being invoked from the CPU that was IPIed to continue the offline
 * operation.  Migrate the outgoing CPU's callbacks to the current CPU.
 */
void pcutree_migrate_callbacks(int cpu)
{
	unsigned long flags;
	struct pcu_data *my_rdp;
	struct pcu_node *my_rnp;
	struct pcu_data *rdp = per_cpu_ptr(&pcu_data, cpu);
	bool needwake;

	if (pcu_rdp_is_offloaded(rdp) ||
	    pcu_segcblist_empty(&rdp->cblist))
		return;  /* No callbacks to migrate. */

	local_irq_save(flags);
	my_rdp = this_cpu_ptr(&pcu_data);
	my_rnp = my_rdp->mynode;
	pcu_nocb_lock(my_rdp); /* irqs already disabled. */
	WARN_ON_ONCE(!pcu_nocb_flush_bypass(my_rdp, NULL, jiffies));
	raw_spin_lock_pcu_node(my_rnp); /* irqs already disabled. */
	/* Leverage recent GPs and set GP for new callbacks. */
	needwake = pcu_advance_cbs(my_rnp, rdp) ||
		   pcu_advance_cbs(my_rnp, my_rdp);
	pcu_segcblist_merge(&my_rdp->cblist, &rdp->cblist);
	needwake = needwake || pcu_advance_cbs(my_rnp, my_rdp);
	pcu_segcblist_disable(&rdp->cblist);
	WARN_ON_ONCE(pcu_segcblist_empty(&my_rdp->cblist) !=
		     !pcu_segcblist_n_cbs(&my_rdp->cblist));
	if (pcu_rdp_is_offloaded(my_rdp)) {
		raw_spin_unlock_pcu_node(my_rnp); /* irqs remain disabled. */
		__call_pcu_nocb_wake(my_rdp, true, flags);
	} else {
		pcu_nocb_unlock(my_rdp); /* irqs remain disabled. */
		raw_spin_unlock_irqrestore_pcu_node(my_rnp, flags);
	}
	if (needwake)
		pcu_gp_kthread_wake();
	lockdep_assert_irqs_enabled();
	WARN_ONCE(pcu_segcblist_n_cbs(&rdp->cblist) != 0 ||
		  !pcu_segcblist_empty(&rdp->cblist),
		  "pcu_cleanup_dead_cpu: Callbacks on offline CPU %d: qlen=%lu, 1stCB=%p\n",
		  cpu, pcu_segcblist_n_cbs(&rdp->cblist),
		  pcu_segcblist_first_cb(&rdp->cblist));
}
#endif

/*
 * On non-huge systems, use expedited PCU grace periods to make suspend
 * and hibernation run faster.
 */
static int pcu_pm_notify(struct notifier_block *self,
			 unsigned long action, void *hcpu)
{
	switch (action) {
	case PM_HIBERNATION_PREPARE:
	case PM_SUSPEND_PREPARE:
		pcu_expedite_gp();
		break;
	case PM_POST_HIBERNATION:
	case PM_POST_SUSPEND:
		pcu_unexpedite_gp();
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

/* ych	*/
struct task_struct *pcu_gp_kthread_task;
/* ych	*/

/*
 * Spawn the kthreads that handle PCU's grace periods.
 */
int pcu_spawn_gp_kthread(void)
{
	unsigned long flags;
	int kthread_prio_in = kthread_prio;
	struct pcu_node *rnp;
	struct sched_param sp;
	struct task_struct *t;
	printk("[%s] ych_1\n", __func__);

	/* Force priority into range. */
	if (IS_ENABLED(CONFIG_PCU_BOOST) && kthread_prio < 2
	    && IS_BUILTIN(CONFIG_PCU_TORTURE_TEST))
		kthread_prio = 2;
	else if (IS_ENABLED(CONFIG_PCU_BOOST) && kthread_prio < 1)
		kthread_prio = 1;
	else if (kthread_prio < 0)
		kthread_prio = 0;
	else if (kthread_prio > 99)
		kthread_prio = 99;
	printk("[%s] ych_1, kthread_prio = %d\n", __func__, kthread_prio);	
//	printk("[%s] pcu_state.name = %s\n", __func__, pcu_state.name);

#if 1
	if (kthread_prio != kthread_prio_in)
		pr_alert("pcu_spawn_gp_kthread(): Limited prio to %d from %d\n",
			 kthread_prio, kthread_prio_in);

	pcu_scheduler_fully_active = 1;
	t = kthread_create(pcu_gp_kthread, NULL, "%s", pcu_state.name);
	if (WARN_ONCE(IS_ERR(t), "%s: Could not start grace-period kthread, OOM is now expected behavior\n", __func__))
		return 0;

	/* ych	*/
	pcu_gp_kthread_task = t;
	/* ych	*/

	if (kthread_prio) {
		sp.sched_priority = kthread_prio;
		sched_setscheduler_nocheck(t, SCHED_FIFO, &sp);
	}
	rnp = pcu_get_root();
	raw_spin_lock_irqsave_pcu_node(rnp, flags);
	WRITE_ONCE(pcu_state.gp_activity, jiffies);
	WRITE_ONCE(pcu_state.gp_req_activity, jiffies);
	// Reset .gp_activity and .gp_req_activity before setting .gp_kthread.
	smp_store_release(&pcu_state.gp_kthread, t);  /* ^^^ */
	raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
	wake_up_process(t);
	pcu_spawn_nocb_kthreads();
	pcu_spawn_boost_kthreads();
	pcu_spawn_core_kthreads();
#endif
	return 0;
}
//early_initcall(pcu_spawn_gp_kthread);

/*
 * This function is invoked towards the end of the scheduler's
 * initialization process.  Before this is called, the idle task might
 * contain synchronous grace-period primitives (during which time, this idle
 * task is booting the system, and such primitives are no-ops).  After this
 * function is called, any synchronous grace-period primitives are run as
 * expedited, with the requesting task driving the grace period forward.
 * A later core_initcall() pcu_set_runtime_mode() will switch to full
 * runtime PCU functionality.
 */
void pcu_scheduler_starting(void)
{
	WARN_ON(num_online_cpus() != 1);
	WARN_ON(nr_context_switches() > 0);
	pcu_test_sync_prims();
	pcu_scheduler_active = PCU_SCHEDULER_INIT;
	pcu_test_sync_prims();
}

/*
 * Helper function for pcu_init() that initializes the pcu_state structure.
 */
static void pcu_init_one(void)
{
	static const char * const buf[] = PCU_NODE_NAME_INIT;
	static const char * const fqs[] = PCU_FQS_NAME_INIT;
	static struct lock_class_key pcu_node_class[PCU_NUM_LVLS];
	static struct lock_class_key pcu_fqs_class[PCU_NUM_LVLS];

	int levelspread[PCU_NUM_LVLS];		/* kids/node in each level. */
	int cpustride = 1;
	int i;
	int j;
	struct pcu_node *rnp;
	printk("[%s] ych_1\n", __func__);

	BUILD_BUG_ON(PCU_NUM_LVLS > ARRAY_SIZE(buf));  /* Fix buf[] init! */

	/* Silence gcc 4.8 false positive about array index out of range. */
	if (pcu_num_lvls <= 0 || pcu_num_lvls > PCU_NUM_LVLS)
		panic("pcu_init_one: pcu_num_lvls out of range");

	/* Initialize the level-tracking arrays. */
	for (i = 1; i < pcu_num_lvls; i++)
		pcu_state.level[i] =
			pcu_state.level[i - 1] + num_pcu_lvl[i - 1];
	pcu_init_levelspread(levelspread, num_pcu_lvl);

	/* Initialize the elements themselves, starting from the leaves. */

	for (i = pcu_num_lvls - 1; i >= 0; i--) {
		cpustride *= levelspread[i];
		rnp = pcu_state.level[i];
		for (j = 0; j < num_pcu_lvl[i]; j++, rnp++) {
			raw_spin_lock_init(&ACCESS_PRIVATE(rnp, lock));
			lockdep_set_class_and_name(&ACCESS_PRIVATE(rnp, lock),
						   &pcu_node_class[i], buf[i]);
			raw_spin_lock_init(&rnp->fqslock);
			lockdep_set_class_and_name(&rnp->fqslock,
						   &pcu_fqs_class[i], fqs[i]);
			rnp->gp_seq = pcu_state.gp_seq;
			rnp->gp_seq_needed = pcu_state.gp_seq;
			rnp->completedqs = pcu_state.gp_seq;
			rnp->qsmask = 0;
			rnp->qsmaskinit = 0;
			rnp->grplo = j * cpustride;
			rnp->grphi = (j + 1) * cpustride - 1;
			if (rnp->grphi >= nr_cpu_ids)
				rnp->grphi = nr_cpu_ids - 1;
			if (i == 0) {
				rnp->grpnum = 0;
				rnp->grpmask = 0;
				rnp->parent = NULL;
			} else {
				rnp->grpnum = j % levelspread[i - 1];
				rnp->grpmask = BIT(rnp->grpnum);
				rnp->parent = pcu_state.level[i - 1] +
					      j / levelspread[i - 1];
			}
			rnp->level = i;
			INIT_LIST_HEAD(&rnp->blkd_tasks);
			pcu_init_one_nocb(rnp);
			init_waitqueue_head(&rnp->exp_wq[0]);
			init_waitqueue_head(&rnp->exp_wq[1]);
			init_waitqueue_head(&rnp->exp_wq[2]);
			init_waitqueue_head(&rnp->exp_wq[3]);
			spin_lock_init(&rnp->exp_lock);
		}
	}

	init_swait_queue_head(&pcu_state.gp_wq);
	init_swait_queue_head(&pcu_state.expedited_wq);
	rnp = pcu_first_leaf_node();
	for_each_possible_cpu(i) {
		while (i > rnp->grphi)
			rnp++;
		per_cpu_ptr(&pcu_data, i)->mynode = rnp;
		pcu_boot_init_percpu_data(i);
	}
}

/*
 * Compute the pcu_node tree geometry from kernel parameters.  This cannot
 * replace the definitions in tree.h because those are needed to size
 * the ->node array in the pcu_state structure.
 */
void pcu_init_geometry(void)
{
	ulong d;
	int i;
	static unsigned long old_nr_cpu_ids;
	int pcu_capacity[PCU_NUM_LVLS];
	static bool initialized;

	printk("[%s] ych_1\n", __func__);
	if (initialized) {
		/*
		 * Warn if setup_nr_cpu_ids() had not yet been invoked,
		 * unless nr_cpus_ids == NR_CPUS, in which case who cares?
		 */
		WARN_ON_ONCE(old_nr_cpu_ids != nr_cpu_ids);
		return;
	}

	old_nr_cpu_ids = nr_cpu_ids;
	initialized = true;

	/*
	 * Initialize any unspecified boot parameters.
	 * The default values of jiffies_till_first_fqs and
	 * jiffies_till_next_fqs are set to the PCU_JIFFIES_TILL_FORCE_QS
	 * value, which is a function of HZ, then adding one for each
	 * PCU_JIFFIES_FQS_DIV CPUs that might be on the system.
	 */
	d = PCU_JIFFIES_TILL_FORCE_QS + nr_cpu_ids / PCU_JIFFIES_FQS_DIV;
	if (jiffies_till_first_fqs == ULONG_MAX)
		jiffies_till_first_fqs = d;
	if (jiffies_till_next_fqs == ULONG_MAX)
		jiffies_till_next_fqs = d;
	adjust_jiffies_till_sched_qs();

	/* If the compile-time values are accurate, just leave. */
	printk("[%s] pcu_fanout_leaf = %d, PCU_FANOUT_LEAF = %d\n",
			__func__, pcu_fanout_leaf, PCU_FANOUT_LEAF);
	if (pcu_fanout_leaf == PCU_FANOUT_LEAF &&
	    nr_cpu_ids == NR_CPUS)
		return;
	pr_info("Adjusting geometry for pcu_fanout_leaf=%d, nr_cpu_ids=%u\n",
		pcu_fanout_leaf, nr_cpu_ids);

	/*
	 * The boot-time pcu_fanout_leaf parameter must be at least two
	 * and cannot exceed the number of bits in the pcu_node masks.
	 * Complain and fall back to the compile-time values if this
	 * limit is exceeded.
	 */
	if (pcu_fanout_leaf < 2 ||
	    pcu_fanout_leaf > sizeof(unsigned long) * 8) {
		pcu_fanout_leaf = PCU_FANOUT_LEAF;
		WARN_ON(1);
		return;
	}

	/*
	 * Compute number of nodes that can be handled an pcu_node tree
	 * with the given number of levels.
	 */
	pcu_capacity[0] = pcu_fanout_leaf;
	for (i = 1; i < PCU_NUM_LVLS; i++)
		pcu_capacity[i] = pcu_capacity[i - 1] * PCU_FANOUT;

	/*
	 * The tree must be able to accommodate the configured number of CPUs.
	 * If this limit is exceeded, fall back to the compile-time values.
	 */
	if (nr_cpu_ids > pcu_capacity[PCU_NUM_LVLS - 1]) {
		pcu_fanout_leaf = PCU_FANOUT_LEAF;
		WARN_ON(1);
		return;
	}

	/* Calculate the number of levels in the tree. */
	for (i = 0; nr_cpu_ids > pcu_capacity[i]; i++) {
	}
	pcu_num_lvls = i + 1;
	printk("[%s] ych_8, pcu_num_lvls = %d\n", __func__, pcu_num_lvls);

	/* Calculate the number of pcu_nodes at each level of the tree. */
	for (i = 0; i < pcu_num_lvls; i++) {
		int cap = pcu_capacity[(pcu_num_lvls - 1) - i];
		printk("[%s] ych_9\n", __func__);
		num_pcu_lvl[i] = DIV_ROUND_UP(nr_cpu_ids, cap);
	}

	/* Calculate the total number of pcu_node structures. */
	pcu_num_nodes = 0;
	for (i = 0; i < pcu_num_lvls; i++)
		pcu_num_nodes += num_pcu_lvl[i];
}

/*
 * Dump out the structure of the pcu_node combining tree associated
 * with the pcu_state structure.
 */
static void pcu_dump_pcu_node_tree(void)
{
	int level = 0;
	struct pcu_node *rnp;

	pr_info("pcu_node tree layout dump\n");
	pr_info(" ");
	pcu_for_each_node_breadth_first(rnp) {
		if (rnp->level != level) {
			pr_cont("\n");
			pr_info(" ");
			level = rnp->level;
		}
		pr_cont("%d:%d ^%d  ", rnp->grplo, rnp->grphi, rnp->grpnum);
	}
	pr_cont("\n");
}

struct workqueue_struct *pcu_gp_wq;
struct workqueue_struct *pcu_par_gp_wq;

static void kfree_pcu_batch_init(void)
{
	int cpu;
	int i;

//	/* Clamp it to [0:100] seconds interval. */
//	if (pcu_delay_page_cache_fill_msec < 0 ||
//		pcu_delay_page_cache_fill_msec > 100 * MSEC_PER_SEC) {
//
//		pcu_delay_page_cache_fill_msec =
//			clamp(pcu_delay_page_cache_fill_msec, 0,
//				(int) (100 * MSEC_PER_SEC));
//
//		pr_info("Adjusting pcutree.pcu_delay_page_cache_fill_msec to %d ms.\n",
//			pcu_delay_page_cache_fill_msec);
//	}

	for_each_possible_cpu(cpu) {
		struct kfree_pcu_cpu *krcp = per_cpu_ptr(&krc, cpu);
		//printk("[%s] ych_3\n", __func__);

		for (i = 0; i < KFREE_N_BATCHES; i++) {
			//printk("[%s] ych_4\n", __func__);
			INIT_RCU_WORK(&krcp->krw_arr[i].pcu_work, kfree_pcu_work);
			krcp->krw_arr[i].krcp = krcp;
		}

		INIT_DELAYED_WORK(&krcp->monitor_work, kfree_pcu_monitor);
		INIT_DELAYED_WORK(&krcp->page_cache_work, fill_page_cache_func);
		krcp->initialized = true;
	}
	if (register_shrinker(&kfree_pcu_shrinker))
		//printk("[%s] ych_5\n", __func__);
		pr_err("Failed to register kfree_pcu() shrinker!\n");
}

void pcu_init(void)
{
	int cpu;
	printk("[%s] ych_1\n", __func__);

	//pcu_early_boot_tests();

	kfree_pcu_batch_init();

	pcu_bootup_announce();
	pcu_init_geometry();
	pcu_init_one();
	if (dump_tree)
		pcu_dump_pcu_node_tree();
	if (use_softirq){
		printk("[%s] ych_2\n", __func__);
		open_softirq(PCU_SOFTIRQ, pcu_core_si);
	}

	/*
	 * We don't need protection against CPU-hotplug here because
	 * this is called early in boot, before either interrupts
	 * or the scheduler are operational.
	 */
	pm_notifier(pcu_pm_notify, 0);
	for_each_online_cpu(cpu) {
		pcutree_prepare_cpu(cpu);
		pcu_cpu_starting(cpu);
		pcutree_online_cpu(cpu);
	}

	/* Create workqueue for Tree SPCU and for expedited GPs. */
	printk("[%s] ych_3\n", __func__);
	pcu_gp_wq = alloc_workqueue("pcu_gp", WQ_MEM_RECLAIM, 0);
	WARN_ON(!pcu_gp_wq);
	printk("[%s] ych_4\n", __func__);
	pcu_par_gp_wq = alloc_workqueue("pcu_par_gp", WQ_MEM_RECLAIM, 0);
	WARN_ON(!pcu_par_gp_wq);
#if 0

	/* Fill in default value for pcutree.qovld boot parameter. */
	/* -After- the pcu_node ->lock fields are initialized! */
	if (qovld < 0)
		qovld_calc = DEFAULT_PCU_QOVLD_MULT * qhimark;
	else
		qovld_calc = qovld;
#endif
}

/* ych	*/
void pcu_wakeup_gp_kthread(void)
{
	swake_up_all(&pcu_state.gp_wq);
}
EXPORT_SYMBOL_GPL(pcu_wakeup_gp_kthread);
/* ych	*/

#include "tree_stall.h"
#include "tree_exp.h"
#include "tree_plugin.h"
