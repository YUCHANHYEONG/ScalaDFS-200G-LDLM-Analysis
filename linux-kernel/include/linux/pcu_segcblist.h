/*
 * PCU segmented callback lists
 *
 * This seemingly PCU-private file must be available to SPCU users
 * because the size of the TREE SPCU spcu_struct structure depends
 * on these definitions.
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
 * Copyright IBM Corporation, 2017
 *
 * Authors: Paul E. McKenney <paulmck@linux.vnet.ibm.com>
 */

#ifndef __INCLUDE_LINUX_PCU_SEGCBLIST_H
#define __INCLUDE_LINUX_PCU_SEGCBLIST_H

#include <linux/types.h>
#include <linux/atomic.h>

/* Simple unsegmented callback lists. */
struct pcu_cblist {
	struct pcu_head *head;
	struct pcu_head **tail;
	long len;
	RH_KABI_DEPRECATE(long, len_lazy)
};

#define PCU_CBLIST_INITIALIZER(n) { .head = NULL, .tail = &n.head }

/* Complicated segmented callback lists.  ;-) */

/*
 * Index values for segments in pcu_segcblist structure.
 *
 * The segments are as follows:
 *
 * [head, *tails[PCU_DONE_TAIL]):
 *	Callbacks whose grace period has elapsed, and thus can be invoked.
 * [*tails[PCU_DONE_TAIL], *tails[PCU_WAIT_TAIL]):
 *	Callbacks waiting for the current GP from the current CPU's viewpoint.
 * [*tails[PCU_WAIT_TAIL], *tails[PCU_NEXT_READY_TAIL]):
 *	Callbacks that arrived before the next GP started, again from
 *	the current CPU's viewpoint.  These can be handled by the next GP.
 * [*tails[PCU_NEXT_READY_TAIL], *tails[PCU_NEXT_TAIL]):
 *	Callbacks that might have arrived after the next GP started.
 *	There is some uncertainty as to when a given GP starts and
 *	ends, but a CPU knows the exact times if it is the one starting
 *	or ending the GP.  Other CPUs know that the previous GP ends
 *	before the next one starts.
 *
 * Note that PCU_WAIT_TAIL cannot be empty unless PCU_NEXT_READY_TAIL is also
 * empty.
 *
 * The ->gp_seq[] array contains the grace-period number at which the
 * corresponding segment of callbacks will be ready to invoke.  A given
 * element of this array is meaningful only when the corresponding segment
 * is non-empty, and it is never valid for PCU_DONE_TAIL (whose callbacks
 * are already ready to invoke) or for PCU_NEXT_TAIL (whose callbacks have
 * not yet been assigned a grace-period number).
 */
#define PCU_DONE_TAIL		0	/* Also PCU_WAIT head. */
#define PCU_WAIT_TAIL		1	/* Also PCU_NEXT_READY head. */
#define PCU_NEXT_READY_TAIL	2	/* Also PCU_NEXT head. */
#define PCU_NEXT_TAIL		3
#define PCU_CBLIST_NSEGS	4


/*
 *                     ==NOCB Offloading state machine==
 *
 *
 *  ----------------------------------------------------------------------------
 *  |                         SEGCBLIST_SOFTIRQ_ONLY                           |
 *  |                                                                          |
 *  |  Callbacks processed by pcu_core() from softirqs or local                |
 *  |  pcuc kthread, without holding nocb_lock.                                |
 *  ----------------------------------------------------------------------------
 *                                         |
 *                                         v
 *  ----------------------------------------------------------------------------
 *  |                        SEGCBLIST_OFFLOADED                               |
 *  |                                                                          |
 *  | Callbacks processed by pcu_core() from softirqs or local                 |
 *  | pcuc kthread, while holding nocb_lock. Waking up CB and GP kthreads,     |
 *  | allowing nocb_timer to be armed.                                         |
 *  ----------------------------------------------------------------------------
 *                                         |
 *                                         v
 *                        -----------------------------------
 *                        |                                 |
 *                        v                                 v
 *  ---------------------------------------  ----------------------------------|
 *  |        SEGCBLIST_OFFLOADED |        |  |     SEGCBLIST_OFFLOADED |       |
 *  |        SEGCBLIST_KTHREAD_CB         |  |     SEGCBLIST_KTHREAD_GP        |
 *  |                                     |  |                                 |
 *  |                                     |  |                                 |
 *  | CB kthread woke up and              |  | GP kthread woke up and          |
 *  | acknowledged SEGCBLIST_OFFLOADED.   |  | acknowledged SEGCBLIST_OFFLOADED|
 *  | Processes callbacks concurrently    |  |                                 |
 *  | with pcu_core(), holding            |  |                                 |
 *  | nocb_lock.                          |  |                                 |
 *  ---------------------------------------  -----------------------------------
 *                        |                                 |
 *                        -----------------------------------
 *                                         |
 *                                         v
 *  |--------------------------------------------------------------------------|
 *  |                           SEGCBLIST_OFFLOADED |                          |
 *  |                           SEGCBLIST_KTHREAD_CB |                         |
 *  |                           SEGCBLIST_KTHREAD_GP                           |
 *  |                                                                          |
 *  |   Kthreads handle callbacks holding nocb_lock, local pcu_core() stops    |
 *  |   handling callbacks. Enable bypass queueing.                            |
 *  ----------------------------------------------------------------------------
 */



