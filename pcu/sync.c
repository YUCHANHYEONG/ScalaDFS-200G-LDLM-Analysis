/*
 * PCU-based infrastructure for lightweight reader-writer locking
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
 * Copyright (c) 2015, Red Hat, Inc.
 *
 * Author: Oleg Nesterov <oleg@redhat.com>
 */

#include <linux/pcu_sync.h>
#include <linux/sched.h>

enum { GP_IDLE = 0, GP_ENTER, GP_PASSED, GP_EXIT, GP_REPLAY };

#define	rss_lock	gp_wait.lock

/**
 * pcu_sync_init() - Initialize an pcu_sync structure
 * @rsp: Pointer to pcu_sync structure to be initialized
 */
void pcu_sync_init(struct pcu_sync *rsp)
{
	memset(rsp, 0, sizeof(*rsp));
	init_waitqueue_head(&rsp->gp_wait);
}

/**
 * pcu_sync_enter_start - Force readers onto slow path for multiple updates
 * @rsp: Pointer to pcu_sync structure to use for synchronization
 *
 * Must be called after pcu_sync_init() and before first use.
 *
 * Ensures pcu_sync_is_idle() returns false and pcu_sync_{enter,exit}()
 * pairs turn into NO-OPs.
 */
void pcu_sync_enter_start(struct pcu_sync *rsp)
{
	rsp->gp_count++;
	rsp->gp_state = GP_PASSED;
}


static void pcu_sync_func(struct pcu_head *rhp);

static void pcu_sync_call(struct pcu_sync *rsp)
{
	call_pcu(&rsp->cb_head, pcu_sync_func);
}

/**
 * pcu_sync_func() - Callback function managing reader access to fastpath
 * @rhp: Pointer to pcu_head in pcu_sync structure to use for synchronization
 *
 * This function is passed to call_pcu() function by pcu_sync_enter() and
 * pcu_sync_exit(), so that it is invoked after a grace period following the
 * that invocation of enter/exit.
 *
 * If it is called by pcu_sync_enter() it signals that all the readers were
 * switched onto slow path.
 *
 * If it is called by pcu_sync_exit() it takes action based on events that
 * have taken place in the meantime, so that closely spaced pcu_sync_enter()
 * and pcu_sync_exit() pairs need not wait for a grace period.
 *
 * If another pcu_sync_enter() is invoked before the grace period
 * ended, reset state to allow the next pcu_sync_exit() to let the
 * readers back onto their fastpaths (after a grace period).  If both
 * another pcu_sync_enter() and its matching pcu_sync_exit() are invoked
 * before the grace period ended, re-invoke call_pcu() on behalf of that
 * pcu_sync_exit().  Otherwise, set all state back to idle so that readers
 * can again use their fastpaths.
 */
static void pcu_sync_func(struct pcu_head *rhp)
{
	struct pcu_sync *rsp = container_of(rhp, struct pcu_sync, cb_head);
	unsigned long flags;

	WARN_ON_ONCE(READ_ONCE(rsp->gp_state) == GP_IDLE);
	WARN_ON_ONCE(READ_ONCE(rsp->gp_state) == GP_PASSED);

	spin_lock_irqsave(&rsp->rss_lock, flags);
	if (rsp->gp_count) {
		/*
		 * We're at least a GP after the GP_IDLE->GP_ENTER transition.
		 */
		WRITE_ONCE(rsp->gp_state, GP_PASSED);
		wake_up_locked(&rsp->gp_wait);
	} else if (rsp->gp_state == GP_REPLAY) {
		/*
		 * A new pcu_sync_exit() has happened; requeue the callback to
		 * catch a later GP.
		 */
		WRITE_ONCE(rsp->gp_state, GP_EXIT);
		pcu_sync_call(rsp);
	} else {
		/*
		 * We're at least a GP after the last pcu_sync_exit(); eveybody
		 * will now have observed the write side critical section.
		 * Let 'em rip!.
		 */
		WRITE_ONCE(rsp->gp_state, GP_IDLE);
	}
	spin_unlock_irqrestore(&rsp->rss_lock, flags);
}

