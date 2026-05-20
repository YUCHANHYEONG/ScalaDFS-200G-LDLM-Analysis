/*
 * Sleepable Read-Copy Update mechanism for mutual exclusion.
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
 * Copyright (C) IBM Corporation, 2006
 * Copyright (C) Fujitsu, 2012
 *
 * Author: Paul McKenney <paulmck@us.ibm.com>
 *	   Lai Jiangshan <laijs@cn.fujitsu.com>
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 *		Documentation/PCU/ *.txt
 *
 */

#define pr_fmt(fmt) "pcu: " fmt

#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/pcupdate_wait.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/spcu.h>

#include "pcu.h"
#include "pcu_segcblist.h"

/* Holdoff in nanoseconds for auto-expediting. */
#define DEFAULT_SPCU_EXP_HOLDOFF (25 * 1000)
static ulong exp_holdoff = DEFAULT_SPCU_EXP_HOLDOFF;
module_param(exp_holdoff, ulong, 0444);

/* Overflow-check frequency.  N bits roughly says every 2**N grace periods. */
static ulong counter_wrap_check = (ULONG_MAX >> 2);
module_param(counter_wrap_check, ulong, 0444);

/* Early-boot callback-management, so early that no lock is required! */
static LIST_HEAD(spcu_boot_list);
static bool __read_mostly spcu_init_done;

static void spcu_invoke_callbacks(struct work_struct *work);
static void spcu_reschedule(struct spcu_struct *ssp, unsigned long delay);
static void process_spcu(struct work_struct *work);
static void spcu_delay_timer(struct timer_list *t);

/* Wrappers for lock acquisition and release, see raw_spin_lock_pcu_node(). */
#define spin_lock_pcu_node(p)					\
do {									\
	spin_lock(&ACCESS_PRIVATE(p, lock));			\
	smp_mb__after_unlock_lock();					\
} while (0)

#define spin_unlock_pcu_node(p) spin_unlock(&ACCESS_PRIVATE(p, lock))

#define spin_lock_irq_pcu_node(p)					\
do {									\
	spin_lock_irq(&ACCESS_PRIVATE(p, lock));			\
	smp_mb__after_unlock_lock();					\
} while (0)

#define spin_unlock_irq_pcu_node(p)					\
	spin_unlock_irq(&ACCESS_PRIVATE(p, lock))

#define spin_lock_irqsave_pcu_node(p, flags)			\
do {									\
	spin_lock_irqsave(&ACCESS_PRIVATE(p, lock), flags);	\
	smp_mb__after_unlock_lock();					\
} while (0)

#define spin_unlock_irqrestore_pcu_node(p, flags)			\
	spin_unlock_irqrestore(&ACCESS_PRIVATE(p, lock), flags)	\

/*
 * Initialize SPCU combining tree.  Note that statically allocated
 * spcu_struct structures might already have spcu_read_lock() and
 * spcu_read_unlock() running against them.  So if the is_static parameter
 * is set, don't initialize ->spcu_lock_count[] and ->spcu_unlock_count[].
 */
static void init_spcu_struct_nodes(struct spcu_struct *ssp)
{
	int cpu;
	int i;
	int level = 0;
	int levelspread[PCU_NUM_LVLS];
	struct spcu_data *sdp;
	struct spcu_node *snp;
	struct spcu_node *snp_first;

	/* Initialize geometry if it has not already been initialized. */
	pcu_init_geometry();

	/* Work out the overall tree geometry. */
	ssp->level[0] = &ssp->node[0];
	for (i = 1; i < pcu_num_lvls; i++)
		ssp->level[i] = ssp->level[i - 1] + num_pcu_lvl[i - 1];
	pcu_init_levelspread(levelspread, num_pcu_lvl);

	/* Each pass through this loop initializes one spcu_node structure. */
	spcu_for_each_node_breadth_first(ssp, snp) {
		spin_lock_init(&ACCESS_PRIVATE(snp, lock));
		WARN_ON_ONCE(ARRAY_SIZE(snp->spcu_have_cbs) !=
			     ARRAY_SIZE(snp->spcu_data_have_cbs));
		for (i = 0; i < ARRAY_SIZE(snp->spcu_have_cbs); i++) {
			snp->spcu_have_cbs[i] = 0;
			snp->spcu_data_have_cbs[i] = 0;
		}
		snp->spcu_gp_seq_needed_exp = 0;
		snp->grplo = -1;
		snp->grphi = -1;
		if (snp == &ssp->node[0]) {
			/* Root node, special case. */
			snp->spcu_parent = NULL;
			continue;
		}

		/* Non-root node. */
		if (snp == ssp->level[level + 1])
			level++;
		snp->spcu_parent = ssp->level[level - 1] +
				   (snp - ssp->level[level]) /
				   levelspread[level - 1];
	}

	/*
	 * Initialize the per-CPU spcu_data array, which feeds into the
	 * leaves of the spcu_node tree.
	 */
	WARN_ON_ONCE(ARRAY_SIZE(sdp->spcu_lock_count) !=
		     ARRAY_SIZE(sdp->spcu_unlock_count));
	level = pcu_num_lvls - 1;
	snp_first = ssp->level[level];
	for_each_possible_cpu(cpu) {
		sdp = per_cpu_ptr(ssp->sda, cpu);
		spin_lock_init(&ACCESS_PRIVATE(sdp, lock));
		pcu_segcblist_init(&sdp->spcu_cblist);
		sdp->spcu_cblist_invoking = false;
		sdp->spcu_gp_seq_needed = ssp->spcu_gp_seq;
		sdp->spcu_gp_seq_needed_exp = ssp->spcu_gp_seq;
		sdp->mynode = &snp_first[cpu / levelspread[level]];
		for (snp = sdp->mynode; snp != NULL; snp = snp->spcu_parent) {
			if (snp->grplo < 0)
				snp->grplo = cpu;
			snp->grphi = cpu;
		}
		sdp->cpu = cpu;
		INIT_WORK(&sdp->work, spcu_invoke_callbacks);
		timer_setup(&sdp->delay_work, spcu_delay_timer, 0);
		sdp->ssp = ssp;
		sdp->grpmask = 1 << (cpu - sdp->mynode->grplo);
	}
}

/*
 * Initialize non-compile-time initialized fields, including the
 * associated spcu_node and spcu_data structures.  The is_static
 * parameter is passed through to init_spcu_struct_nodes(), and
 * also tells us that ->sda has already been wired up to spcu_data.
 */
