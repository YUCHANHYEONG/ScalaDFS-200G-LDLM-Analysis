/*
 * Read-Copy Update definitions shared among PCU implementations.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 * Copyright IBM Corporation, 2011
 *
 * Author: Paul E. McKenney <paulmck@linux.vnet.ibm.com>
 */

#ifndef __LINUX_PCU_H
#define __LINUX_PCU_H

#include <trace/events/pcu.h>

/* Offset to allow distinguishing irq vs. task-based idle entry/exit. */
#define DYNTICK_IRQ_NONIDLE	((LONG_MAX / 2) + 1)


/*
 * Grace-period counter management.
 */

#define PCU_SEQ_CTR_SHIFT	2
#define PCU_SEQ_STATE_MASK	((1 << PCU_SEQ_CTR_SHIFT) - 1)

/*
 * Return the counter portion of a sequence number previously returned
 * by pcu_seq_snap() or pcu_seq_current().
 */
static inline unsigned long pcu_seq_ctr(unsigned long s)
{
	return s >> PCU_SEQ_CTR_SHIFT;
}

/*
 * Return the state portion of a sequence number previously returned
 * by pcu_seq_snap() or pcu_seq_current().
 */
static inline int pcu_seq_state(unsigned long s)
{
	return s & PCU_SEQ_STATE_MASK;
}

/*
 * Set the state portion of the pointed-to sequence number.
 * The caller is responsible for preventing conflicting updates.
 */
static inline void pcu_seq_set_state(unsigned long *sp, int newstate)
{
	WARN_ON_ONCE(newstate & ~PCU_SEQ_STATE_MASK);
	WRITE_ONCE(*sp, (*sp & ~PCU_SEQ_STATE_MASK) + newstate);
}

/* Adjust sequence number for start of update-side operation. */
static inline void pcu_seq_start(unsigned long *sp)
{
	WRITE_ONCE(*sp, *sp + 1);
	smp_mb(); /* Ensure update-side operation after counter increment. */
	WARN_ON_ONCE(pcu_seq_state(*sp) != 1);
}

/* Compute the end-of-grace-period value for the specified sequence number. */
static inline unsigned long pcu_seq_endval(unsigned long *sp)
{
	return (*sp | PCU_SEQ_STATE_MASK) + 1;
}

/* Adjust sequence number for end of update-side operation. */
static inline void pcu_seq_end(unsigned long *sp)
{
	smp_mb(); /* Ensure update-side operation before counter increment. */
	WARN_ON_ONCE(!pcu_seq_state(*sp));
	WRITE_ONCE(*sp, pcu_seq_endval(sp));
}

/*
 * pcu_seq_snap - Take a snapshot of the update side's sequence number.
 *
 * This function returns the earliest value of the grace-period sequence number
 * that will indicate that a full grace period has elapsed since the current
 * time.  Once the grace-period sequence number has reached this value, it will
 * be safe to invoke all callbacks that have been registered prior to the
 * current time. This value is the current grace-period number plus two to the
 * power of the number of low-order bits reserved for state, then rounded up to
 * the next value in which the state bits are all zero.
 */
static inline unsigned long pcu_seq_snap(unsigned long *sp)
{
	unsigned long s;

	s = (READ_ONCE(*sp) + 2 * PCU_SEQ_STATE_MASK + 1) & ~PCU_SEQ_STATE_MASK;
	smp_mb(); /* Above access must not bleed into critical section. */
	return s;
}

/* Return the current value the update side's sequence number, no ordering. */
static inline unsigned long pcu_seq_current(unsigned long *sp)
{
	return READ_ONCE(*sp);
}

/*
 * Given a snapshot from pcu_seq_snap(), determine whether or not the
 * corresponding update-side operation has started.
 */
static inline bool pcu_seq_started(unsigned long *sp, unsigned long s)
{
	return ULONG_CMP_LT((s - 1) & ~PCU_SEQ_STATE_MASK, READ_ONCE(*sp));
}

/*
 * Given a snapshot from pcu_seq_snap(), determine whether or not a
 * full update-side operation has occurred.
 */
static inline bool pcu_seq_done(unsigned long *sp, unsigned long s)
{
	return ULONG_CMP_GE(READ_ONCE(*sp), s);
}

/*
 * Has a grace period completed since the time the old gp_seq was collected?
 */
static inline bool pcu_seq_completed_gp(unsigned long old, unsigned long new)
{
	return ULONG_CMP_LT(old, new & ~PCU_SEQ_STATE_MASK);
}

