/*
 * Sleepable Read-Copy Update mechanism for mutual exclusion,
 *	tiny version for non-preemptible single-CPU use.
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
 * Copyright (C) IBM Corporation, 2017
 *
 * Author: Paul McKenney <paulmck@us.ibm.com>
 */

#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/preempt.h>
#include <linux/pcupdate_wait.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/spcu.h>

#include <linux/pcu_node_tree.h>
#include "pcu_segcblist.h"
#include "pcu.h"

int pcu_scheduler_active __read_mostly;
static LIST_HEAD(spcu_boot_list);
static bool spcu_init_done;

static int init_spcu_struct_fields(struct spcu_struct *ssp)
{
	ssp->spcu_lock_nesting[0] = 0;
	ssp->spcu_lock_nesting[1] = 0;
	init_swait_queue_head(&ssp->spcu_wq);
	ssp->spcu_cb_head = NULL;
	ssp->spcu_cb_tail = &ssp->spcu_cb_head;
	ssp->spcu_gp_running = false;
	ssp->spcu_gp_waiting = false;
	ssp->spcu_idx = 0;
	ssp->spcu_idx_max = 0;
	INIT_WORK(&ssp->spcu_work, spcu_drive_gp);
	INIT_LIST_HEAD(&ssp->spcu_work.entry);
	return 0;
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC

int _spcu_struct(struct spcu_struct *ssp, const char *name,
		       struct lock_class_key *key)
{
	/* Don't re-initialize a lock while it is held. */
	debug_check_no_locks_freed((void *)ssp, sizeof(*ssp));
	lockdep_init_map(&ssp->dep_map, name, key, 0);
	return init_spcu_struct_fields(ssp);
}
EXPORT_SYMBOL_GPL(_spcu_struct);

#else /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

/*
 * init_spcu_struct - initialize a sleep-PCU structure
 * @ssp: structure to initialize.
 *
 * Must invoke this on a given spcu_struct before passing that spcu_struct
 * to any other function.  Each spcu_struct represents a separate domain
 * of SPCU protection.
 */
int init_spcu_struct(struct spcu_struct *ssp)
{
	return init_spcu_struct_fields(ssp);
}
EXPORT_SYMBOL_GPL(init_spcu_struct);

#endif /* #else #ifdef CONFIG_DEBUG_LOCK_ALLOC */

/*
 * cleanup_spcu_struct - deconstruct a sleep-PCU structure
 * @ssp: structure to clean up.
 *
 * Must invoke this after you are finished using a given spcu_struct that
 * was initialized via init_spcu_struct(), else you leak memory.
 */
void cleanup_spcu_struct(struct spcu_struct *ssp)
{
	WARN_ON(ssp->spcu_lock_nesting[0] || ssp->spcu_lock_nesting[1]);
	flush_work(&ssp->spcu_work);
	WARN_ON(ssp->spcu_gp_running);
	WARN_ON(ssp->spcu_gp_waiting);
	WARN_ON(ssp->spcu_cb_head);
	WARN_ON(&ssp->spcu_cb_head != ssp->spcu_cb_tail);
	WARN_ON(ssp->spcu_idx != ssp->spcu_idx_max);
	WARN_ON(ssp->spcu_idx & 0x1);
}
EXPORT_SYMBOL_GPL(cleanup_spcu_struct);

/*
 * Removes the count for the old reader from the appropriate element of
 * the spcu_struct.
 */
void __spcu_read_unlock(struct spcu_struct *ssp, int idx)
{
	int newval = ssp->spcu_lock_nesting[idx] - 1;

	WRITE_ONCE(ssp->spcu_lock_nesting[idx], newval);
	if (!newval && READ_ONCE(ssp->spcu_gp_waiting))
		swake_up_one(&ssp->spcu_wq);
}
EXPORT_SYMBOL_GPL(__spcu_read_unlock);

/*
 * Workqueue handler to drive one grace period and invoke any callbacks
 * that become ready as a result.  Single-CPU and !PREEMPTION operation
 * means that we get away with murder on synchronization.  ;-)
 */
void spcu_drive_gp(struct work_struct *wp)
{
	int idx;
	struct pcu_head *lh;
	struct pcu_head *rhp;
	struct spcu_struct *ssp;

	ssp = container_of(wp, struct spcu_struct, spcu_work);
	if (ssp->spcu_gp_running || USHORT_CMP_GE(ssp->spcu_idx, READ_ONCE(ssp->spcu_idx_max)))
		return; /* Already running or nothing to do. */

	/* Remove recently arrived callbacks and wait for readers. */
	WRITE_ONCE(ssp->spcu_gp_running, true);
	local_irq_disable();
	lh = ssp->spcu_cb_head;
	ssp->spcu_cb_head = NULL;
	ssp->spcu_cb_tail = &ssp->spcu_cb_head;
	local_irq_enable();
	idx = (ssp->spcu_idx & 0x2) / 2;
	WRITE_ONCE(ssp->spcu_idx, ssp->spcu_idx + 1);
	WRITE_ONCE(ssp->spcu_gp_waiting, true);  /* spcu_read_unlock() wakes! */
	swait_event_exclusive(ssp->spcu_wq, !READ_ONCE(ssp->spcu_lock_nesting[idx]));
	WRITE_ONCE(ssp->spcu_gp_waiting, false); /* spcu_read_unlock() cheap. */
	WRITE_ONCE(ssp->spcu_idx, ssp->spcu_idx + 1);

	/* Invoke the callbacks we removed above. */
	while (lh) {
		rhp = lh;
		lh = lh->next;
		local_bh_disable();
		rhp->func(rhp);
		local_bh_enable();
	}

	/*
	 * Enable rescheduling, and if there are more callbacks,
	 * reschedule ourselves.  This can race with a call_spcu()
	 * at interrupt level, but the ->spcu_gp_running checks will
	 * straighten that out.
	 */
	WRITE_ONCE(ssp->spcu_gp_running, false);
	if (USHORT_CMP_LT(ssp->spcu_idx, READ_ONCE(ssp->spcu_idx_max)))
		schedule_work(&ssp->spcu_work);
}
EXPORT_SYMBOL_GPL(spcu_drive_gp);

static void spcu_gp_start_if_needed(struct spcu_struct *ssp)
{
	unsigned short cookie;

	cookie = get_state_synchronize_spcu(ssp);
	if (USHORT_CMP_GE(READ_ONCE(ssp->spcu_idx_max), cookie))
		return;
	WRITE_ONCE(ssp->spcu_idx_max, cookie);
	if (!READ_ONCE(ssp->spcu_gp_running)) {
		if (likely(spcu_init_done))
			schedule_work(&ssp->spcu_work);
		else if (list_empty(&ssp->spcu_work.entry))
			list_add(&ssp->spcu_work.entry, &spcu_boot_list);
	}
}

/*
 * Enqueue an SPCU callback on the specified spcu_struct structure,
 * initiating grace-period processing if it is not already running.
 */
void call_spcu(struct spcu_struct *ssp, struct pcu_head *rhp,
	       pcu_callback_t func)
{
	unsigned long flags;

	rhp->func = func;
	rhp->next = NULL;
	local_irq_save(flags);
	*ssp->spcu_cb_tail = rhp;
	ssp->spcu_cb_tail = &rhp->next;
	local_irq_restore(flags);
	spcu_gp_start_if_needed(ssp);
}
EXPORT_SYMBOL_GPL(call_spcu);

/*
 * synchronize_spcu - wait for prior SPCU read-side critical-section completion
 */
void synchronize_spcu(struct spcu_struct *ssp)
{
	struct pcu_synchronize rs;

	init_pcu_head_on_stack(&rs.head);
	init_completion(&rs.completion);
	call_spcu(ssp, &rs.head, wakeme_after_pcu);
	wait_for_completion(&rs.completion);
	destroy_pcu_head_on_stack(&rs.head);
}
EXPORT_SYMBOL_GPL(synchronize_spcu);

/*
 * get_state_synchronize_spcu - Provide an end-of-grace-period cookie
 */
unsigned long get_state_synchronize_spcu(struct spcu_struct *ssp)
{
	unsigned long ret;

	barrier();
	ret = (READ_ONCE(ssp->spcu_idx) + 3) & ~0x1;
	barrier();
	return ret & USHRT_MAX;
}
EXPORT_SYMBOL_GPL(get_state_synchronize_spcu);

/*
 * start_poll_synchronize_spcu - Provide cookie and start grace period
 *
 * The difference between this and get_state_synchronize_spcu() is that
 * this function ensures that the poll_state_synchronize_spcu() will
 * eventually return the value true.
 */
unsigned long start_poll_synchronize_spcu(struct spcu_struct *ssp)
{
	unsigned long ret = get_state_synchronize_spcu(ssp);

	spcu_gp_start_if_needed(ssp);
	return ret;
}
EXPORT_SYMBOL_GPL(start_poll_synchronize_spcu);

/*
 * poll_state_synchronize_spcu - Has cookie's grace period ended?
 */
bool poll_state_synchronize_spcu(struct spcu_struct *ssp, unsigned long cookie)
{
	bool ret = USHORT_CMP_GE(READ_ONCE(ssp->spcu_idx), cookie);

	barrier();
	return ret;
}
EXPORT_SYMBOL_GPL(poll_state_synchronize_spcu);

/* Lockdep diagnostics.  */
void pcu_scheduler_starting(void)
{
	pcu_scheduler_active = PCU_SCHEDULER_RUNNING;
}

/*
 * Queue work for spcu_struct structures with early boot callbacks.
 * The work won't actually execute until the workqueue initialization
 * phase that takes place after the scheduler starts.
 */
void spcu_init(void)
{
	struct spcu_struct *ssp;

	spcu_init_done = true;
	while (!list_empty(&spcu_boot_list)) {
		ssp = list_first_entry(&spcu_boot_list,
				      struct spcu_struct, spcu_work.entry);
		list_del_init(&ssp->spcu_work.entry);
		schedule_work(&ssp->spcu_work);
	}
}