/**
 * pcu_sync_enter() - Force readers onto slowpath
 * @rsp: Pointer to pcu_sync structure to use for synchronization
 *
 * This function is used by updaters who need readers to make use of
 * a slowpath during the update.  After this function returns, all
 * subsequent calls to pcu_sync_is_idle() will return false, which
 * tells readers to stay off their fastpaths.  A later call to
 * pcu_sync_exit() re-enables reader slowpaths.
 *
 * When called in isolation, pcu_sync_enter() must wait for a grace
 * period, however, closely spaced calls to pcu_sync_enter() can
 * optimize away the grace-period wait via a state machine implemented
 * by pcu_sync_enter(), pcu_sync_exit(), and pcu_sync_func().
 */
void pcu_sync_enter(struct pcu_sync *rsp)
{
	int gp_state;

	spin_lock_irq(&rsp->rss_lock);
	gp_state = rsp->gp_state;
	if (gp_state == GP_IDLE) {
		WRITE_ONCE(rsp->gp_state, GP_ENTER);
		WARN_ON_ONCE(rsp->gp_count);
		/*
		 * Note that we could simply do pcu_sync_call(rsp) here and
		 * avoid the "if (gp_state == GP_IDLE)" block below.
		 *
		 * However, synchronize_pcu() can be faster if pcu_expedited
		 * or pcu_blocking_is_gp() is true.
		 *
		 * Another reason is that we can't wait for pcu callback if
		 * we are called at early boot time but this shouldn't happen.
		 */
	}
	rsp->gp_count++;
	spin_unlock_irq(&rsp->rss_lock);

	if (gp_state == GP_IDLE) {
		/*
		 * See the comment above, this simply does the "synchronous"
		 * call_pcu(pcu_sync_func) which does GP_ENTER -> GP_PASSED.
		 */
		synchronize_pcu();
		pcu_sync_func(&rsp->cb_head);
		/* Not really needed, wait_event() would see GP_PASSED. */
		return;
	}

	wait_event(rsp->gp_wait, READ_ONCE(rsp->gp_state) >= GP_PASSED);
}

/**
 * pcu_sync_exit() - Allow readers back onto fast path after grace period
 * @rsp: Pointer to pcu_sync structure to use for synchronization
 *
 * This function is used by updaters who have completed, and can therefore
 * now allow readers to make use of their fastpaths after a grace period
 * has elapsed.  After this grace period has completed, all subsequent
 * calls to pcu_sync_is_idle() will return true, which tells readers that
 * they can once again use their fastpaths.
 */
void pcu_sync_exit(struct pcu_sync *rsp)
{
	WARN_ON_ONCE(READ_ONCE(rsp->gp_state) == GP_IDLE);
	WARN_ON_ONCE(READ_ONCE(rsp->gp_count) == 0);

	spin_lock_irq(&rsp->rss_lock);
	if (!--rsp->gp_count) {
		if (rsp->gp_state == GP_PASSED) {
			WRITE_ONCE(rsp->gp_state, GP_EXIT);
			pcu_sync_call(rsp);
		} else if (rsp->gp_state == GP_EXIT) {
			WRITE_ONCE(rsp->gp_state, GP_REPLAY);
		}
	}
	spin_unlock_irq(&rsp->rss_lock);
}

/**
 * pcu_sync_dtor() - Clean up an pcu_sync structure
 * @rsp: Pointer to pcu_sync structure to be cleaned up
 */
void pcu_sync_dtor(struct pcu_sync *rsp)
{
	int gp_state;

	WARN_ON_ONCE(READ_ONCE(rsp->gp_count));
	WARN_ON_ONCE(READ_ONCE(rsp->gp_state) == GP_PASSED);

	spin_lock_irq(&rsp->rss_lock);
	if (rsp->gp_state == GP_REPLAY)
		WRITE_ONCE(rsp->gp_state, GP_EXIT);
	gp_state = rsp->gp_state;
	spin_unlock_irq(&rsp->rss_lock);

	if (gp_state != GP_IDLE) {
		pcu_barrier();
		WARN_ON_ONCE(rsp->gp_state != GP_IDLE);
	}
}