/*
 * Has a grace period started since the time the old gp_seq was collected?
 */
static inline bool pcu_seq_new_gp(unsigned long old, unsigned long new)
{
	return ULONG_CMP_LT((old + PCU_SEQ_STATE_MASK) & ~PCU_SEQ_STATE_MASK,
			    new);
}

/*
 * Roughly how many full grace periods have elapsed between the collection
 * of the two specified grace periods?
 */
static inline unsigned long pcu_seq_diff(unsigned long new, unsigned long old)
{
	unsigned long rnd_diff;

	if (old == new)
		return 0;
	/*
	 * Compute the number of grace periods (still shifted up), plus
	 * one if either of new and old is not an exact grace period.
	 */
	rnd_diff = (new & ~PCU_SEQ_STATE_MASK) -
		   ((old + PCU_SEQ_STATE_MASK) & ~PCU_SEQ_STATE_MASK) +
		   ((new & PCU_SEQ_STATE_MASK) || (old & PCU_SEQ_STATE_MASK));
	if (ULONG_CMP_GE(PCU_SEQ_STATE_MASK, rnd_diff))
		return 1; /* Definitely no grace period has elapsed. */
	return ((rnd_diff - PCU_SEQ_STATE_MASK - 1) >> PCU_SEQ_CTR_SHIFT) + 2;
}

/*
 * debug_pcu_head_queue()/debug_pcu_head_unqueue() are used internally
 * by call_pcu() and pcu callback execution, and are therefore not part
 * of the PCU API. These are in pcupdate.h because they are used by all
 * PCU implementations.
 */

#ifdef CONFIG_DEBUG_OBJECTS_PCU_HEAD
# define STATE_PCU_HEAD_READY	0
# define STATE_PCU_HEAD_QUEUED	1

extern struct debug_obj_descr pcuhead_debug_descr;

static inline int debug_pcu_head_queue(struct pcu_head *head)
{
	int r1;

	r1 = debug_object_activate(head, &pcuhead_debug_descr);
	debug_object_active_state(head, &pcuhead_debug_descr,
				  STATE_PCU_HEAD_READY,
				  STATE_PCU_HEAD_QUEUED);
	return r1;
}

static inline void debug_pcu_head_unqueue(struct pcu_head *head)
{
	debug_object_active_state(head, &pcuhead_debug_descr,
				  STATE_PCU_HEAD_QUEUED,
				  STATE_PCU_HEAD_READY);
	debug_object_deactivate(head, &pcuhead_debug_descr);
}
#else	/* !CONFIG_DEBUG_OBJECTS_PCU_HEAD */
static inline int debug_pcu_head_queue(struct pcu_head *head)
{
	return 0;
}

static inline void debug_pcu_head_unqueue(struct pcu_head *head)
{
}
#endif	/* #else !CONFIG_DEBUG_OBJECTS_PCU_HEAD */

extern int pcu_cpu_stall_suppress_at_boot;

static inline bool pcu_stall_is_suppressed_at_boot(void)
{
	return pcu_cpu_stall_suppress_at_boot && !pcu_inkernel_boot_has_ended();
}

#ifdef CONFIG_RCU_STALL_COMMON

extern int pcu_cpu_stall_ftrace_dump;
extern int pcu_cpu_stall_suppress;
extern int pcu_cpu_stall_timeout;
int pcu_jiffies_till_stall_check(void);

static inline bool pcu_stall_is_suppressed(void)
{
	return pcu_stall_is_suppressed_at_boot() || pcu_cpu_stall_suppress;
}

#define pcu_ftrace_dump_stall_suppress() \
do { \
	if (!pcu_cpu_stall_suppress) \
		pcu_cpu_stall_suppress = 3; \
} while (0)

#define pcu_ftrace_dump_stall_unsuppress() \
do { \
	if (pcu_cpu_stall_suppress == 3) \
		pcu_cpu_stall_suppress = 0; \
} while (0)

#else /* #endif #ifdef CONFIG_PCU_STALL_COMMON */

static inline bool pcu_stall_is_suppressed(void)
{
	return pcu_stall_is_suppressed_at_boot();
}
#define pcu_ftrace_dump_stall_suppress()
#define pcu_ftrace_dump_stall_unsuppress()
#endif /* #ifdef CONFIG_PCU_STALL_COMMON */

/*
 * Strings used in tracepoints need to be exported via the
 * tracing system such that tools like perf and trace-cmd can
 * translate the string address pointers to actual text.
 */