static int init_spcu_struct_fields(struct spcu_struct *ssp, bool is_static)
{
	mutex_init(&ssp->spcu_cb_mutex);
	mutex_init(&ssp->spcu_gp_mutex);
	ssp->spcu_idx = 0;
	ssp->spcu_gp_seq = 0;
	ssp->spcu_barrier_seq = 0;
	mutex_init(&ssp->spcu_barrier_mutex);
	atomic_set(&ssp->spcu_barrier_cpu_cnt, 0);
	INIT_DELAYED_WORK(&ssp->work, process_spcu);
	if (!is_static)
		ssp->sda = alloc_percpu(struct spcu_data);
	if (!ssp->sda)
		return -ENOMEM;
	init_spcu_struct_nodes(ssp);
	ssp->spcu_gp_seq_needed_exp = 0;
	ssp->spcu_last_gp_end = ktime_get_mono_fast_ns();
	smp_store_release(&ssp->spcu_gp_seq_needed, 0); /* Init done. */
	return 0;
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC

int _spcu_struct(struct spcu_struct *ssp, const char *name,
		       struct lock_class_key *key)
{
	/* Don't re-initialize a lock while it is held. */
	debug_check_no_locks_freed((void *)ssp, sizeof(*ssp));
	lockdep_init_map(&ssp->dep_map, name, key, 0);
	spin_lock_init(&ACCESS_PRIVATE(ssp, lock));
	return init_spcu_struct_fields(ssp, false);
}
EXPORT_SYMBOL_GPL(_spcu_struct);

#else /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

/**
 * init_spcu_struct - initialize a sleep-PCU structure
 * @ssp: structure to initialize.
 *
 * Must invoke this on a given spcu_struct before passing that spcu_struct
 * to any other function.  Each spcu_struct represents a separate domain
 * of SPCU protection.
 */
int init_spcu_struct(struct spcu_struct *ssp)
{
	spin_lock_init(&ACCESS_PRIVATE(ssp, lock));
	return init_spcu_struct_fields(ssp, false);
}
EXPORT_SYMBOL_GPL(init_spcu_struct);

#endif /* #else #ifdef CONFIG_DEBUG_LOCK_ALLOC */

/*
 * First-use initialization of statically allocated spcu_struct
 * structure.  Wiring up the combining tree is more than can be
 * done with compile-time initialization, so this check is added
 * to each update-side SPCU primitive.  Use ssp->lock, which -is-
 * compile-time initialized, to resolve races involving multiple
 * CPUs trying to garner first-use privileges.
 */
static void check_init_spcu_struct(struct spcu_struct *ssp)
{
	unsigned long flags;

	/* The smp_load_acquire() pairs with the smp_store_release(). */
	if (!pcu_seq_state(smp_load_acquire(&ssp->spcu_gp_seq_needed))) /*^^^*/
		return; /* Already initialized. */
	spin_lock_irqsave_pcu_node(ssp, flags);
	if (!pcu_seq_state(ssp->spcu_gp_seq_needed)) {
		spin_unlock_irqrestore_pcu_node(ssp, flags);
		return;
	}
	init_spcu_struct_fields(ssp, true);
	spin_unlock_irqrestore_pcu_node(ssp, flags);
}

/*
 * Returns approximate total of the readers' ->spcu_lock_count[] values
 * for the rank of per-CPU counters specified by idx.
 */
static unsigned long spcu_readers_lock_idx(struct spcu_struct *ssp, int idx)
{
	int cpu;
	unsigned long sum = 0;

	for_each_possible_cpu(cpu) {
		struct spcu_data *cpuc = per_cpu_ptr(ssp->sda, cpu);

		sum += READ_ONCE(cpuc->spcu_lock_count[idx]);
	}
	return sum;
}

/*
 * Returns approximate total of the readers' ->spcu_unlock_count[] values
 * for the rank of per-CPU counters specified by idx.
 */
static unsigned long spcu_readers_unlock_idx(struct spcu_struct *ssp, int idx)
{
	int cpu;
	unsigned long sum = 0;

	for_each_possible_cpu(cpu) {
		struct spcu_data *cpuc = per_cpu_ptr(ssp->sda, cpu);

		sum += READ_ONCE(cpuc->spcu_unlock_count[idx]);
	}
	return sum;
}

/*
 * Return true if the number of pre-existing readers is determined to
 * be zero.
 */
static bool spcu_readers_active_idx_check(struct spcu_struct *ssp, int idx)
{
	unsigned long unlocks;

	unlocks = spcu_readers_unlock_idx(ssp, idx);

	/*
	 * Make sure that a lock is always counted if the corresponding
	 * unlock is counted. Needs to be a smp_mb() as the read side may
	 * contain a read from a variable that is written to before the
	 * synchronize_spcu() in the write side. In this case smp_mb()s
	 * A and B act like the store buffering pattern.
	 *
	 * This smp_mb() also pairs with smp_mb() C to prevent accesses
	 * after the synchronize_spcu() from being executed before the
	 * grace period ends.
	 */
	smp_mb(); /* A */

	/*
	 * If the locks are the same as the unlocks, then there must have
	 * been no readers on this index at some time in between. This does
	 * not mean that there are no more readers, as one could have read
	 * the current index but not have incremented the lock counter yet.
	 *
	 * So suppose that the updater is preempted here for so long
	 * that more than ULONG_MAX non-nested readers come and go in
	 * the meantime.  It turns out that this cannot result in overflow
	 * because if a reader modifies its unlock count after we read it
	 * above, then that reader's next load of ->spcu_idx is guaranteed
	 * to get the new value, which will cause it to operate on the
	 * other bank of counters, where it cannot contribute to the
	 * overflow of these counters.  This means that there is a maximum
	 * of 2*NR_CPUS increments, which cannot overflow given current
	 * systems, especially not on 64-bit systems.
	 *
	 * OK, how about nesting?  This does impose a limit on nesting
	 * of floor(ULONG_MAX/NR_CPUS/2), which should be sufficient,
	 * especially on 64-bit systems.
	 */
	return spcu_readers_lock_idx(ssp, idx) == unlocks;
}

/**
 * spcu_readers_active - returns true if there are readers. and false
 *                       otherwise
 * @ssp: which spcu_struct to count active readers (holding spcu_read_lock).
 *
 * Note that this is not an atomic primitive, and can therefore suffer
 * severe errors when invoked on an active spcu_struct.  That said, it
 * can be useful as an error check at cleanup time.
 */
static bool spcu_readers_active(struct spcu_struct *ssp)
{
	int cpu;
	unsigned long sum = 0;

	for_each_possible_cpu(cpu) {
		struct spcu_data *cpuc = per_cpu_ptr(ssp->sda, cpu);

		sum += READ_ONCE(cpuc->spcu_lock_count[0]);
		sum += READ_ONCE(cpuc->spcu_lock_count[1]);
		sum -= READ_ONCE(cpuc->spcu_unlock_count[0]);
		sum -= READ_ONCE(cpuc->spcu_unlock_count[1]);
	}
	return sum;
}

#define SPCU_INTERVAL		1

/*
 * Return grace-period delay, zero if there are expedited grace
 * periods pending, SPCU_INTERVAL otherwise.
 */
static unsigned long spcu_get_delay(struct spcu_struct *ssp)
{
	if (ULONG_CMP_LT(READ_ONCE(ssp->spcu_gp_seq),
			 READ_ONCE(ssp->spcu_gp_seq_needed_exp)))
		return 0;
	return SPCU_INTERVAL;
}

/**
 * cleanup_spcu_struct - deconstruct a sleep-PCU structure
 * @ssp: structure to clean up.
 *
 * Must invoke this after you are finished using a given spcu_struct that
 * was initialized via init_spcu_struct(), else you leak memory.
 */
void cleanup_spcu_struct(struct spcu_struct *ssp)
{
	int cpu;

	if (WARN_ON(!spcu_get_delay(ssp)))
		return; /* Just leak it! */
	if (WARN_ON(spcu_readers_active(ssp)))
		return; /* Just leak it! */
	flush_delayed_work(&ssp->work);
	for_each_possible_cpu(cpu) {
		struct spcu_data *sdp = per_cpu_ptr(ssp->sda, cpu);

		del_timer_sync(&sdp->delay_work);
		flush_work(&sdp->work);
		if (WARN_ON(pcu_segcblist_n_cbs(&sdp->spcu_cblist)))
			return; /* Forgot spcu_barrier(), so just leak it! */
	}
	if (WARN_ON(pcu_seq_state(READ_ONCE(ssp->spcu_gp_seq)) != SPCU_STATE_IDLE) ||
	    WARN_ON(spcu_readers_active(ssp))) {
		pr_info("%s: Active spcu_struct %p state: %d\n",
			__func__, ssp, pcu_seq_state(READ_ONCE(ssp->spcu_gp_seq)));
		return; /* Caller forgot to stop doing call_spcu()? */
	}
	free_percpu(ssp->sda);
	ssp->sda = NULL;
}
EXPORT_SYMBOL_GPL(cleanup_spcu_struct);

/*
 * Counts the new reader in the appropriate per-CPU element of the
 * spcu_struct.
 * Returns an index that must be passed to the matching spcu_read_unlock().
 */
int __spcu_read_lock(struct spcu_struct *ssp)
{
	int idx;

	idx = READ_ONCE(ssp->spcu_idx) & 0x1;
	this_cpu_inc(ssp->sda->spcu_lock_count[idx]);
	smp_mb(); /* B */  /* Avoid leaking the critical section. */
	return idx;
}
EXPORT_SYMBOL_GPL(__spcu_read_lock);

/*
 * Removes the count for the old reader from the appropriate per-CPU
 * element of the spcu_struct.  Note that this may well be a different
 * CPU than that which was incremented by the corresponding spcu_read_lock().
 */
void __spcu_read_unlock(struct spcu_struct *ssp, int idx)
{
	smp_mb(); /* C */  /* Avoid leaking the critical section. */
	this_cpu_inc(ssp->sda->spcu_unlock_count[idx]);
}
EXPORT_SYMBOL_GPL(__spcu_read_unlock);

/*
 * We use an adaptive strategy for synchronize_spcu() and especially for
 * synchronize_spcu_expedited().  We spin for a fixed time period
 * (defined below) to allow SPCU readers to exit their read-side critical
 * sections.  If there are still some readers after a few microseconds,
 * we repeatedly block for 1-millisecond time periods.
 */
#define SPCU_RETRY_CHECK_DELAY		5

/*
 * Start an SPCU grace period.
 */
static void spcu_gp_start(struct spcu_struct *ssp)
{
	struct spcu_data *sdp = this_cpu_ptr(ssp->sda);
	int state;

	lockdep_assert_held(&ACCESS_PRIVATE(ssp, lock));
	WARN_ON_ONCE(ULONG_CMP_GE(ssp->spcu_gp_seq, ssp->spcu_gp_seq_needed));
	spin_lock_pcu_node(sdp);  /* Interrupts already disabled. */
	pcu_segcblist_advance(&sdp->spcu_cblist,
			      pcu_seq_current(&ssp->spcu_gp_seq));
	(void)pcu_segcblist_accelerate(&sdp->spcu_cblist,
				       pcu_seq_snap(&ssp->spcu_gp_seq));
	spin_unlock_pcu_node(sdp);  /* Interrupts remain disabled. */
	smp_mb(); /* Order prior store to ->spcu_gp_seq_needed vs. GP start. */
	pcu_seq_start(&ssp->spcu_gp_seq);
	state = pcu_seq_state(ssp->spcu_gp_seq);
	WARN_ON_ONCE(state != SPCU_STATE_SCAN1);
}


static void spcu_delay_timer(struct timer_list *t)
{
	struct spcu_data *sdp = container_of(t, struct spcu_data, delay_work);

	queue_work_on(sdp->cpu, pcu_gp_wq, &sdp->work);
}

static void spcu_queue_delayed_work_on(struct spcu_data *sdp,
				       unsigned long delay)
{
	if (!delay) {
		queue_work_on(sdp->cpu, pcu_gp_wq, &sdp->work);
		return;
	}

	timer_reduce(&sdp->delay_work, jiffies + delay);
}

/*
 * Schedule callback invocation for the specified spcu_data structure,
 * if possible, on the corresponding CPU.
 */
static void spcu_schedule_cbs_sdp(struct spcu_data *sdp, unsigned long delay)
{
	spcu_queue_delayed_work_on(sdp, delay);
}

/*
 * Schedule callback invocation for all spcu_data structures associated
 * with the specified spcu_node structure that have callbacks for the
 * just-completed grace period, the one corresponding to idx.  If possible,
 * schedule this invocation on the corresponding CPUs.
 */
static void spcu_schedule_cbs_snp(struct spcu_struct *ssp, struct spcu_node *snp,
				  unsigned long mask, unsigned long delay)
{
	int cpu;

	for (cpu = snp->grplo; cpu <= snp->grphi; cpu++) {
		if (!(mask & (1 << (cpu - snp->grplo))))
			continue;
		spcu_schedule_cbs_sdp(per_cpu_ptr(ssp->sda, cpu), delay);
	}
}

/*
 * Note the end of an SPCU grace period.  Initiates callback invocation
 * and starts a new grace period if needed.
 *
 * The ->spcu_cb_mutex acquisition does not protect any data, but
 * instead prevents more than one grace period from starting while we
 * are initiating callback invocation.  This allows the ->spcu_have_cbs[]
 * array to have a finite number of elements.
 */
static void spcu_gp_end(struct spcu_struct *ssp)
{
	unsigned long cbdelay;
	bool cbs;
	bool last_lvl;
	int cpu;
	unsigned long flags;
	unsigned long gpseq;
	int idx;
	unsigned long mask;
	struct spcu_data *sdp;
	struct spcu_node *snp;

	/* Prevent more than one additional grace period. */
	mutex_lock(&ssp->spcu_cb_mutex);

	/* End the current grace period. */
	spin_lock_irq_pcu_node(ssp);
	idx = pcu_seq_state(ssp->spcu_gp_seq);
	WARN_ON_ONCE(idx != SPCU_STATE_SCAN2);
	cbdelay = spcu_get_delay(ssp);
	WRITE_ONCE(ssp->spcu_last_gp_end, ktime_get_mono_fast_ns());
	pcu_seq_end(&ssp->spcu_gp_seq);
	gpseq = pcu_seq_current(&ssp->spcu_gp_seq);
	if (ULONG_CMP_LT(ssp->spcu_gp_seq_needed_exp, gpseq))
		WRITE_ONCE(ssp->spcu_gp_seq_needed_exp, gpseq);
	spin_unlock_irq_pcu_node(ssp);
	mutex_unlock(&ssp->spcu_gp_mutex);
	/* A new grace period can start at this point.  But only one. */

	/* Initiate callback invocation as needed. */
	idx = pcu_seq_ctr(gpseq) % ARRAY_SIZE(snp->spcu_have_cbs);
	spcu_for_each_node_breadth_first(ssp, snp) {
		spin_lock_irq_pcu_node(snp);
		cbs = false;
		last_lvl = snp >= ssp->level[pcu_num_lvls - 1];
		if (last_lvl)
			cbs = snp->spcu_have_cbs[idx] == gpseq;
		snp->spcu_have_cbs[idx] = gpseq;
		pcu_seq_set_state(&snp->spcu_have_cbs[idx], 1);
		if (ULONG_CMP_LT(snp->spcu_gp_seq_needed_exp, gpseq))
			WRITE_ONCE(snp->spcu_gp_seq_needed_exp, gpseq);
		mask = snp->spcu_data_have_cbs[idx];
		snp->spcu_data_have_cbs[idx] = 0;
		spin_unlock_irq_pcu_node(snp);
		if (cbs)
			spcu_schedule_cbs_snp(ssp, snp, mask, cbdelay);

		/* Occasionally prevent spcu_data counter wrap. */
		if (!(gpseq & counter_wrap_check) && last_lvl)
			for (cpu = snp->grplo; cpu <= snp->grphi; cpu++) {
				sdp = per_cpu_ptr(ssp->sda, cpu);
				spin_lock_irqsave_pcu_node(sdp, flags);
				if (ULONG_CMP_GE(gpseq,
						 sdp->spcu_gp_seq_needed + 100))
					sdp->spcu_gp_seq_needed = gpseq;
				if (ULONG_CMP_GE(gpseq,
						 sdp->spcu_gp_seq_needed_exp + 100))
					sdp->spcu_gp_seq_needed_exp = gpseq;
				spin_unlock_irqrestore_pcu_node(sdp, flags);
			}
	}

	/* Callback initiation done, allow grace periods after next. */
	mutex_unlock(&ssp->spcu_cb_mutex);

	/* Start a new grace period if needed. */
	spin_lock_irq_pcu_node(ssp);
	gpseq = pcu_seq_current(&ssp->spcu_gp_seq);
	if (!pcu_seq_state(gpseq) &&
	    ULONG_CMP_LT(gpseq, ssp->spcu_gp_seq_needed)) {
		spcu_gp_start(ssp);
		spin_unlock_irq_pcu_node(ssp);
		spcu_reschedule(ssp, 0);
	} else {
		spin_unlock_irq_pcu_node(ssp);
	}
}

/*
 * Funnel-locking scheme to scalably mediate many concurrent expedited
 * grace-period requests.  This function is invoked for the first known
 * expedited request for a grace period that has already been requested,
 * but without expediting.  To start a completely new grace period,
 * whether expedited or not, use spcu_funnel_gp_start() instead.
 */
static void spcu_funnel_exp_start(struct spcu_struct *ssp, struct spcu_node *snp,
				  unsigned long s)
{
	unsigned long flags;

	for (; snp != NULL; snp = snp->spcu_parent) {
		if (pcu_seq_done(&ssp->spcu_gp_seq, s) ||
		    ULONG_CMP_GE(READ_ONCE(snp->spcu_gp_seq_needed_exp), s))
			return;
		spin_lock_irqsave_pcu_node(snp, flags);
		if (ULONG_CMP_GE(snp->spcu_gp_seq_needed_exp, s)) {
			spin_unlock_irqrestore_pcu_node(snp, flags);
			return;
		}
		WRITE_ONCE(snp->spcu_gp_seq_needed_exp, s);
		spin_unlock_irqrestore_pcu_node(snp, flags);
	}
	spin_lock_irqsave_pcu_node(ssp, flags);
	if (ULONG_CMP_LT(ssp->spcu_gp_seq_needed_exp, s))
		WRITE_ONCE(ssp->spcu_gp_seq_needed_exp, s);
	spin_unlock_irqrestore_pcu_node(ssp, flags);
}

/*
 * Funnel-locking scheme to scalably mediate many concurrent grace-period
 * requests.  The winner has to do the work of actually starting grace
 * period s.  Losers must either ensure that their desired grace-period
 * number is recorded on at least their leaf spcu_node structure, or they
 * must take steps to invoke their own callbacks.
 *
 * Note that this function also does the work of spcu_funnel_exp_start(),
 * in some cases by directly invoking it.
 */
static void spcu_funnel_gp_start(struct spcu_struct *ssp, struct spcu_data *sdp,
				 unsigned long s, bool do_norm)
{
	unsigned long flags;
	int idx = pcu_seq_ctr(s) % ARRAY_SIZE(sdp->mynode->spcu_have_cbs);
	struct spcu_node *snp = sdp->mynode;
	unsigned long snp_seq;

	/* Each pass through the loop does one level of the spcu_node tree. */
	for (; snp != NULL; snp = snp->spcu_parent) {
		if (pcu_seq_done(&ssp->spcu_gp_seq, s) && snp != sdp->mynode)
			return; /* GP already done and CBs recorded. */
		spin_lock_irqsave_pcu_node(snp, flags);
		if (ULONG_CMP_GE(snp->spcu_have_cbs[idx], s)) {
			snp_seq = snp->spcu_have_cbs[idx];
			if (snp == sdp->mynode && snp_seq == s)
				snp->spcu_data_have_cbs[idx] |= sdp->grpmask;
			spin_unlock_irqrestore_pcu_node(snp, flags);
			if (snp == sdp->mynode && snp_seq != s) {
				spcu_schedule_cbs_sdp(sdp, do_norm
							   ? SPCU_INTERVAL
							   : 0);
				return;
			}
			if (!do_norm)
				spcu_funnel_exp_start(ssp, snp, s);
			return;
		}
		snp->spcu_have_cbs[idx] = s;
		if (snp == sdp->mynode)
			snp->spcu_data_have_cbs[idx] |= sdp->grpmask;
		if (!do_norm && ULONG_CMP_LT(snp->spcu_gp_seq_needed_exp, s))
			WRITE_ONCE(snp->spcu_gp_seq_needed_exp, s);
		spin_unlock_irqrestore_pcu_node(snp, flags);
	}

	/* Top of tree, must ensure the grace period will be started. */
	spin_lock_irqsave_pcu_node(ssp, flags);
	if (ULONG_CMP_LT(ssp->spcu_gp_seq_needed, s)) {
		/*
		 * Record need for grace period s.  Pair with load
		 * acquire setting up for initialization.
		 */
		smp_store_release(&ssp->spcu_gp_seq_needed, s); /*^^^*/
	}
	if (!do_norm && ULONG_CMP_LT(ssp->spcu_gp_seq_needed_exp, s))
		WRITE_ONCE(ssp->spcu_gp_seq_needed_exp, s);

	/* If grace period not already done and none in progress, start it. */
	if (!pcu_seq_done(&ssp->spcu_gp_seq, s) &&
	    pcu_seq_state(ssp->spcu_gp_seq) == SPCU_STATE_IDLE) {
		WARN_ON_ONCE(ULONG_CMP_GE(ssp->spcu_gp_seq, ssp->spcu_gp_seq_needed));
		spcu_gp_start(ssp);
		if (likely(spcu_init_done))
			queue_delayed_work(pcu_gp_wq, &ssp->work,
					   spcu_get_delay(ssp));
		else if (list_empty(&ssp->work.work.entry))
			list_add(&ssp->work.work.entry, &spcu_boot_list);
	}
	spin_unlock_irqrestore_pcu_node(ssp, flags);
}

/*
 * Wait until all readers counted by array index idx complete, but
 * loop an additional time if there is an expedited grace period pending.
 * The caller must ensure that ->spcu_idx is not changed while checking.
 */
static bool try_check_zero(struct spcu_struct *ssp, int idx, int trycount)
{
	for (;;) {
		if (spcu_readers_active_idx_check(ssp, idx))
			return true;
		if (--trycount + !spcu_get_delay(ssp) <= 0)
			return false;
		udelay(SPCU_RETRY_CHECK_DELAY);
	}
}

/*
 * Increment the ->spcu_idx counter so that future SPCU readers will
 * use the other rank of the ->spcu_(un)lock_count[] arrays.  This allows
 * us to wait for pre-existing readers in a starvation-free manner.
 */
static void spcu_flip(struct spcu_struct *ssp)
{
	/*
	 * Ensure that if this updater saw a given reader's increment
	 * from __spcu_read_lock(), that reader was using an old value
	 * of ->spcu_idx.  Also ensure that if a given reader sees the
	 * new value of ->spcu_idx, this updater's earlier scans cannot
	 * have seen that reader's increments (which is OK, because this
	 * grace period need not wait on that reader).
	 */
	smp_mb(); /* E */  /* Pairs with B and C. */

	WRITE_ONCE(ssp->spcu_idx, ssp->spcu_idx + 1);

	/*
	 * Ensure that if the updater misses an __spcu_read_unlock()
	 * increment, that task's next __spcu_read_lock() will see the
	 * above counter update.  Note that both this memory barrier
	 * and the one in spcu_readers_active_idx_check() provide the
	 * guarantee for __spcu_read_lock().
	 */
	smp_mb(); /* D */  /* Pairs with C. */
}

/*
 * If SPCU is likely idle, return true, otherwise return false.
 *
 * Note that it is OK for several current from-idle requests for a new
 * grace period from idle to specify expediting because they will all end
 * up requesting the same grace period anyhow.  So no loss.
 *
 * Note also that if any CPU (including the current one) is still invoking
 * callbacks, this function will nevertheless say "idle".  This is not
 * ideal, but the overhead of checking all CPUs' callback lists is even
 * less ideal, especially on large systems.  Furthermore, the wakeup
 * can happen before the callback is fully removed, so we have no choice
 * but to accept this type of error.
 *
 * This function is also subject to counter-wrap errors, but let's face
 * it, if this function was preempted for enough time for the counters
 * to wrap, it really doesn't matter whether or not we expedite the grace
 * period.  The extra overhead of a needlessly expedited grace period is
 * negligible when amortized over that time period, and the extra latency
 * of a needlessly non-expedited grace period is similarly negligible.
 */
static bool spcu_might_be_idle(struct spcu_struct *ssp)
{
	unsigned long curseq;
	unsigned long flags;
	struct spcu_data *sdp;
	unsigned long t;
	unsigned long tlast;

	check_init_spcu_struct(ssp);
	/* If the local spcu_data structure has callbacks, not idle.  */
	sdp = raw_cpu_ptr(ssp->sda);
	spin_lock_irqsave_pcu_node(sdp, flags);
	if (pcu_segcblist_pend_cbs(&sdp->spcu_cblist)) {
		spin_unlock_irqrestore_pcu_node(sdp, flags);
		return false; /* Callbacks already present, so not idle. */
	}
	spin_unlock_irqrestore_pcu_node(sdp, flags);

	/*
	 * No local callbacks, so probabalistically probe global state.
	 * Exact information would require acquiring locks, which would
	 * kill scalability, hence the probabalistic nature of the probe.
	 */

	/* First, see if enough time has passed since the last GP. */
	t = ktime_get_mono_fast_ns();
	tlast = READ_ONCE(ssp->spcu_last_gp_end);
	if (exp_holdoff == 0 ||
	    time_in_range_open(t, tlast, tlast + exp_holdoff))
		return false; /* Too soon after last GP. */

	/* Next, check for probable idleness. */
	curseq = pcu_seq_current(&ssp->spcu_gp_seq);
	smp_mb(); /* Order ->spcu_gp_seq with ->spcu_gp_seq_needed. */
	if (ULONG_CMP_LT(curseq, READ_ONCE(ssp->spcu_gp_seq_needed)))
		return false; /* Grace period in progress, so not idle. */
	smp_mb(); /* Order ->spcu_gp_seq with prior access. */
	if (curseq != pcu_seq_current(&ssp->spcu_gp_seq))
		return false; /* GP # changed, so not idle. */
	return true; /* With reasonable probability, idle! */
}

/*
 * SPCU callback function to leak a callback.
 */
static void spcu_leak_callback(struct pcu_head *rhp)
{
}

/*
 * Start an SPCU grace period, and also queue the callback if non-NULL.
 */
static unsigned long spcu_gp_start_if_needed(struct spcu_struct *ssp,
					     struct pcu_head *rhp, bool do_norm)
{
	unsigned long flags;
	int idx;
	bool needexp = false;
	bool needgp = false;
	unsigned long s;
	struct spcu_data *sdp;

	check_init_spcu_struct(ssp);
	idx = spcu_read_lock(ssp);
	sdp = raw_cpu_ptr(ssp->sda);
	spin_lock_irqsave_pcu_node(sdp, flags);
	if (rhp)
		pcu_segcblist_enqueue(&sdp->spcu_cblist, rhp);
	pcu_segcblist_advance(&sdp->spcu_cblist,
			      pcu_seq_current(&ssp->spcu_gp_seq));
	s = pcu_seq_snap(&ssp->spcu_gp_seq);
	(void)pcu_segcblist_accelerate(&sdp->spcu_cblist, s);
	if (ULONG_CMP_LT(sdp->spcu_gp_seq_needed, s)) {
		sdp->spcu_gp_seq_needed = s;
		needgp = true;
	}
	if (!do_norm && ULONG_CMP_LT(sdp->spcu_gp_seq_needed_exp, s)) {
		sdp->spcu_gp_seq_needed_exp = s;
		needexp = true;
	}
	spin_unlock_irqrestore_pcu_node(sdp, flags);
	if (needgp)
		spcu_funnel_gp_start(ssp, sdp, s, do_norm);
	else if (needexp)
		spcu_funnel_exp_start(ssp, sdp->mynode, s);
	spcu_read_unlock(ssp, idx);
	return s;
}

/*
 * Enqueue an SPCU callback on the spcu_data structure associated with
 * the current CPU and the specified spcu_struct structure, initiating
 * grace-period processing if it is not already running.
 *
 * Note that all CPUs must agree that the grace period extended beyond
 * all pre-existing SPCU read-side critical section.  On systems with
 * more than one CPU, this means that when "func()" is invoked, each CPU
 * is guaranteed to have executed a full memory barrier since the end of
 * its last corresponding SPCU read-side critical section whose beginning
 * preceded the call to call_spcu().  It also means that each CPU executing
 * an SPCU read-side critical section that continues beyond the start of
 * "func()" must have executed a memory barrier after the call_spcu()
 * but before the beginning of that SPCU read-side critical section.
 * Note that these guarantees include CPUs that are offline, idle, or
 * executing in user mode, as well as CPUs that are executing in the kernel.
 *
 * Furthermore, if CPU A invoked call_spcu() and CPU B invoked the
 * resulting SPCU callback function "func()", then both CPU A and CPU
 * B are guaranteed to execute a full memory barrier during the time
 * interval between the call to call_spcu() and the invocation of "func()".
 * This guarantee applies even if CPU A and CPU B are the same CPU (but
 * again only if the system has more than one CPU).
 *
 * Of course, these guarantees apply only for invocations of call_spcu(),
 * spcu_read_lock(), and spcu_read_unlock() that are all passed the same
 * spcu_struct structure.
 */
static void __call_spcu(struct spcu_struct *ssp, struct pcu_head *rhp,
			pcu_callback_t func, bool do_norm)
{
	if (debug_pcu_head_queue(rhp)) {
		/* Probable double call_spcu(), so leak the callback. */
		WRITE_ONCE(rhp->func, spcu_leak_callback);
		WARN_ONCE(1, "call_spcu(): Leaked duplicate callback\n");
		return;
	}
	rhp->func = func;
	(void)spcu_gp_start_if_needed(ssp, rhp, do_norm);
}

/**
 * call_spcu() - Queue a callback for invocation after an SPCU grace period
 * @ssp: spcu_struct in queue the callback
 * @rhp: structure to be used for queueing the SPCU callback.
 * @func: function to be invoked after the SPCU grace period
 *
 * The callback function will be invoked some time after a full SPCU
 * grace period elapses, in other words after all pre-existing SPCU
 * read-side critical sections have completed.  However, the callback
 * function might well execute concurrently with other SPCU read-side
 * critical sections that started after call_spcu() was invoked.  SPCU
 * read-side critical sections are delimited by spcu_read_lock() and
 * spcu_read_unlock(), and may be nested.
 *
 * The callback will be invoked from process context, but must nevertheless
 * be fast and must not block.
 */
void call_spcu(struct spcu_struct *ssp, struct pcu_head *rhp,
	       pcu_callback_t func)
{
	__call_spcu(ssp, rhp, func, true);
}
EXPORT_SYMBOL_GPL(call_spcu);

/*
 * Helper function for synchronize_spcu() and synchronize_spcu_expedited().
 */
static void __synchronize_spcu(struct spcu_struct *ssp, bool do_norm)
{
	struct pcu_synchronize pcu;

	PCU_LOCKDEP_WARN(lockdep_is_held(ssp) ||
			 lock_is_held(&pcu_bh_lock_map) ||
			 lock_is_held(&pcu_lock_map) ||
			 lock_is_held(&pcu_sched_lock_map),
			 "Illegal synchronize_spcu() in same-type SPCU (or in PCU) read-side critical section");

	if (pcu_scheduler_active == PCU_SCHEDULER_INACTIVE)
		return;
	might_sleep();
	check_init_spcu_struct(ssp);
	init_completion(&pcu.completion);
	init_pcu_head_on_stack(&pcu.head);
	__call_spcu(ssp, &pcu.head, wakeme_after_pcu, do_norm);
	wait_for_completion(&pcu.completion);
	destroy_pcu_head_on_stack(&pcu.head);

	/*
	 * Make sure that later code is ordered after the SPCU grace
	 * period.  This pairs with the spin_lock_irq_pcu_node()
	 * in spcu_invoke_callbacks().  Unlike Tree PCU, this is needed
	 * because the current CPU might have been totally uninvolved with
	 * (and thus unordered against) that grace period.
	 */
	smp_mb();
}

/**
 * synchronize_spcu_expedited - Brute-force SPCU grace period
 * @ssp: spcu_struct with which to synchronize.
 *
 * Wait for an SPCU grace period to elapse, but be more aggressive about
 * spinning rather than blocking when waiting.
 *
 * Note that synchronize_spcu_expedited() has the same deadlock and
 * memory-ordering properties as does synchronize_spcu().
 */
void synchronize_spcu_expedited(struct spcu_struct *ssp)
{
	__synchronize_spcu(ssp, pcu_gp_is_normal());
}
EXPORT_SYMBOL_GPL(synchronize_spcu_expedited);

/**
 * synchronize_spcu - wait for prior SPCU read-side critical-section completion
 * @ssp: spcu_struct with which to synchronize.
 *
 * Wait for the count to drain to zero of both indexes. To avoid the
 * possible starvation of synchronize_spcu(), it waits for the count of
 * the index=((->spcu_idx & 1) ^ 1) to drain to zero at first,
 * and then flip the spcu_idx and wait for the count of the other index.
 *
 * Can block; must be called from process context.
 *
 * Note that it is illegal to call synchronize_spcu() from the corresponding
 * SPCU read-side critical section; doing so will result in deadlock.
 * However, it is perfectly legal to call synchronize_spcu() on one
 * spcu_struct from some other spcu_struct's read-side critical section,
 * as long as the resulting graph of spcu_structs is acyclic.
 *
 * There are memory-ordering constraints implied by synchronize_spcu().
 * On systems with more than one CPU, when synchronize_spcu() returns,
 * each CPU is guaranteed to have executed a full memory barrier since
 * the end of its last corresponding SPCU read-side critical section
 * whose beginning preceded the call to synchronize_spcu().  In addition,
 * each CPU having an SPCU read-side critical section that extends beyond
 * the return from synchronize_spcu() is guaranteed to have executed a
 * full memory barrier after the beginning of synchronize_spcu() and before
 * the beginning of that SPCU read-side critical section.  Note that these
 * guarantees include CPUs that are offline, idle, or executing in user mode,
 * as well as CPUs that are executing in the kernel.
 *
 * Furthermore, if CPU A invoked synchronize_spcu(), which returned
 * to its caller on CPU B, then both CPU A and CPU B are guaranteed
 * to have executed a full memory barrier during the execution of
 * synchronize_spcu().  This guarantee applies even if CPU A and CPU B
 * are the same CPU, but again only if the system has more than one CPU.
 *
 * Of course, these memory-ordering guarantees apply only when
 * synchronize_spcu(), spcu_read_lock(), and spcu_read_unlock() are
 * passed the same spcu_struct structure.
 *
 * Implementation of these memory-ordering guarantees is similar to
 * that of synchronize_pcu().
 *
 * If SPCU is likely idle, expedite the first request.  This semantic
 * was provided by Classic SPCU, and is relied upon by its users, so TREE
 * SPCU must also provide it.  Note that detecting idleness is heuristic
 * and subject to both false positives and negatives.
 */
void synchronize_spcu(struct spcu_struct *ssp)
{
	if (spcu_might_be_idle(ssp) || pcu_gp_is_expedited())
		synchronize_spcu_expedited(ssp);
	else
		__synchronize_spcu(ssp, true);
}
EXPORT_SYMBOL_GPL(synchronize_spcu);

/**
 * get_state_synchronize_spcu - Provide an end-of-grace-period cookie
 * @ssp: spcu_struct to provide cookie for.
 *
 * This function returns a cookie that can be passed to
 * poll_state_synchronize_spcu(), which will return true if a full grace
 * period has elapsed in the meantime.  It is the caller's responsibility
 * to make sure that grace period happens, for example, by invoking
 * call_spcu() after return from get_state_synchronize_spcu().
 */
unsigned long get_state_synchronize_spcu(struct spcu_struct *ssp)
{
	// Any prior manipulation of SPCU-protected data must happen
	// before the load from ->spcu_gp_seq.
	smp_mb();
	return pcu_seq_snap(&ssp->spcu_gp_seq);
}
EXPORT_SYMBOL_GPL(get_state_synchronize_spcu);

/**
 * start_poll_synchronize_spcu - Provide cookie and start grace period
 * @ssp: spcu_struct to provide cookie for.
 *
 * This function returns a cookie that can be passed to
 * poll_state_synchronize_spcu(), which will return true if a full grace
 * period has elapsed in the meantime.  Unlike get_state_synchronize_spcu(),
 * this function also ensures that any needed SPCU grace period will be
 * started.  This convenience does come at a cost in terms of CPU overhead.
 */
unsigned long start_poll_synchronize_spcu(struct spcu_struct *ssp)
{
	return spcu_gp_start_if_needed(ssp, NULL, true);
}
EXPORT_SYMBOL_GPL(start_poll_synchronize_spcu);

/**
 * poll_state_synchronize_spcu - Has cookie's grace period ended?
 * @ssp: spcu_struct to provide cookie for.
 * @cookie: Return value from get_state_synchronize_spcu() or start_poll_synchronize_spcu().
 *
 * This function takes the cookie that was returned from either
 * get_state_synchronize_spcu() or start_poll_synchronize_spcu(), and
 * returns @true if an SPCU grace period elapsed since the time that the
 * cookie was created.
 *
 * Because cookies are finite in size, wrapping/overflow is possible.
 * This is more pronounced on 32-bit systems where cookies are 32 bits,
 * where in theory wrapping could happen in about 14 hours assuming
 * 25-microsecond expedited SPCU grace periods.  However, a more likely
 * overflow lower bound is on the order of 24 days in the case of
 * one-millisecond SPCU grace periods.  Of course, wrapping in a 64-bit
 * system requires geologic timespans, as in more than seven million years
 * even for expedited SPCU grace periods.
 *
 * Wrapping/overflow is much more of an issue for CONFIG_SMP=n systems
 * that also have CONFIG_PREEMPTION=n, which selects Tiny SPCU.  This uses
 * a 16-bit cookie, which pcutorture routinely wraps in a matter of a
 * few minutes.  If this proves to be a problem, this counter will be
 * expanded to the same size as for Tree SPCU.
 */
bool poll_state_synchronize_spcu(struct spcu_struct *ssp, unsigned long cookie)
{
	if (!pcu_seq_done(&ssp->spcu_gp_seq, cookie))
		return false;
	// Ensure that the end of the SPCU grace period happens before
	// any subsequent code that the caller might execute.
	smp_mb(); // ^^^
	return true;
}
EXPORT_SYMBOL_GPL(poll_state_synchronize_spcu);

/*
 * Callback function for spcu_barrier() use.
 */
static void spcu_barrier_cb(struct pcu_head *rhp)
{
	struct spcu_data *sdp;
	struct spcu_struct *ssp;

	sdp = container_of(rhp, struct spcu_data, spcu_barrier_head);
	ssp = sdp->ssp;
	if (atomic_dec_and_test(&ssp->spcu_barrier_cpu_cnt))
		complete(&ssp->spcu_barrier_completion);
}

/**
 * spcu_barrier - Wait until all in-flight call_spcu() callbacks complete.
 * @ssp: spcu_struct on which to wait for in-flight callbacks.
 */
void spcu_barrier(struct spcu_struct *ssp)
{
	int cpu;
	struct spcu_data *sdp;
	unsigned long s = pcu_seq_snap(&ssp->spcu_barrier_seq);

	check_init_spcu_struct(ssp);
	mutex_lock(&ssp->spcu_barrier_mutex);
	if (pcu_seq_done(&ssp->spcu_barrier_seq, s)) {
		smp_mb(); /* Force ordering following return. */
		mutex_unlock(&ssp->spcu_barrier_mutex);
		return; /* Someone else did our work for us. */
	}
	pcu_seq_start(&ssp->spcu_barrier_seq);
	init_completion(&ssp->spcu_barrier_completion);

	/* Initial count prevents reaching zero until all CBs are posted. */
	atomic_set(&ssp->spcu_barrier_cpu_cnt, 1);

	/*
	 * Each pass through this loop enqueues a callback, but only
	 * on CPUs already having callbacks enqueued.  Note that if
	 * a CPU already has callbacks enqueue, it must have already
	 * registered the need for a future grace period, so all we
	 * need do is enqueue a callback that will use the same
	 * grace period as the last callback already in the queue.
	 */
	for_each_possible_cpu(cpu) {
		sdp = per_cpu_ptr(ssp->sda, cpu);
		spin_lock_irq_pcu_node(sdp);
		atomic_inc(&ssp->spcu_barrier_cpu_cnt);
		sdp->spcu_barrier_head.func = spcu_barrier_cb;
		debug_pcu_head_queue(&sdp->spcu_barrier_head);
		if (!pcu_segcblist_entrain(&sdp->spcu_cblist,
					   &sdp->spcu_barrier_head)) {
			debug_pcu_head_unqueue(&sdp->spcu_barrier_head);
			atomic_dec(&ssp->spcu_barrier_cpu_cnt);
		}
		spin_unlock_irq_pcu_node(sdp);
	}

	/* Remove the initial count, at which point reaching zero can happen. */
	if (atomic_dec_and_test(&ssp->spcu_barrier_cpu_cnt))
		complete(&ssp->spcu_barrier_completion);
	wait_for_completion(&ssp->spcu_barrier_completion);

	pcu_seq_end(&ssp->spcu_barrier_seq);
	mutex_unlock(&ssp->spcu_barrier_mutex);
}
EXPORT_SYMBOL_GPL(spcu_barrier);

/**
 * spcu_batches_completed - return batches completed.
 * @ssp: spcu_struct on which to report batch completion.
 *
 * Report the number of batches, correlated with, but not necessarily
 * precisely the same as, the number of grace periods that have elapsed.
 */
unsigned long spcu_batches_completed(struct spcu_struct *ssp)
{
	return READ_ONCE(ssp->spcu_idx);
}
EXPORT_SYMBOL_GPL(spcu_batches_completed);

/*
 * Core SPCU state machine.  Push state bits of ->spcu_gp_seq
 * to SPCU_STATE_SCAN2, and invoke spcu_gp_end() when scan has
 * completed in that state.
 */
static void spcu_advance_state(struct spcu_struct *ssp)
{
	int idx;

	mutex_lock(&ssp->spcu_gp_mutex);

	/*
	 * Because readers might be delayed for an extended period after
	 * fetching ->spcu_idx for their index, at any point in time there
	 * might well be readers using both idx=0 and idx=1.  We therefore
	 * need to wait for readers to clear from both index values before
	 * invoking a callback.
	 *
	 * The load-acquire ensures that we see the accesses performed
	 * by the prior grace period.
	 */
	idx = pcu_seq_state(smp_load_acquire(&ssp->spcu_gp_seq)); /* ^^^ */
	if (idx == SPCU_STATE_IDLE) {
		spin_lock_irq_pcu_node(ssp);
		if (ULONG_CMP_GE(ssp->spcu_gp_seq, ssp->spcu_gp_seq_needed)) {
			WARN_ON_ONCE(pcu_seq_state(ssp->spcu_gp_seq));
			spin_unlock_irq_pcu_node(ssp);
			mutex_unlock(&ssp->spcu_gp_mutex);
			return;
		}
		idx = pcu_seq_state(READ_ONCE(ssp->spcu_gp_seq));
		if (idx == SPCU_STATE_IDLE)
			spcu_gp_start(ssp);
		spin_unlock_irq_pcu_node(ssp);
		if (idx != SPCU_STATE_IDLE) {
			mutex_unlock(&ssp->spcu_gp_mutex);
			return; /* Someone else started the grace period. */
		}
	}

	if (pcu_seq_state(READ_ONCE(ssp->spcu_gp_seq)) == SPCU_STATE_SCAN1) {
		idx = 1 ^ (ssp->spcu_idx & 1);
		if (!try_check_zero(ssp, idx, 1)) {
			mutex_unlock(&ssp->spcu_gp_mutex);
			return; /* readers present, retry later. */
		}
		spcu_flip(ssp);
		spin_lock_irq_pcu_node(ssp);
		pcu_seq_set_state(&ssp->spcu_gp_seq, SPCU_STATE_SCAN2);
		spin_unlock_irq_pcu_node(ssp);
	}

	if (pcu_seq_state(READ_ONCE(ssp->spcu_gp_seq)) == SPCU_STATE_SCAN2) {

		/*
		 * SPCU read-side critical sections are normally short,
		 * so check at least twice in quick succession after a flip.
		 */
		idx = 1 ^ (ssp->spcu_idx & 1);
		if (!try_check_zero(ssp, idx, 2)) {
			mutex_unlock(&ssp->spcu_gp_mutex);
			return; /* readers present, retry later. */
		}
		spcu_gp_end(ssp);  /* Releases ->spcu_gp_mutex. */
	}
}

/*
 * Invoke a limited number of SPCU callbacks that have passed through
 * their grace period.  If there are more to do, SPCU will reschedule
 * the workqueue.  Note that needed memory barriers have been executed
 * in this task's context by spcu_readers_active_idx_check().
 */
static void spcu_invoke_callbacks(struct work_struct *work)
{
	long len;
	bool more;
	struct pcu_cblist ready_cbs;
	struct pcu_head *rhp;
	struct spcu_data *sdp;
	struct spcu_struct *ssp;

	sdp = container_of(work, struct spcu_data, work);

	ssp = sdp->ssp;
	pcu_cblist_init(&ready_cbs);
	spin_lock_irq_pcu_node(sdp);
	pcu_segcblist_advance(&sdp->spcu_cblist,
			      pcu_seq_current(&ssp->spcu_gp_seq));
	if (sdp->spcu_cblist_invoking ||
	    !pcu_segcblist_ready_cbs(&sdp->spcu_cblist)) {
		spin_unlock_irq_pcu_node(sdp);
		return;  /* Someone else on the job or nothing to do. */
	}

	/* We are on the job!  Extract and invoke ready callbacks. */
	sdp->spcu_cblist_invoking = true;
	pcu_segcblist_extract_done_cbs(&sdp->spcu_cblist, &ready_cbs);
	len = ready_cbs.len;
	spin_unlock_irq_pcu_node(sdp);
	rhp = pcu_cblist_dequeue(&ready_cbs);
	for (; rhp != NULL; rhp = pcu_cblist_dequeue(&ready_cbs)) {
		debug_pcu_head_unqueue(rhp);
		local_bh_disable();
		rhp->func(rhp);
		local_bh_enable();
	}
	WARN_ON_ONCE(ready_cbs.len);

	/*
	 * Update counts, accelerate new callbacks, and if needed,
	 * schedule another round of callback invocation.
	 */
	spin_lock_irq_pcu_node(sdp);
	pcu_segcblist_add_len(&sdp->spcu_cblist, -len);
	(void)pcu_segcblist_accelerate(&sdp->spcu_cblist,
				       pcu_seq_snap(&ssp->spcu_gp_seq));
	sdp->spcu_cblist_invoking = false;
	more = pcu_segcblist_ready_cbs(&sdp->spcu_cblist);
	spin_unlock_irq_pcu_node(sdp);
	if (more)
		spcu_schedule_cbs_sdp(sdp, 0);
}

/*
 * Finished one round of SPCU grace period.  Start another if there are
 * more SPCU callbacks queued, otherwise put SPCU into not-running state.
 */
static void spcu_reschedule(struct spcu_struct *ssp, unsigned long delay)
{
	bool pushgp = true;

	spin_lock_irq_pcu_node(ssp);
	if (ULONG_CMP_GE(ssp->spcu_gp_seq, ssp->spcu_gp_seq_needed)) {
		if (!WARN_ON_ONCE(pcu_seq_state(ssp->spcu_gp_seq))) {
			/* All requests fulfilled, time to go idle. */
			pushgp = false;
		}
	} else if (!pcu_seq_state(ssp->spcu_gp_seq)) {
		/* Outstanding request and no GP.  Start one. */
		spcu_gp_start(ssp);
	}
	spin_unlock_irq_pcu_node(ssp);

	if (pushgp)
		queue_delayed_work(pcu_gp_wq, &ssp->work, delay);
}

/*
 * This is the work-queue function that handles SPCU grace periods.
 */
static void process_spcu(struct work_struct *work)
{
	struct spcu_struct *ssp;

	ssp = container_of(work, struct spcu_struct, work.work);

	spcu_advance_state(ssp);
	spcu_reschedule(ssp, spcu_get_delay(ssp));
}

void spcutorture_get_gp_data(enum pcutorture_type test_type,
			     struct spcu_struct *ssp, int *flags,
			     unsigned long *gp_seq)
{
	if (test_type != SPCU_FLAVOR)
		return;
	*flags = 0;
	*gp_seq = pcu_seq_current(&ssp->spcu_gp_seq);
}
EXPORT_SYMBOL_GPL(spcutorture_get_gp_data);

void spcu_torture_stats_print(struct spcu_struct *ssp, char *tt, char *tf)
{
	int cpu;
	int idx;
	unsigned long s0 = 0, s1 = 0;

	idx = ssp->spcu_idx & 0x1;
	pr_alert("%s%s Tree SPCU g%ld per-CPU(idx=%d):",
		 tt, tf, pcu_seq_current(&ssp->spcu_gp_seq), idx);
	for_each_possible_cpu(cpu) {
		unsigned long l0, l1;
		unsigned long u0, u1;
		long c0, c1;
		struct spcu_data *sdp;

		sdp = per_cpu_ptr(ssp->sda, cpu);
		u0 = data_race(sdp->spcu_unlock_count[!idx]);
		u1 = data_race(sdp->spcu_unlock_count[idx]);

		/*
		 * Make sure that a lock is always counted if the corresponding
		 * unlock is counted.
		 */
		smp_rmb();

		l0 = data_race(sdp->spcu_lock_count[!idx]);
		l1 = data_race(sdp->spcu_lock_count[idx]);

		c0 = l0 - u0;
		c1 = l1 - u1;
		pr_cont(" %d(%ld,%ld %c)",
			cpu, c0, c1,
			"C."[pcu_segcblist_empty(&sdp->spcu_cblist)]);
		s0 += c0;
		s1 += c1;
	}
	pr_cont(" T(%ld,%ld)\n", s0, s1);
}
EXPORT_SYMBOL_GPL(spcu_torture_stats_print);

int spcu_bootup_announce(void)
{
	pr_info("Hierarchical SPCU implementation.\n");
	if (exp_holdoff != DEFAULT_SPCU_EXP_HOLDOFF)
		pr_info("\tNon-default auto-expedite holdoff of %lu ns.\n", exp_holdoff);
	return 0;
}
//early_initcall(spcu_bootup_announce);

void spcu_init(void)
{
	struct spcu_struct *ssp;

	/*
	 * Once that is set, call_spcu() can follow the normal path and
	 * queue delayed work. This must follow PCU workqueues creation
	 * and timers initialization.
	 */
	spcu_init_done = true;
	while (!list_empty(&spcu_boot_list)) {
		ssp = list_first_entry(&spcu_boot_list, struct spcu_struct,
				      work.work.entry);
		list_del_init(&ssp->work.work.entry);
		queue_work(pcu_gp_wq, &ssp->work.work);
	}
}
