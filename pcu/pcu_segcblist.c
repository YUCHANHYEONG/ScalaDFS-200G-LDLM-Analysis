/*
 * PCU segmented callback lists, function definitions
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

#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include "pcu_segcblist.h"

/* Initialize simple callback list. */
void pcu_cblist_init(struct pcu_cblist *rclp)
{
	rclp->head = NULL;
	rclp->tail = &rclp->head;
	rclp->len = 0;
}

/*
 * Enqueue an pcu_head structure onto the specified callback list.
 */
void pcu_cblist_enqueue(struct pcu_cblist *rclp, struct pcu_head *rhp)
{
	*rclp->tail = rhp;
	rclp->tail = &rhp->next;
	WRITE_ONCE(rclp->len, rclp->len + 1);
}

/*
 * Flush the second pcu_cblist structure onto the first one, obliterating
 * any contents of the first.  If rhp is non-NULL, enqueue it as the sole
 * element of the second pcu_cblist structure, but ensuring that the second
 * pcu_cblist structure, if initially non-empty, always appears non-empty
 * throughout the process.  If rdp is NULL, the second pcu_cblist structure
 * is instead initialized to empty.
 */
void pcu_cblist_flush_enqueue(struct pcu_cblist *drclp,
			      struct pcu_cblist *srclp,
			      struct pcu_head *rhp)
{
	drclp->head = srclp->head;
	if (drclp->head)
		drclp->tail = srclp->tail;
	else
		drclp->tail = &drclp->head;
	drclp->len = srclp->len;
	if (!rhp) {
		pcu_cblist_init(srclp);
	} else {
		rhp->next = NULL;
		srclp->head = rhp;
		srclp->tail = &rhp->next;
		WRITE_ONCE(srclp->len, 1);
	}
}

/*
 * Dequeue the oldest pcu_head structure from the specified callback
 * list.
 */
struct pcu_head *pcu_cblist_dequeue(struct pcu_cblist *rclp)
{
	struct pcu_head *rhp;

	rhp = rclp->head;
	if (!rhp)
		return NULL;
	rclp->len--;
	rclp->head = rhp->next;
	if (!rclp->head)
		rclp->tail = &rclp->head;
	return rhp;
}

/* Set the length of an pcu_segcblist structure. */
static void pcu_segcblist_set_len(struct pcu_segcblist *rsclp, long v)
{
#ifdef CONFIG_PCU_NOCB_CPU
	atomic_long_set(&rsclp->len, v);
#else
	WRITE_ONCE(rsclp->len, v);
#endif
}

/* Get the length of a segment of the pcu_segcblist structure. */
static long pcu_segcblist_get_seglen(struct pcu_segcblist *rsclp, int seg)
{
	return READ_ONCE(rsclp->seglen[seg]);
}

/* Return number of callbacks in segmented callback list by summing seglen. */
long pcu_segcblist_n_segment_cbs(struct pcu_segcblist *rsclp)
{
	long len = 0;
	int i;

	for (i = PCU_DONE_TAIL; i < PCU_CBLIST_NSEGS; i++)
		len += pcu_segcblist_get_seglen(rsclp, i);

	return len;
}

/* Set the length of a segment of the pcu_segcblist structure. */
static void pcu_segcblist_set_seglen(struct pcu_segcblist *rsclp, int seg, long v)
{
	WRITE_ONCE(rsclp->seglen[seg], v);
}

/* Increase the numeric length of a segment by a specified amount. */
static void pcu_segcblist_add_seglen(struct pcu_segcblist *rsclp, int seg, long v)
{
	WRITE_ONCE(rsclp->seglen[seg], rsclp->seglen[seg] + v);
}

/* Move from's segment length to to's segment. */
static void pcu_segcblist_move_seglen(struct pcu_segcblist *rsclp, int from, int to)
{
	long len;

	if (from == to)
		return;

	len = pcu_segcblist_get_seglen(rsclp, from);
	if (!len)
		return;

	pcu_segcblist_add_seglen(rsclp, to, len);
	pcu_segcblist_set_seglen(rsclp, from, 0);
}

/* Increment segment's length. */
static void pcu_segcblist_inc_seglen(struct pcu_segcblist *rsclp, int seg)
{
	pcu_segcblist_add_seglen(rsclp, seg, 1);
}

