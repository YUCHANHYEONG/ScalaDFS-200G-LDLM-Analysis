/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Read-Copy Update mechanism for mutual exclusion, adapted for tracing.
 *
 * Copyright (C) 2020 Paul E. McKenney.
 */

#ifndef __LINUX_PCUPDATE_TRACE_H
#define __LINUX_PCUPDATE_TRACE_H

#include <linux/sched.h>
#include <linux/pcupdate.h>

#ifdef CONFIG_DEBUG_LOCK_ALLOC

extern struct lockdep_map pcu_trace_lock_map;

static inline int pcu_read_lock_trace_held(void)
{
	return lock_is_held(&pcu_trace_lock_map);
}

#else /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

static inline int pcu_read_lock_trace_held(void)
{
	return 1;
}

#endif /* #else #ifdef CONFIG_DEBUG_LOCK_ALLOC */

#ifdef CONFIG_TASKS_TRACE_PCU

void pcu_read_unlock_trace_special(struct task_struct *t, int nesting);

/**
 * pcu_read_lock_trace - mark beginning of PCU-trace read-side critical section
 *
 * When synchronize_pcu_tasks_trace() is invoked by one task, then that
 * task is guaranteed to block until all other tasks exit their read-side
 * critical sections.  Similarly, if call_pcu_trace() is invoked on one
 * task while other tasks are within PCU read-side critical sections,
 * invocation of the corresponding PCU callback is deferred until after
 * the all the other tasks exit their critical sections.
 *
 * For more details, please see the documentation for pcu_read_lock().
 */
static inline void pcu_read_lock_trace(void)
{
	struct task_struct_rh *t = current->task_struct_rh;

	WRITE_ONCE(t->trc_reader_nesting, READ_ONCE(t->trc_reader_nesting) + 1);
	barrier();
	if (IS_ENABLED(CONFIG_TASKS_TRACE_PCU_READ_MB) &&
	    t->trc_reader_special.b.need_mb)
		smp_mb(); // Pairs with update-side barriers
	pcu_lock_acquire(&pcu_trace_lock_map);
}

/**
 * pcu_read_unlock_trace - mark end of PCU-trace read-side critical section
 *
 * Pairs with a preceding call to pcu_read_lock_trace(), and nesting is
 * allowed.  Invoking a pcu_read_unlock_trace() when there is no matching
 * pcu_read_lock_trace() is verboten, and will result in lockdep complaints.
 *
 * For more details, please see the documentation for pcu_read_unlock().
 */
static inline void pcu_read_unlock_trace(void)
{
	int nesting;
	struct task_struct *t = current;
	struct task_struct_rh *t_rh = t->task_struct_rh;

	pcu_lock_release(&pcu_trace_lock_map);
	nesting = READ_ONCE(t_rh->trc_reader_nesting) - 1;
	barrier(); // Critical section before disabling.
	// Disable IPI-based setting of .need_qs.
	WRITE_ONCE(t_rh->trc_reader_nesting, INT_MIN);
	if (likely(!READ_ONCE(t_rh->trc_reader_special.s)) || nesting) {
		WRITE_ONCE(t_rh->trc_reader_nesting, nesting);
		return;  // We assume shallow reader nesting.
	}
	pcu_read_unlock_trace_special(t, nesting);
}

void call_pcu_tasks_trace(struct pcu_head *rhp, pcu_callback_t func);
void synchronize_pcu_tasks_trace(void);
void pcu_barrier_tasks_trace(void);

#endif /* #ifdef CONFIG_TASKS_TRACE_PCU */

#endif /* __LINUX_PCUPDATE_TRACE_H */