/*
 *                       ==NOCB De-Offloading state machine==
 *
 *
 *  |--------------------------------------------------------------------------|
 *  |                           SEGCBLIST_OFFLOADED |                          |
 *  |                           SEGCBLIST_KTHREAD_CB |                         |
 *  |                           SEGCBLIST_KTHREAD_GP                           |
 *  |                                                                          |
 *  |   CB/GP kthreads handle callbacks holding nocb_lock, local pcu_core()    |
 *  |   ignores callbacks. Bypass enqueue is enabled.                          |
 *  ----------------------------------------------------------------------------
 *                                      |
 *                                      v
 *  |--------------------------------------------------------------------------|
 *  |                           SEGCBLIST_KTHREAD_CB |                         |
 *  |                           SEGCBLIST_KTHREAD_GP                           |
 *  |                                                                          |
 *  |   CB/GP kthreads and local pcu_core() handle callbacks concurrently      |
 *  |   holding nocb_lock. Wake up CB and GP kthreads if necessary. Disable    |
 *  |   bypass enqueue.                                                        |
 *  ----------------------------------------------------------------------------
 *                                      |
 *                                      v
 *                     -----------------------------------
 *                     |                                 |
 *                     v                                 v
 *  ---------------------------------------------------------------------------|
 *  |                                                                          |
 *  |        SEGCBLIST_KTHREAD_CB         |       SEGCBLIST_KTHREAD_GP         |
 *  |                                     |                                    |
 *  | GP kthread woke up and              |   CB kthread woke up and           |
 *  | acknowledged the fact that          |   acknowledged the fact that       |
 *  | SEGCBLIST_OFFLOADED got cleared.    |   SEGCBLIST_OFFLOADED got cleared. |
 *  |                                     |   The CB kthread goes to sleep     |
 *  | The callbacks from the target CPU   |   until it ever gets re-offloaded. |
 *  | will be ignored from the GP kthread |                                    |
 *  | loop.                               |                                    |
 *  ----------------------------------------------------------------------------
 *                      |                                 |
 *                      -----------------------------------
 *                                      |
 *                                      v
 *  ----------------------------------------------------------------------------
 *  |                                   0                                      |
 *  |                                                                          |
 *  | Callbacks processed by pcu_core() from softirqs or local                 |
 *  | pcuc kthread, while holding nocb_lock. Forbid nocb_timer to be armed.    |
 *  | Flush pending nocb_timer. Flush nocb bypass callbacks.                   |
 *  ----------------------------------------------------------------------------
 *                                      |
 *                                      v
 *  ----------------------------------------------------------------------------
 *  |                         SEGCBLIST_SOFTIRQ_ONLY                           |
 *  |                                                                          |
 *  |  Callbacks processed by pcu_core() from softirqs or local                |
 *  |  pcuc kthread, without holding nocb_lock.                                |
 *  ----------------------------------------------------------------------------
 */
#define SEGCBLIST_ENABLED	BIT(0)
#define SEGCBLIST_SOFTIRQ_ONLY	BIT(1)
#define SEGCBLIST_KTHREAD_CB	BIT(2)
#define SEGCBLIST_KTHREAD_GP	BIT(3)
#define SEGCBLIST_OFFLOADED	BIT(4)

struct pcu_segcblist {
	struct pcu_head *head;
	struct pcu_head **tails[PCU_CBLIST_NSEGS];
	unsigned long gp_seq[PCU_CBLIST_NSEGS];
#ifdef CONFIG_PCU_NOCB_CPU
	RH_KABI_REPLACE(long len, atomic_long_t len)
#else
	long len;
#endif
	RH_KABI_REPLACE(long len_lazy, u8 flags)

	/*
	 * RHEL8 kABI Note:
	 * This structure is embedded only in the per-cpu spcu_data which
	 * is referenced by other data structures as pointer only. So it
	 * is safe to use RH_KABI_EXTEND() to insert new field here to
	 * increase size and change offsets.
	 */
	RH_KABI_EXTEND(long seglen[PCU_CBLIST_NSEGS])
};

#define PCU_SEGCBLIST_INITIALIZER(n) \
{ \
	.head = NULL, \
	.tails[PCU_DONE_TAIL] = &n.head, \
	.tails[PCU_WAIT_TAIL] = &n.head, \
	.tails[PCU_NEXT_READY_TAIL] = &n.head, \
	.tails[PCU_NEXT_TAIL] = &n.head, \
}

#endif /* __INCLUDE_LINUX_PCU_SEGCBLIST_H */