/*
 * Increase the numeric length of an pcu_segcblist structure by the
 * specified amount, which can be negative.  This can cause the ->len
 * field to disagree with the actual number of callbacks on the structure.
 * This increase is fully ordered with respect to the callers accesses
 * both before and after.
 *
 * So why on earth is a memory barrier required both before and after
 * the update to the ->len field???
 *
 * The reason is that pcu_barrier() locklessly samples each CPU's ->len
 * field, and if a given CPU's field is zero, avoids IPIing that CPU.
 * This can of course race with both queuing and invoking of callbacks.
 * Failing to correctly handle either of these races could result in
 * pcu_barrier() failing to IPI a CPU that actually had callbacks queued
 * which pcu_barrier() was obligated to wait on.  And if pcu_barrier()
 * failed to wait on such a callback, unloading certain kernel modules
 * would result in calls to functions whose code was no longer present in
 * the kernel, for but one example.
 *
 * Therefore, ->len transitions from 1->0 and 0->1 have to be carefully
 * ordered with respect with both list modifications and the pcu_barrier().
 *
 * The queuing case is CASE 1 and the invoking case is CASE 2.
 *
 * CASE 1: Suppose that CPU 0 has no callbacks queued, but invokes
 * call_pcu() just as CPU 1 invokes pcu_barrier().  CPU 0's ->len field
 * will transition from 0->1, which is one of the transitions that must
 * be handled carefully.  Without the full memory barriers after the ->len
 * update and at the beginning of pcu_barrier(), the following could happen:
 *
 * CPU 0				CPU 1
 *
 * call_pcu().
 *					pcu_barrier() sees ->len as 0.
 * set ->len = 1.
 *					pcu_barrier() does nothing.
 *					module is unloaded.
 * callback invokes unloaded function!
 *
 * With the full barriers, any case where pcu_barrier() sees ->len as 0 will
 * have unambiguously preceded the return from the racing call_pcu(), which
 * means that this call_pcu() invocation is OK to not wait on.  After all,
 * you are supposed to make sure that any problematic call_pcu() invocations
 * happen before the pcu_barrier().
 *
 *
 * CASE 2: Suppose that CPU 0 is invoking its last callback just as
 * CPU 1 invokes pcu_barrier().  CPU 0's ->len field will transition from
 * 1->0, which is one of the transitions that must be handled carefully.
 * Without the full memory barriers before the ->len update and at the
 * end of pcu_barrier(), the following could happen:
 *
 * CPU 0				CPU 1
 *
 * start invoking last callback
 * set ->len = 0 (reordered)
 *					pcu_barrier() sees ->len as 0
 *					pcu_barrier() does nothing.
 *					module is unloaded
 * callback executing after unloaded!
 *
 * With the full barriers, any case where pcu_barrier() sees ->len as 0
 * will be fully ordered after the completion of the callback function,
 * so that the module unloading operation is completely safe.
 *
 */
void pcu_segcblist_add_len(struct pcu_segcblist *rsclp, long v)
{
#ifdef CONFIG_PCU_NOCB_CPU
	smp_mb__before_atomic(); // Read header comment above.
	atomic_long_add(v, &rsclp->len);
	smp_mb__after_atomic();  // Read header comment above.
#else
	smp_mb(); // Read header comment above.
	WRITE_ONCE(rsclp->len, rsclp->len + v);
	smp_mb(); // Read header comment above.
#endif
}

/*
 * Increase the numeric length of an pcu_segcblist structure by one.
 * This can cause the ->len field to disagree with the actual number of
 * callbacks on the structure.  This increase is fully ordered with respect
 * to the callers accesses both before and after.
 */
void pcu_segcblist_inc_len(struct pcu_segcblist *rsclp)
{
	pcu_segcblist_add_len(rsclp, 1);
}

/*
 * Initialize an pcu_segcblist structure.
 */
void pcu_segcblist_init(struct pcu_segcblist *rsclp)
{
	int i;

	BUILD_BUG_ON(PCU_NEXT_TAIL + 1 != ARRAY_SIZE(rsclp->gp_seq));
	BUILD_BUG_ON(ARRAY_SIZE(rsclp->tails) != ARRAY_SIZE(rsclp->gp_seq));
	rsclp->head = NULL;
	for (i = 0; i < PCU_CBLIST_NSEGS; i++) {
		rsclp->tails[i] = &rsclp->head;
		pcu_segcblist_set_seglen(rsclp, i, 0);
	}
	pcu_segcblist_set_len(rsclp, 0);
	pcu_segcblist_set_flags(rsclp, SEGCBLIST_ENABLED);
}

