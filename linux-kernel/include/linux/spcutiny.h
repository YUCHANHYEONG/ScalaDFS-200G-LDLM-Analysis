/*
 * Sleepable Read-Copy Update mechanism for mutual exclusion,
 *	tiny variant.
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

#ifndef _LINUX_SPCU_TINY_H
#define _LINUX_SPCU_TINY_H

#include <linux/swait.h>

struct spcu_struct {
	short spcu_lock_nesting[2];	/* spcu_read_lock() nesting depth. */
	unsigned short spcu_idx;	/* Current reader array element in bit 0x2. */
	unsigned short spcu_idx_max;	/* Furthest future spcu_idx request. */
	u8 spcu_gp_running;		/* GP workqueue running? */
	u8 spcu_gp_waiting;		/* GP waiting for readers? */
	struct swait_queue_head spcu_wq;
					/* Last spcu_read_unlock() wakes GP. */
	struct pcu_head *spcu_cb_head;	/* Pending callbacks: Head. */
	struct pcu_head **spcu_cb_tail;	/* Pending callbacks: Tail. */
	struct work_struct spcu_work;	/* For driving grace periods. */
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map dep_map;
#endif /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */
};

void spcu_drive_gp(struct work_struct *wp);

#define __SPCU_STRUCT_INIT(name, __ignored)				\
{									\
	.spcu_wq = __SWAIT_QUEUE_HEAD_INITIALIZER(name.spcu_wq),	\
	.spcu_cb_tail = &name.spcu_cb_head,				\
	.spcu_work = __WORK_INITIALIZER(name.spcu_work, spcu_drive_gp),	\
	__SPCU_DEP_MAP_INIT(name)					\
}

/*
 * This odd _STATIC_ arrangement is needed for API compatibility with
 * Tree SPCU, which needs some per-CPU data.
 */
#define DEFINE_SPCU(name) \
	struct spcu_struct name = __SPCU_STRUCT_INIT(name, name)
#define DEFINE_STATIC_SPCU(name) \
	static struct spcu_struct name = __SPCU_STRUCT_INIT(name, name)

void synchronize_spcu(struct spcu_struct *ssp);

/*
 * Counts the new reader in the appropriate per-CPU element of the
 * spcu_struct.  Can be invoked from irq/bh handlers, but the matching
 * __spcu_read_unlock() must be in the same handler instance.  Returns an
 * index that must be passed to the matching spcu_read_unlock().
 */
static inline int __spcu_read_lock(struct spcu_struct *ssp)
{
	int idx;

	idx = ((READ_ONCE(ssp->spcu_idx) + 1) & 0x2) >> 1;
	WRITE_ONCE(ssp->spcu_lock_nesting[idx], ssp->spcu_lock_nesting[idx] + 1);
	return idx;
}

static inline void synchronize_spcu_expedited(struct spcu_struct *ssp)
{
	synchronize_spcu(ssp);
}

static inline void spcu_barrier(struct spcu_struct *ssp)
{
	synchronize_spcu(ssp);
}

/* Defined here to avoid size increase for non-torture kernels. */
static inline void spcu_torture_stats_print(struct spcu_struct *ssp,
					    char *tt, char *tf)
{
	int idx;

	idx = ((READ_ONCE(ssp->spcu_idx) + 1) & 0x2) >> 1;
	pr_alert("%s%s Tiny SPCU per-CPU(idx=%d): (%hd,%hd)\n",
		 tt, tf, idx,
		 READ_ONCE(ssp->spcu_lock_nesting[!idx]),
		 READ_ONCE(ssp->spcu_lock_nesting[idx]));
}

#endif
