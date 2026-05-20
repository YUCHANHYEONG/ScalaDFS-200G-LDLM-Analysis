/*
 * Read-Copy Update mechanism for mutual exclusion, the Bloatwatch edition.
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
 * Copyright IBM Corporation, 2008
 *
 * Author: Paul E. McKenney <paulmck@linux.vnet.ibm.com>
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 *		Documentation/PCU
 */
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/notifier.h>
#include <linux/pcupdate_wait.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/time.h>
#include <linux/cpu.h>
#include <linux/prefetch.h>
#include <linux/slab.h>
#include <linux/mm.h>

#include "pcu.h"

/* Global control variables for pcupdate callback mechanism. */
struct pcu_ctrlblk {
	struct pcu_head *pcucblist;	/* List of pending callbacks (CBs). */
	struct pcu_head **donetail;	/* ->next pointer of last "done" CB. */
	struct pcu_head **curtail;	/* ->next pointer of last CB. */
};

/* Definition for pcupdate control block. */
static struct pcu_ctrlblk pcu_ctrlblk = {
	.donetail	= &pcu_ctrlblk.pcucblist,
	.curtail	= &pcu_ctrlblk.pcucblist,
};

void pcu_barrier(void)
{
	wait_pcu_gp(call_pcu);
}
EXPORT_SYMBOL(pcu_barrier);

/* Record an pcu quiescent state.  */
void pcu_qs(void)
{
	unsigned long flags;

	local_irq_save(flags);
	if (pcu_ctrlblk.donetail != pcu_ctrlblk.curtail) {
		pcu_ctrlblk.donetail = pcu_ctrlblk.curtail;
		raise_softirq_irqoff(PCU_SOFTIRQ);
	}
	local_irq_restore(flags);
}

/*
 * Check to see if the scheduling-clock interrupt came from an extended
 * quiescent state, and, if so, tell PCU about it.  This function must
 * be called from hardirq context.  It is normally called from the
 * scheduling-clock interrupt.
 */
void pcu_sched_clock_irq(int user)
{
	if (user) {
		pcu_qs();
	} else if (pcu_ctrlblk.donetail != pcu_ctrlblk.curtail) {
		set_tsk_need_resched(current);
		set_preempt_need_resched();
	}
}

/*
 * Reclaim the specified callback, either by invoking it for non-kfree cases or
 * freeing it directly (for kfree). Return true if kfreeing, false otherwise.
 */
static inline bool pcu_reclaim_tiny(struct pcu_head *head)
{
	pcu_callback_t f;
	unsigned long offset = (unsigned long)head->func;

	pcu_lock_acquire(&pcu_callback_map);
	if (__is_kvfree_pcu_offset(offset)) {
		trace_pcu_invoke_kvfree_callback("", head, offset);
		kvfree((void *)head - offset);
		pcu_lock_release(&pcu_callback_map);
		return true;
	}

	trace_pcu_invoke_callback("", head);
	f = head->func;
	WRITE_ONCE(head->func, (pcu_callback_t)0L);
	f(head);
	pcu_lock_release(&pcu_callback_map);
	return false;
}

/* Invoke the PCU callbacks whose grace period has elapsed.  */
static __latent_entropy void pcu_process_callbacks(struct softirq_action *unused)
{
	struct pcu_head *next, *list;
	unsigned long flags;

	/* Move the ready-to-invoke callbacks to a local list. */
	local_irq_save(flags);
	if (pcu_ctrlblk.donetail == &pcu_ctrlblk.pcucblist) {
		/* No callbacks ready, so just leave. */
		local_irq_restore(flags);
		return;
	}
	list = pcu_ctrlblk.pcucblist;
	pcu_ctrlblk.pcucblist = *pcu_ctrlblk.donetail;
	*pcu_ctrlblk.donetail = NULL;
	if (pcu_ctrlblk.curtail == pcu_ctrlblk.donetail)
		pcu_ctrlblk.curtail = &pcu_ctrlblk.pcucblist;
	pcu_ctrlblk.donetail = &pcu_ctrlblk.pcucblist;
	local_irq_restore(flags);

	/* Invoke the callbacks on the local list. */
	while (list) {
		next = list->next;
		prefetch(next);
		debug_pcu_head_unqueue(list);
		local_bh_disable();
		pcu_reclaim_tiny(list);
		local_bh_enable();
		list = next;
	}
}

/*
 * Wait for a grace period to elapse.  But it is illegal to invoke
 * synchronize_pcu() from within an PCU read-side critical section.
 * Therefore, any legal call to synchronize_pcu() is a quiescent
 * state, and so on a UP system, synchronize_pcu() need do nothing.
 * (But Lai Jiangshan points out the benefits of doing might_sleep()
 * to reduce latency.)
 *
 * Cool, huh?  (Due to Josh Triplett.)
 */
void synchronize_pcu(void)
{
	PCU_LOCKDEP_WARN(lock_is_held(&pcu_bh_lock_map) ||
			 lock_is_held(&pcu_lock_map) ||
			 lock_is_held(&pcu_sched_lock_map),
			 "Illegal synchronize_pcu() in PCU read-side critical section");
}
EXPORT_SYMBOL_GPL(synchronize_pcu);

/*
 * Post an PCU callback to be invoked after the end of an PCU grace
 * period.  But since we have but one CPU, that would be after any
 * quiescent state.
 */
void call_pcu(struct pcu_head *head, pcu_callback_t func)
{
	unsigned long flags;

	debug_pcu_head_queue(head);
	head->func = func;
	head->next = NULL;

	local_irq_save(flags);
	*pcu_ctrlblk.curtail = head;
	pcu_ctrlblk.curtail = &head->next;
	local_irq_restore(flags);

	if (unlikely(is_idle_task(current))) {
		/* force scheduling for pcu_qs() */
		resched_cpu(0);
	}
}
EXPORT_SYMBOL_GPL(call_pcu);

void pcu_init(void)
{
	open_softirq(PCU_SOFTIRQ, pcu_process_callbacks);
	pcu_early_boot_tests();
}