/*
 * Disable the specified pcu_segcblist structure, so that callbacks can
 * no longer be posted to it.  This structure must be empty.
 */
void pcu_segcblist_disable(struct pcu_segcblist *rsclp)
{
	WARN_ON_ONCE(!pcu_segcblist_empty(rsclp));
	WARN_ON_ONCE(pcu_segcblist_n_cbs(rsclp));
	pcu_segcblist_clear_flags(rsclp, SEGCBLIST_ENABLED);
}

/*
 * Mark the specified pcu_segcblist structure as offloaded.
 */
void pcu_segcblist_offload(struct pcu_segcblist *rsclp, bool offload)
{
	if (offload) {
		pcu_segcblist_clear_flags(rsclp, SEGCBLIST_SOFTIRQ_ONLY);
		pcu_segcblist_set_flags(rsclp, SEGCBLIST_OFFLOADED);
	} else {
		pcu_segcblist_clear_flags(rsclp, SEGCBLIST_OFFLOADED);
	}
}

/*
 * Does the specified pcu_segcblist structure contain callbacks that
 * are ready to be invoked?
 */
bool pcu_segcblist_ready_cbs(struct pcu_segcblist *rsclp)
{
	return pcu_segcblist_is_enabled(rsclp) &&
	       &rsclp->head != READ_ONCE(rsclp->tails[PCU_DONE_TAIL]);
}

/*
 * Does the specified pcu_segcblist structure contain callbacks that
 * are still pending, that is, not yet ready to be invoked?
 */
bool pcu_segcblist_pend_cbs(struct pcu_segcblist *rsclp)
{
	return pcu_segcblist_is_enabled(rsclp) &&
	       !pcu_segcblist_restempty(rsclp, PCU_DONE_TAIL);
}

/*
 * Return a pointer to the first callback in the specified pcu_segcblist
 * structure.  This is useful for diagnostics.
 */
struct pcu_head *pcu_segcblist_first_cb(struct pcu_segcblist *rsclp)
{
	if (pcu_segcblist_is_enabled(rsclp))
		return rsclp->head;
	return NULL;
}

/*
 * Return a pointer to the first pending callback in the specified
 * pcu_segcblist structure.  This is useful just after posting a given
 * callback -- if that callback is the first pending callback, then
 * you cannot rely on someone else having already started up the required
 * grace period.
 */
struct pcu_head *pcu_segcblist_first_pend_cb(struct pcu_segcblist *rsclp)
{
	if (pcu_segcblist_is_enabled(rsclp))
		return *rsclp->tails[PCU_DONE_TAIL];
	return NULL;
}

/*
 * Return false if there are no CBs awaiting grace periods, otherwise,
 * return true and store the nearest waited-upon grace period into *lp.
 */
bool pcu_segcblist_nextgp(struct pcu_segcblist *rsclp, unsigned long *lp)
{
	if (!pcu_segcblist_pend_cbs(rsclp))
		return false;
	*lp = rsclp->gp_seq[PCU_WAIT_TAIL];
	return true;
}

/*
 * Enqueue the specified callback onto the specified pcu_segcblist
 * structure, updating accounting as needed.  Note that the ->len
 * field may be accessed locklessly, hence the WRITE_ONCE().
 * The ->len field is used by pcu_barrier() and friends to determine
 * if it must post a callback on this structure, and it is OK
 * for pcu_barrier() to sometimes post callbacks needlessly, but
 * absolutely not OK for it to ever miss posting a callback.
 */
void pcu_segcblist_enqueue(struct pcu_segcblist *rsclp,
			   struct pcu_head *rhp)
{
	pcu_segcblist_inc_len(rsclp);
	pcu_segcblist_inc_seglen(rsclp, PCU_NEXT_TAIL);
	rhp->next = NULL;
	WRITE_ONCE(*rsclp->tails[PCU_NEXT_TAIL], rhp);
	WRITE_ONCE(rsclp->tails[PCU_NEXT_TAIL], &rhp->next);
}