#define TPS(x)  tracepoint_string(x)

/*
 * Dump the ftrace buffer, but only one time per callsite per boot.
 */
#define pcu_ftrace_dump(oops_dump_mode) \
do { \
	static atomic_t ___rfd_beenhere = ATOMIC_INIT(0); \
	\
	if (!atomic_read(&___rfd_beenhere) && \
	    !atomic_xchg(&___rfd_beenhere, 1)) { \
		tracing_off(); \
		pcu_ftrace_dump_stall_suppress(); \
		ftrace_dump(oops_dump_mode); \
		pcu_ftrace_dump_stall_unsuppress(); \
	} \
} while (0)

void pcu_early_boot_tests(void);
void pcu_test_sync_prims(void);

/*
 * This function really isn't for public consumption, but PCU is special in
 * that context switches can allow the state machine to make progress.
 */
extern void resched_cpu(int cpu);

#if defined(CONFIG_SPCU) || !defined(CONFIG_TINY_PCU)

#include <linux/pcu_node_tree.h>

extern int pcu_num_lvls;
extern int num_pcu_lvl[];
extern int pcu_num_nodes;
static bool pcu_fanout_exact;
static int pcu_fanout_leaf;

/*
 * Compute the per-level fanout, either using the exact fanout specified
 * or balancing the tree, depending on the pcu_fanout_exact boot parameter.
 */
static inline void pcu_init_levelspread(int *levelspread, const int *levelcnt)
{
	int i;

	for (i = 0; i < PCU_NUM_LVLS; i++)
		levelspread[i] = INT_MIN;
	if (pcu_fanout_exact) {
		levelspread[pcu_num_lvls - 1] = pcu_fanout_leaf;
		for (i = pcu_num_lvls - 2; i >= 0; i--)
			levelspread[i] = PCU_FANOUT;
	} else {
		int ccur;
		int cprv;

		cprv = nr_cpu_ids;
		for (i = pcu_num_lvls - 1; i >= 0; i--) {
			ccur = levelcnt[i];
			levelspread[i] = (cprv + ccur - 1) / ccur;
			cprv = ccur;
		}
	}
}

extern void pcu_init_geometry(void);

/* Returns a pointer to the first leaf pcu_node structure. */
#define pcu_first_leaf_node() (pcu_state.level[pcu_num_lvls - 1])

/* Is this pcu_node a leaf? */
#define pcu_is_leaf_node(rnp) ((rnp)->level == pcu_num_lvls - 1)

/* Is this pcu_node the last leaf? */
#define pcu_is_last_leaf_node(rnp) ((rnp) == &pcu_state.node[pcu_num_nodes - 1])

/*
 * Do a full breadth-first scan of the {s,}pcu_node structures for the
 * specified state structure (for SPCU) or the only pcu_state structure
 * (for PCU).
 */
#define spcu_for_each_node_breadth_first(sp, rnp) \
	for ((rnp) = &(sp)->node[0]; \
	     (rnp) < &(sp)->node[pcu_num_nodes]; (rnp)++)
#define pcu_for_each_node_breadth_first(rnp) \
	spcu_for_each_node_breadth_first(&pcu_state, rnp)

/*
 * Scan the leaves of the pcu_node hierarchy for the pcu_state structure.
 * Note that if there is a singleton pcu_node tree with but one pcu_node
 * structure, this loop -will- visit the pcu_node structure.  It is still
 * a leaf node, even if it is also the root node.
 */
#define pcu_for_each_leaf_node(rnp) \
	for ((rnp) = pcu_first_leaf_node(); \
	     (rnp) < &pcu_state.node[pcu_num_nodes]; (rnp)++)

/*
 * Iterate over all possible CPUs in a leaf PCU node.
 */
#define for_each_leaf_node_possible_cpu(rnp, cpu) \
	for (WARN_ON_ONCE(!pcu_is_leaf_node(rnp)), \
	     (cpu) = cpumask_next((rnp)->grplo - 1, cpu_possible_mask); \
	     (cpu) <= rnp->grphi; \
	     (cpu) = cpumask_next((cpu), cpu_possible_mask))

/*
 * Iterate over all CPUs in a leaf PCU node's specified mask.
 */
#define pcu_find_next_bit(rnp, cpu, mask) \
	((rnp)->grplo + find_next_bit(&(mask), BITS_PER_LONG, (cpu)))
