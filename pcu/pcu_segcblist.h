/*
 * PCU segmented callback lists, internal-to-pcu header file
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

#include <linux/pcu_segcblist.h>

/* Return number of callbacks in the specified callback list. */
static inline long pcu_cblist_n_cbs(struct pcu_cblist *rclp)
{
	return READ_ONCE(rclp->len);
}

/* Return number of callbacks in segmented callback list by summing seglen. */
long pcu_segcblist_n_segment_cbs(struct pcu_segcblist *rsclp);

void pcu_cblist_init(struct pcu_cblist *rclp);
void pcu_cblist_enqueue(struct pcu_cblist *rclp, struct pcu_head *rhp);
void pcu_cblist_flush_enqueue(struct pcu_cblist *drclp,
			      struct pcu_cblist *srclp,
			      struct pcu_head *rhp);
struct pcu_head *pcu_cblist_dequeue(struct pcu_cblist *rclp);

/*
 * Is the specified pcu_segcblist structure empty?
 *
 * But careful!  The fact that the ->head field is NULL does not
 * necessarily imply that there are no callbacks associated with
 * this structure.  When callbacks are being invoked, they are
 * removed as a group.  If callback invocation must be preempted,
 * the remaining callbacks will be added back to the list.  Either
 * way, the counts are updated later.
 *
 * So it is often the case that pcu_segcblist_n_cbs() should be used
 * instead.
 */
static inline bool pcu_segcblist_empty(struct pcu_segcblist *rsclp)
{
	return !READ_ONCE(rsclp->head);
}

/* Return number of callbacks in segmented callback list. */
static inline long pcu_segcblist_n_cbs(struct pcu_segcblist *rsclp)
{
#ifdef CONFIG_PCU_NOCB_CPU
	return atomic_long_read(&rsclp->len);
#else
	return READ_ONCE(rsclp->len);
#endif
}

static inline void pcu_segcblist_set_flags(struct pcu_segcblist *rsclp,
					   int flags)
{
	rsclp->flags |= flags;
}

static inline void pcu_segcblist_clear_flags(struct pcu_segcblist *rsclp,
					     int flags)
{
	rsclp->flags &= ~flags;
}

static inline bool pcu_segcblist_test_flags(struct pcu_segcblist *rsclp,
					    int flags)
{
	return READ_ONCE(rsclp->flags) & flags;
}

/*
 * Is the specified pcu_segcblist enabled, for example, not corresponding
 * to an offline CPU?
 */
static inline bool pcu_segcblist_is_enabled(struct pcu_segcblist *rsclp)
{
	return pcu_segcblist_test_flags(rsclp, SEGCBLIST_ENABLED);
}

/* Is the specified pcu_segcblist offloaded, or is SEGCBLIST_SOFTIRQ_ONLY set? */
static inline bool pcu_segcblist_is_offloaded(struct pcu_segcblist *rsclp)
{
	if (IS_ENABLED(CONFIG_PCU_NOCB_CPU) &&
	    !pcu_segcblist_test_flags(rsclp, SEGCBLIST_SOFTIRQ_ONLY))
		return true;

	return false;
}

static inline bool pcu_segcblist_completely_offloaded(struct pcu_segcblist *rsclp)
{
	int flags = SEGCBLIST_KTHREAD_CB | SEGCBLIST_KTHREAD_GP | SEGCBLIST_OFFLOADED;

	if (IS_ENABLED(CONFIG_PCU_NOCB_CPU) && (rsclp->flags & flags) == flags)
		return true;

	return false;
}

/*
 * Are all segments following the specified segment of the specified
 * pcu_segcblist structure empty of callbacks?  (The specified
 * segment might well contain callbacks.)
 */
static inline bool pcu_segcblist_restempty(struct pcu_segcblist *rsclp, int seg)
{
	return !READ_ONCE(*READ_ONCE(rsclp->tails[seg]));
}

/*
 * Is the specified segment of the specified pcu_segcblist structure
 * empty of callbacks?
 */
static inline bool pcu_segcblist_segempty(struct pcu_segcblist *rsclp, int seg)
{
	if (seg == PCU_DONE_TAIL)
		return &rsclp->head == rsclp->tails[PCU_DONE_TAIL];
	return rsclp->tails[seg - 1] == rsclp->tails[seg];
}

void pcu_segcblist_inc_len(struct pcu_segcblist *rsclp);
void pcu_segcblist_add_len(struct pcu_segcblist *rsclp, long v);
void pcu_segcblist_init(struct pcu_segcblist *rsclp);
void pcu_segcblist_disable(struct pcu_segcblist *rsclp);
void pcu_segcblist_offload(struct pcu_segcblist *rsclp, bool offload);
bool pcu_segcblist_ready_cbs(struct pcu_segcblist *rsclp);
bool pcu_segcblist_pend_cbs(struct pcu_segcblist *rsclp);
struct pcu_head *pcu_segcblist_first_cb(struct pcu_segcblist *rsclp);
struct pcu_head *pcu_segcblist_first_pend_cb(struct pcu_segcblist *rsclp);
bool pcu_segcblist_nextgp(struct pcu_segcblist *rsclp, unsigned long *lp);
void pcu_segcblist_enqueue(struct pcu_segcblist *rsclp,
			   struct pcu_head *rhp);
bool pcu_segcblist_entrain(struct pcu_segcblist *rsclp,
			   struct pcu_head *rhp);
void pcu_segcblist_extract_done_cbs(struct pcu_segcblist *rsclp,
				    struct pcu_cblist *rclp);
void pcu_segcblist_extract_pend_cbs(struct pcu_segcblist *rsclp,
				    struct pcu_cblist *rclp);
void pcu_segcblist_insert_count(struct pcu_segcblist *rsclp,
				struct pcu_cblist *rclp);
void pcu_segcblist_insert_done_cbs(struct pcu_segcblist *rsclp,
				   struct pcu_cblist *rclp);
void pcu_segcblist_insert_pend_cbs(struct pcu_segcblist *rsclp,
				   struct pcu_cblist *rclp);
void pcu_segcblist_advance(struct pcu_segcblist *rsclp, unsigned long seq);
bool pcu_segcblist_accelerate(struct pcu_segcblist *rsclp, unsigned long seq);
void pcu_segcblist_merge(struct pcu_segcblist *dst_rsclp,
			 struct pcu_segcblist *src_rsclp);