/*
 * Entrain the specified callback onto the specified pcu_segcblist at
 * the end of the last non-empty segment.  If the entire pcu_segcblist
 * is empty, make no change, but return false.
 *
 * This is intended for use by pcu_barrier()-like primitives, -not-
 * for normal grace-period use.  IMPORTANT:  The callback you enqueue
 * will wait for all prior callbacks, NOT necessarily for a grace
 * period.  You have been warned.
 */
bool pcu_segcblist_entrain(struct pcu_segcblist *rsclp,
			   struct pcu_head *rhp)
{
	int i;

	if (pcu_segcblist_n_cbs(rsclp) == 0)
		return false;
	pcu_segcblist_inc_len(rsclp);
	smp_mb(); /* Ensure counts are updated before callback is entrained. */
	rhp->next = NULL;
	for (i = PCU_NEXT_TAIL; i > PCU_DONE_TAIL; i--)
		if (rsclp->tails[i] != rsclp->tails[i - 1])
			break;
	pcu_segcblist_inc_seglen(rsclp, i);
	WRITE_ONCE(*rsclp->tails[i], rhp);
	for (; i <= PCU_NEXT_TAIL; i++)
		WRITE_ONCE(rsclp->tails[i], &rhp->next);
	return true;
}

/*
 * Extract only those callbacks ready to be invoked from the specified
 * pcu_segcblist structure and place them in the specified pcu_cblist
 * structure.
 */
void pcu_segcblist_extract_done_cbs(struct pcu_segcblist *rsclp,
				    struct pcu_cblist *rclp)
{
	int i;

	if (!pcu_segcblist_ready_cbs(rsclp))
		return; /* Nothing to do. */
	rclp->len = pcu_segcblist_get_seglen(rsclp, PCU_DONE_TAIL);
	*rclp->tail = rsclp->head;
	WRITE_ONCE(rsclp->head, *rsclp->tails[PCU_DONE_TAIL]);
	WRITE_ONCE(*rsclp->tails[PCU_DONE_TAIL], NULL);
	rclp->tail = rsclp->tails[PCU_DONE_TAIL];
	for (i = PCU_CBLIST_NSEGS - 1; i >= PCU_DONE_TAIL; i--)
		if (rsclp->tails[i] == rsclp->tails[PCU_DONE_TAIL])
			WRITE_ONCE(rsclp->tails[i], &rsclp->head);
	pcu_segcblist_set_seglen(rsclp, PCU_DONE_TAIL, 0);
}

/*
 * Extract only those callbacks still pending (not yet ready to be
 * invoked) from the specified pcu_segcblist structure and place them in
 * the specified pcu_cblist structure.  Note that this loses information
 * about any callbacks that might have been partway done waiting for
 * their grace period.  Too bad!  They will have to start over.
 */
void pcu_segcblist_extract_pend_cbs(struct pcu_segcblist *rsclp,
				    struct pcu_cblist *rclp)
{
	int i;

	if (!pcu_segcblist_pend_cbs(rsclp))
		return; /* Nothing to do. */
	rclp->len = 0;
	*rclp->tail = *rsclp->tails[PCU_DONE_TAIL];
	rclp->tail = rsclp->tails[PCU_NEXT_TAIL];
	WRITE_ONCE(*rsclp->tails[PCU_DONE_TAIL], NULL);
	for (i = PCU_DONE_TAIL + 1; i < PCU_CBLIST_NSEGS; i++) {
		rclp->len += pcu_segcblist_get_seglen(rsclp, i);
		WRITE_ONCE(rsclp->tails[i], rsclp->tails[PCU_DONE_TAIL]);
		pcu_segcblist_set_seglen(rsclp, i, 0);
	}
}

/*
 * Insert counts from the specified pcu_cblist structure in the
 * specified pcu_segcblist structure.
 */
void pcu_segcblist_insert_count(struct pcu_segcblist *rsclp,
				struct pcu_cblist *rclp)
{
	pcu_segcblist_add_len(rsclp, rclp->len);
}

/*
 * Move callbacks from the specified pcu_cblist to the beginning of the
 * done-callbacks segment of the specified pcu_segcblist.
 */