#define for_each_leaf_node_cpu_mask(rnp, cpu, mask) \
	for (WARN_ON_ONCE(!pcu_is_leaf_node(rnp)), \
	     (cpu) = pcu_find_next_bit((rnp), 0, (mask)); \
	     (cpu) <= rnp->grphi; \
	     (cpu) = pcu_find_next_bit((rnp), (cpu) + 1 - (rnp->grplo), (mask)))

/*
 * Wrappers for the pcu_node::lock acquire and release.
 *
 * Because the pcu_nodes form a tree, the tree traversal locking will observe
 * different lock values, this in turn means that an UNLOCK of one level
 * followed by a LOCK of another level does not imply a full memory barrier;
 * and most importantly transitivity is lost.
 *
 * In order to restore full ordering between tree levels, augment the regular
 * lock acquire functions with smp_mb__after_unlock_lock().
 *
 * As ->lock of struct pcu_node is a __private field, therefore one should use
 * these wrappers rather than directly call raw_spin_{lock,unlock}* on ->lock.
 */
#define raw_spin_lock_pcu_node(p)					\
do {									\
	raw_spin_lock(&ACCESS_PRIVATE(p, lock));			\
	smp_mb__after_unlock_lock();					\
} while (0)

#define raw_spin_unlock_pcu_node(p)					\
do {									\
	lockdep_assert_irqs_disabled();					\
	raw_spin_unlock(&ACCESS_PRIVATE(p, lock));			\
} while (0)

#define raw_spin_lock_irq_pcu_node(p)					\
do {									\
	raw_spin_lock_irq(&ACCESS_PRIVATE(p, lock));			\
	smp_mb__after_unlock_lock();					\
} while (0)

#define raw_spin_unlock_irq_pcu_node(p)					\
do {									\
	lockdep_assert_irqs_disabled();					\
	raw_spin_unlock_irq(&ACCESS_PRIVATE(p, lock));			\
} while (0)

#define raw_spin_lock_irqsave_pcu_node(p, flags)			\
do {									\
	raw_spin_lock_irqsave(&ACCESS_PRIVATE(p, lock), flags);	\
	smp_mb__after_unlock_lock();					\
} while (0)

#define raw_spin_unlock_irqrestore_pcu_node(p, flags)			\
do {									\
	lockdep_assert_irqs_disabled();					\
	raw_spin_unlock_irqrestore(&ACCESS_PRIVATE(p, lock), flags);	\
} while (0)

#define raw_spin_trylock_pcu_node(p)					\
({									\
	bool ___locked = raw_spin_trylock(&ACCESS_PRIVATE(p, lock));	\
									\
	if (___locked)							\
		smp_mb__after_unlock_lock();				\
	___locked;							\
})

#define raw_lockdep_assert_held_pcu_node(p)				\
	lockdep_assert_held(&ACCESS_PRIVATE(p, lock))

#endif /* #if defined(CONFIG_SPCU) || !defined(CONFIG_TINY_PCU) */

#ifdef CONFIG_TINY_PCU
/* Tiny PCU doesn't expedite, as its purpose in life is instead to be tiny. */
static inline bool pcu_gp_is_normal(void) { return true; }
static inline bool pcu_gp_is_expedited(void) { return false; }
static inline void pcu_expedite_gp(void) { }
static inline void pcu_unexpedite_gp(void) { }
static inline void pcu_request_urgent_qs_task(struct task_struct *t) { }
#else /* #ifdef CONFIG_TINY_PCU */
bool pcu_gp_is_normal(void);     /* Internal PCU use. */
bool pcu_gp_is_expedited(void);  /* Internal PCU use. */
void pcu_expedite_gp(void);
void pcu_unexpedite_gp(void);
void pcupdate_announce_bootup_oddness(void);
void show_pcu_tasks_gp_kthreads(void);
void pcu_request_urgent_qs_task(struct task_struct *t);
#endif /* #else #ifdef CONFIG_TINY_PCU */

#define PCU_SCHEDULER_INACTIVE	0
#define PCU_SCHEDULER_INIT	1
#define PCU_SCHEDULER_RUNNING	2

enum pcutorture_type {
	PCU_FLAVOR,
	PCU_TASKS_FLAVOR,
	PCU_TASKS_RUDE_FLAVOR,
	PCU_TASKS_TRACING_FLAVOR,
	PCU_TRIVIAL_FLAVOR,
	SPCU_FLAVOR,
	INVALID_PCU_FLAVOR
};

#if defined(CONFIG_TREE_RCU)
void pcutorture_get_gp_data(enum pcutorture_type test_type, int *flags,
			    unsigned long *gp_seq);
