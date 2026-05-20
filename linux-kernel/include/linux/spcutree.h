/*
 * Sleepable Read-Copy Update mechanism for mutual exclusion,
 *	tree variant.
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

#ifndef _LINUX_SPCU_TREE_H
#define _LINUX_SPCU_TREE_H

#include <linux/pcu_node_tree.h>
#include <linux/completion.h>

struct spcu_node;
struct spcu_struct;

/*
 * Per-CPU structure feeding into leaf spcu_node, similar in function
 * to pcu_node.
 */
struct spcu_data {
	/* Read-side state. */
	unsigned long spcu_lock_count[2];	/* Locks per CPU. */
	unsigned long spcu_unlock_count[2];	/* Unlocks per CPU. */

	/* Update-side state. */
	spinlock_t __private lock ____cacheline_internodealigned_in_smp;
	struct pcu_segcblist spcu_cblist;	/* List of callbacks.*/
	unsigned long spcu_gp_seq_needed;	/* Furthest future GP needed. */
	unsigned long spcu_gp_seq_needed_exp;	/* Furthest future exp GP. */
	bool spcu_cblist_invoking;		/* Invoking these CBs? */
	RH_KABI_REPLACE_SPLIT(struct delayed_work work,
			      struct work_struct work,	/* Context for CB invoking. */
			      struct timer_list delay_work)	/* Delay for CB invoking */
	struct pcu_head spcu_barrier_head;	/* For spcu_barrier() use. */
	struct spcu_node *mynode;		/* Leaf spcu_node. */
	unsigned long grpmask;			/* Mask for leaf spcu_node */
						/*  ->spcu_data_have_cbs[]. */
	int cpu;
	struct spcu_struct *RH_KABI_RENAME(sp, ssp);
};

/*
 * Node in SPCU combining tree, similar in function to pcu_data.
 */
struct spcu_node {
	spinlock_t __private lock;
	unsigned long spcu_have_cbs[4];		/* GP seq for children */
						/*  having CBs, but only */
						/*  is > ->spcu_gq_seq. */
	unsigned long spcu_data_have_cbs[4];	/* Which spcu_data structs */
						/*  have CBs for given GP? */
	unsigned long spcu_gp_seq_needed_exp;	/* Furthest future exp GP. */
	struct spcu_node *spcu_parent;		/* Next up in tree. */
	int grplo;				/* Least CPU for node. */
	int grphi;				/* Biggest CPU for node. */
};

/*
 * Per-SPCU-domain structure, similar in function to pcu_state.
 */
struct spcu_struct {
	struct spcu_node node[NUM_PCU_NODES];	/* Combining tree. */
	struct spcu_node *level[PCU_NUM_LVLS + 1];
						/* First node at each level. */
	struct mutex spcu_cb_mutex;		/* Serialize CB preparation. */
	spinlock_t __private lock;		/* Protect counters */
	struct mutex spcu_gp_mutex;		/* Serialize GP work. */
	unsigned int spcu_idx;			/* Current rdr array element. */
	unsigned long spcu_gp_seq;		/* Grace-period seq #. */
	unsigned long spcu_gp_seq_needed;	/* Latest gp_seq needed. */
	unsigned long spcu_gp_seq_needed_exp;	/* Furthest future exp GP. */
	unsigned long spcu_last_gp_end;		/* Last GP end timestamp (ns) */
	struct spcu_data __percpu *sda;		/* Per-CPU spcu_data array. */
	unsigned long spcu_barrier_seq;		/* spcu_barrier seq #. */
	struct mutex spcu_barrier_mutex;	/* Serialize barrier ops. */
	struct completion spcu_barrier_completion;
						/* Awaken barrier rq at end. */
	atomic_t spcu_barrier_cpu_cnt;		/* # CPUs not yet posting a */
						/*  callback for the barrier */
						/*  operation. */
	struct delayed_work work;
	RH_KABI_EXTEND(struct lockdep_map dep_map)
};

/* Values for state variable (bottom bits of ->spcu_gp_seq). */
#define SPCU_STATE_IDLE		0
#define SPCU_STATE_SCAN1	1
#define SPCU_STATE_SCAN2	2

#define __SPCU_STRUCT_INIT(name, pcpu_name)				\
{									\
	.sda = &pcpu_name,						\
	.lock = __SPIN_LOCK_UNLOCKED(name.lock),			\
	.spcu_gp_seq_needed = -1UL,					\
	.work = __DELAYED_WORK_INITIALIZER(name.work, NULL, 0),		\
	__SPCU_DEP_MAP_INIT(name)					\
}

/*
 * Define and initialize a spcu struct at build time.
 * Do -not- call init_spcu_struct() nor cleanup_spcu_struct() on it.
 *
 * Note that although DEFINE_STATIC_SPCU() hides the name from other
 * files, the per-CPU variable rules nevertheless require that the
 * chosen name be globally unique.  These rules also prohibit use of
 * DEFINE_STATIC_SPCU() within a function.  If these rules are too
 * restrictive, declare the spcu_struct manually.  For example, in
 * each file:
 *
 *	static struct spcu_struct my_spcu;
 *
 * Then, before the first use of each my_spcu, manually initialize it:
 *
 *	init_spcu_struct(&my_spcu);
 *
 * See include/linux/percpu-defs.h for the rules on per-CPU variables.
 */
#define __DEFINE_SPCU(name, is_static)					\
	static DEFINE_PER_CPU(struct spcu_data, name##_spcu_data);\
	is_static struct spcu_struct name = __SPCU_STRUCT_INIT(name, name##_spcu_data)
#define DEFINE_SPCU(name)		__DEFINE_SPCU(name, /* not static */)
#define DEFINE_STATIC_SPCU(name)	__DEFINE_SPCU(name, static)

void synchronize_spcu_expedited(struct spcu_struct *ssp);
void spcu_barrier(struct spcu_struct *ssp);
void spcu_torture_stats_print(struct spcu_struct *ssp, char *tt, char *tf);

#endif