void pcu_segcblist_insert_done_cbs(struct pcu_segcblist *rsclp,
				   struct pcu_cblist *rclp)
{
	int i;

	if (!rclp->head)
		return; /* No callbacks to move. */
	pcu_segcblist_add_seglen(rsclp, PCU_DONE_TAIL, rclp->len);
	*rclp->tail = rsclp->head;
	WRITE_ONCE(rsclp->head, rclp->head);
	for (i = PCU_DONE_TAIL; i < PCU_CBLIST_NSEGS; i++)
		if (&rsclp->head == rsclp->tails[i])
			WRITE_ONCE(rsclp->tails[i], rclp->tail);
		else
			break;
	rclp->head = NULL;
	rclp->tail = &rclp->head;
}

/*
 * Move callbacks from the specified pcu_cblist to the end of the
 * new-callbacks segment of the specified pcu_segcblist.
 */
void pcu_segcblist_insert_pend_cbs(struct pcu_segcblist *rsclp,
				   struct pcu_cblist *rclp)
{
	if (!rclp->head)
		return; /* Nothing to do. */

	pcu_segcblist_add_seglen(rsclp, PCU_NEXT_TAIL, rclp->len);
	WRITE_ONCE(*rsclp->tails[PCU_NEXT_TAIL], rclp->head);
	WRITE_ONCE(rsclp->tails[PCU_NEXT_TAIL], rclp->tail);
}

/*
 * Advance the callbacks in the specified pcu_segcblist structure based
 * on the current value passed in for the grace-period counter.
 */
void pcu_segcblist_advance(struct pcu_segcblist *rsclp, unsigned long seq)
{
	int i, j;

	WARN_ON_ONCE(!pcu_segcblist_is_enabled(rsclp));
	if (pcu_segcblist_restempty(rsclp, PCU_DONE_TAIL))
		return;

	/*
	 * Find all callbacks whose ->gp_seq numbers indicate that they
	 * are ready to invoke, and put them into the PCU_DONE_TAIL segment.
	 */
	for (i = PCU_WAIT_TAIL; i < PCU_NEXT_TAIL; i++) {
		if (ULONG_CMP_LT(seq, rsclp->gp_seq[i]))
			break;
		WRITE_ONCE(rsclp->tails[PCU_DONE_TAIL], rsclp->tails[i]);
		pcu_segcblist_move_seglen(rsclp, i, PCU_DONE_TAIL);
	}

	/* If no callbacks moved, nothing more need be done. */
	if (i == PCU_WAIT_TAIL)
		return;

	/* Clean up tail pointers that might have been misordered above. */
	for (j = PCU_WAIT_TAIL; j < i; j++)
		WRITE_ONCE(rsclp->tails[j], rsclp->tails[PCU_DONE_TAIL]);

	/*
	 * Callbacks moved, so clean up the misordered ->tails[] pointers
	 * that now point into the middle of the list of ready-to-invoke
	 * callbacks.  The overall effect is to copy down the later pointers
	 * into the gap that was created by the now-ready segments.
	 */
	for (j = PCU_WAIT_TAIL; i < PCU_NEXT_TAIL; i++, j++) {
		if (rsclp->tails[j] == rsclp->tails[PCU_NEXT_TAIL])
			break;  /* No more callbacks. */
		WRITE_ONCE(rsclp->tails[j], rsclp->tails[i]);
		pcu_segcblist_move_seglen(rsclp, i, j);
		rsclp->gp_seq[j] = rsclp->gp_seq[i];
	}
}

/*
 * "Accelerate" callbacks based on more-accurate grace-period information.
 * The reason for this is that PCU does not synchronize the beginnings and
 * ends of grace periods, and that callbacks are posted locally.  This in
 * turn means that the callbacks must be labelled conservatively early
 * on, as getting exact information would degrade both performance and
 * scalability.  When more accurate grace-period information becomes
 * available, previously posted callbacks can be "accelerated", marking
 * them to complete at the end of the earlier grace period.
 *
 * This function operates on an pcu_segcblist structure, and also the
 * grace-period sequence number seq at which new callbacks would become
 * ready to invoke.  Returns true if there are callbacks that won't be
 * ready to invoke until seq, false otherwise.
 */