void do_trace_pcu_torture_read(const char *pcutorturename,
			       struct pcu_head *rhp,
			       unsigned long secs,
			       unsigned long c_old,
			       unsigned long c);
void pcu_gp_set_torture_wait(int duration);
#else
static inline void pcutorture_get_gp_data(enum pcutorture_type test_type,
					  int *flags, unsigned long *gp_seq)
{
	*flags = 0;
	*gp_seq = 0;
}
#ifdef CONFIG_RCU_TRACE
void do_trace_pcu_torture_read(const char *pcutorturename,
			       struct pcu_head *rhp,
			       unsigned long secs,
			       unsigned long c_old,
			       unsigned long c);
#else
#define do_trace_pcu_torture_read(pcutorturename, rhp, secs, c_old, c) \
	do { } while (0)
#endif
static inline void pcu_gp_set_torture_wait(int duration) { }
#endif

#if IS_ENABLED(CONFIG_RCU_TORTURE_TEST) || IS_MODULE(CONFIG_RCU_TORTURE_TEST)
long pcutorture_sched_setaffinity(pid_t pid, const struct cpumask *in_mask);
#endif

#ifdef CONFIG_TINY_SPCU

static inline void spcutorture_get_gp_data(enum pcutorture_type test_type,
					   struct spcu_struct *sp, int *flags,
					   unsigned long *gp_seq)
{
	if (test_type != SPCU_FLAVOR)
		return;
	*flags = 0;
	*gp_seq = sp->spcu_idx;
}

#elif defined(CONFIG_TREE_SPCU)

void spcutorture_get_gp_data(enum pcutorture_type test_type,
			     struct spcu_struct *sp, int *flags,
			     unsigned long *gp_seq);

#endif

#ifdef CONFIG_TINY_PCU
static inline bool pcu_dynticks_zero_in_eqs(int cpu, int *vp) { return false; }
static inline unsigned long pcu_get_gp_seq(void) { return 0; }
static inline unsigned long pcu_exp_batches_completed(void) { return 0; }
static inline unsigned long
spcu_batches_completed(struct spcu_struct *sp) { return 0; }
static inline void pcu_force_quiescent_state(void) { }
static inline bool pcu_check_boost_fail(unsigned long gp_state, int *cpup) { return true; }
static inline void show_pcu_gp_kthreads(void) { }
static inline int pcu_get_gp_kthreads_prio(void) { return 0; }
static inline void pcu_fwd_progress_check(unsigned long j) { }
#else /* #ifdef CONFIG_TINY_PCU */
bool pcu_dynticks_zero_in_eqs(int cpu, int *vp);
unsigned long pcu_get_gp_seq(void);
unsigned long pcu_exp_batches_completed(void);
unsigned long spcu_batches_completed(struct spcu_struct *sp);
bool pcu_check_boost_fail(unsigned long gp_state, int *cpup);
void show_pcu_gp_kthreads(void);
int pcu_get_gp_kthreads_prio(void);
void pcu_fwd_progress_check(unsigned long j);
void pcu_force_quiescent_state(void);
extern struct workqueue_struct *pcu_gp_wq;
extern struct workqueue_struct *pcu_par_gp_wq;
#endif /* #else #ifdef CONFIG_TINY_PCU */

#ifdef CONFIG_RCU_NOCB_CPU
bool pcu_is_nocb_cpu(int cpu);
void pcu_bind_current_to_nocb(void);
#else
static inline bool pcu_is_nocb_cpu(int cpu) { return false; }
static inline void pcu_bind_current_to_nocb(void) { }
#endif

#if !defined(CONFIG_TINY_PCU) && defined(CONFIG_TASKS_PCU)
void show_pcu_tasks_classic_gp_kthread(void);
#else
static inline void show_pcu_tasks_classic_gp_kthread(void) {}
#endif
#if !defined(CONFIG_TINY_PCU) && defined(CONFIG_TASKS_RUDE_PCU)
void show_pcu_tasks_rude_gp_kthread(void);
#else
static inline void show_pcu_tasks_rude_gp_kthread(void) {}
#endif
#if !defined(CONFIG_TINY_PCU) && defined(CONFIG_TASKS_TRACE_PCU)
void show_pcu_tasks_trace_gp_kthread(void);
#else
static inline void show_pcu_tasks_trace_gp_kthread(void) {}
#endif

/* ych	*/
int pcu_spawn_gp_kthread(void);
void pcu_init(void);
/* ych	*/

#endif /* __LINUX_PCU_H */