bool pcu_segcblist_accelerate(struct pcu_segcblist *rsclp, unsigned long seq)
{
	int i, j;

	WARN_ON_ONCE(!pcu_segcblist_is_enabled(rsclp));
	if (pcu_segcblist_restempty(rsclp, PCU_DONE_TAIL))
		return false;

	/*
	 * Find the segment preceding the oldest segment of callbacks
	 * whose ->gp_seq[] completion is at or after that passed in via
	 * "seq", skipping any empty segments.  This oldest segment, along
	 * with any later segments, can be merged in with any newly arrived
	 * callbacks in the PCU_NEXT_TAIL segment, and assigned "seq"
	 * as their ->gp_seq[] grace-period completion sequence number.
	 */
	for (i = PCU_NEXT_READY_TAIL; i > PCU_DONE_TAIL; i--)
		if (rsclp->tails[i] != rsclp->tails[i - 1] &&
		    ULONG_CMP_LT(rsclp->gp_seq[i], seq))
			break;

	/*
	 * If all the segments contain callbacks that correspond to
	 * earlier grace-period sequence numbers than "seq", leave.
	 * Assuming that the pcu_segcblist structure has enough
	 * segments in its arrays, this can only happen if some of
	 * the non-done segments contain callbacks that really are
	 * ready to invoke.  This situation will get straightened
	 * out by the next call to pcu_segcblist_advance().
	 *
	 * Also advance to the oldest segment of callbacks whose
	 * ->gp_seq[] completion is at or after that passed in via "seq",
	 * skipping any empty segments.
	 *
	 * Note that segment "i" (and any lower-numbered segments
	 * containing older callbacks) will be unaffected, and their
	 * grace-period numbers remain unchanged.  For example, if i ==
	 * WAIT_TAIL, then neither WAIT_TAIL nor DONE_TAIL will be touched.
	 * Instead, the CBs in NEXT_TAIL will be merged with those in
	 * NEXT_READY_TAIL and the grace-period number of NEXT_READY_TAIL
	 * would be updated.  NEXT_TAIL would then be empty.
	 */
	if (pcu_segcblist_restempty(rsclp, i) || ++i >= PCU_NEXT_TAIL)
		return false;

	/* Accounting: everything below i is about to get merged into i. */
	for (j = i + 1; j <= PCU_NEXT_TAIL; j++)
		pcu_segcblist_move_seglen(rsclp, j, i);

	/*
	 * Merge all later callbacks, including newly arrived callbacks,
	 * into the segment located by the for-loop above.  Assign "seq"
	 * as the ->gp_seq[] value in order to correctly handle the case
	 * where there were no pending callbacks in the pcu_segcblist
	 * structure other than in the PCU_NEXT_TAIL segment.
	 */
	for (; i < PCU_NEXT_TAIL; i++) {
		WRITE_ONCE(rsclp->tails[i], rsclp->tails[PCU_NEXT_TAIL]);
		rsclp->gp_seq[i] = seq;
	}
	return true;
}

/*
 * Merge the source pcu_segcblist structure into the destination
 * pcu_segcblist structure, then initialize the source.  Any pending
 * callbacks from the source get to start over.  It is best to
 * advance and accelerate both the destination and the source
 * before merging.
 */
void pcu_segcblist_merge(struct pcu_segcblist *dst_rsclp,
			 struct pcu_segcblist *src_rsclp)
{
	struct pcu_cblist donecbs;
	struct pcu_cblist pendcbs;

	lockdep_assert_cpus_held();

	pcu_cblist_init(&donecbs);
	pcu_cblist_init(&pendcbs);

	pcu_segcblist_extract_done_cbs(src_rsclp, &donecbs);
	pcu_segcblist_extract_pend_cbs(src_rsclp, &pendcbs);

	/*
	 * No need smp_mb() before setting length to 0, because CPU hotplug
	 * lock excludes pcu_barrier.
	 */
	pcu_segcblist_set_len(src_rsclp, 0);

	pcu_segcblist_insert_count(dst_rsclp, &donecbs);
	pcu_segcblist_insert_count(dst_rsclp, &pendcbs);
	pcu_segcblist_insert_done_cbs(dst_rsclp, &donecbs);
	pcu_segcblist_insert_pend_cbs(dst_rsclp, &pendcbs);

	pcu_segcblist_init(src_rsclp);
}
