/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * PCU expedited grace periods
 *
 * Copyright IBM Corporation, 2016
 *
 * Authors: Paul E. McKenney <paulmck@linux.ibm.com>
 */

#include <linux/lockdep.h>

static void pcu_exp_handler(void *unused);
static int pcu_print_task_exp_stall(struct pcu_node *rnp);

/*
 * Record the start of an expedited grace period.
 */
static void pcu_exp_gp_seq_start(void)
{
	pcu_seq_start(&pcu_state.expedited_sequence);
}

/*
 * Return the value that the expedited-grace-period counter will have
 * at the end of the current grace period.
 */
static __maybe_unused unsigned long pcu_exp_gp_seq_endval(void)
{
	return pcu_seq_endval(&pcu_state.expedited_sequence);
}

/*
 * Record the end of an expedited grace period.
 */
static void pcu_exp_gp_seq_end(void)
{
	pcu_seq_end(&pcu_state.expedited_sequence);
	smp_mb(); /* Ensure that consecutive grace periods serialize. */
}

/*
 * Take a snapshot of the expedited-grace-period counter, which is the
 * earliest value that will indicate that a full grace period has
 * elapsed since the current time.
 */
static unsigned long pcu_exp_gp_seq_snap(void)
{
	unsigned long s;

	smp_mb(); /* Caller's modifications seen first by other CPUs. */
	s = pcu_seq_snap(&pcu_state.expedited_sequence);
	//trace_pcu_exp_grace_period(pcu_state.name, s, TPS("snap"));
	return s;
}

/*
 * Given a counter snapshot from pcu_exp_gp_seq_snap(), return true
 * if a full expedited grace period has elapsed since that snapshot
 * was taken.
 */
static bool pcu_exp_gp_seq_done(unsigned long s)
{
	return pcu_seq_done(&pcu_state.expedited_sequence, s);
}

/*
 * Reset the ->expmaskinit values in the pcu_node tree to reflect any
 * recent CPU-online activity.  Note that these masks are not cleared
 * when CPUs go offline, so they reflect the union of all CPUs that have
 * ever been online.  This means that this function normally takes its
 * no-work-to-do fastpath.
 */
static void sync_exp_reset_tree_hotplug(void)
{
	bool done;
	unsigned long flags;
	unsigned long mask;
	unsigned long oldmask;
	int ncpus = smp_load_acquire(&pcu_state.ncpus); /* Order vs. locking. */
	struct pcu_node *rnp;
	struct pcu_node *rnp_up;

	/* If no new CPUs onlined since last time, nothing to do. */
	if (likely(ncpus == pcu_state.ncpus_snap))
		return;
	pcu_state.ncpus_snap = ncpus;

	/*
	 * Each pass through the following loop propagates newly onlined
	 * CPUs for the current pcu_node structure up the pcu_node tree.
	 */
	pcu_for_each_leaf_node(rnp) {
		raw_spin_lock_irqsave_pcu_node(rnp, flags);
		if (rnp->expmaskinit == rnp->expmaskinitnext) {
			raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
			continue;  /* No new CPUs, nothing to do. */
		}

		/* Update this node's mask, track old value for propagation. */
		oldmask = rnp->expmaskinit;
		rnp->expmaskinit = rnp->expmaskinitnext;
		raw_spin_unlock_irqrestore_pcu_node(rnp, flags);

		/* If was already nonzero, nothing to propagate. */
		if (oldmask)
			continue;

		/* Propagate the new CPU up the tree. */
		mask = rnp->grpmask;
		rnp_up = rnp->parent;
		done = false;
		while (rnp_up) {
			raw_spin_lock_irqsave_pcu_node(rnp_up, flags);
			if (rnp_up->expmaskinit)
				done = true;
			rnp_up->expmaskinit |= mask;
			raw_spin_unlock_irqrestore_pcu_node(rnp_up, flags);
			if (done)
				break;
			mask = rnp_up->grpmask;
			rnp_up = rnp_up->parent;
		}
	}
}

/*
 * Reset the ->expmask values in the pcu_node tree in preparation for
 * a new expedited grace period.
 */
static void __maybe_unused sync_exp_reset_tree(void)
{
	unsigned long flags;
	struct pcu_node *rnp;

	sync_exp_reset_tree_hotplug();
	pcu_for_each_node_breadth_first(rnp) {
		raw_spin_lock_irqsave_pcu_node(rnp, flags);
		WARN_ON_ONCE(rnp->expmask);
		WRITE_ONCE(rnp->expmask, rnp->expmaskinit);
		raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
	}
}

/*
 * Return non-zero if there is no PCU expedited grace period in progress
 * for the specified pcu_node structure, in other words, if all CPUs and
 * tasks covered by the specified pcu_node structure have done their bit
 * for the current expedited grace period.
 */
static bool sync_pcu_exp_done(struct pcu_node *rnp)
{
	raw_lockdep_assert_held_pcu_node(rnp);
	return READ_ONCE(rnp->exp_tasks) == NULL &&
	       READ_ONCE(rnp->expmask) == 0;
}

/*
 * Like sync_pcu_exp_done(), but where the caller does not hold the
 * pcu_node's ->lock.
 */
static bool sync_pcu_exp_done_unlocked(struct pcu_node *rnp)
{
	unsigned long flags;
	bool ret;

	raw_spin_lock_irqsave_pcu_node(rnp, flags);
	ret = sync_pcu_exp_done(rnp);
	raw_spin_unlock_irqrestore_pcu_node(rnp, flags);

	return ret;
}


/*
 * Report the exit from PCU read-side critical section for the last task
 * that queued itself during or before the current expedited preemptible-PCU
 * grace period.  This event is reported either to the pcu_node structure on
 * which the task was queued or to one of that pcu_node structure's ancestors,
 * recursively up the tree.  (Calm down, calm down, we do the recursion
 * iteratively!)
 */
static void __pcu_report_exp_rnp(struct pcu_node *rnp,
				 bool wake, unsigned long flags)
	__releases(rnp->lock)
{
	unsigned long mask;

	raw_lockdep_assert_held_pcu_node(rnp);
	for (;;) {
		if (!sync_pcu_exp_done(rnp)) {
			if (!rnp->expmask)
				pcu_initiate_boost(rnp, flags);
			else
				raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
			break;
		}
		if (rnp->parent == NULL) {
			raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
			if (wake) {
				smp_mb(); /* EGP done before wake_up(). */
				swake_up_one(&pcu_state.expedited_wq);
			}
			break;
		}
		mask = rnp->grpmask;
		raw_spin_unlock_pcu_node(rnp); /* irqs remain disabled */
		rnp = rnp->parent;
		raw_spin_lock_pcu_node(rnp); /* irqs already disabled */
		WARN_ON_ONCE(!(rnp->expmask & mask));
		WRITE_ONCE(rnp->expmask, rnp->expmask & ~mask);
	}
}

/*
 * Report expedited quiescent state for specified node.  This is a
 * lock-acquisition wrapper function for __pcu_report_exp_rnp().
 */
static void __maybe_unused pcu_report_exp_rnp(struct pcu_node *rnp, bool wake)
{
	unsigned long flags;

	raw_spin_lock_irqsave_pcu_node(rnp, flags);
	__pcu_report_exp_rnp(rnp, wake, flags);
}

/*
 * Report expedited quiescent state for multiple CPUs, all covered by the
 * specified leaf pcu_node structure.
 */
static void pcu_report_exp_cpu_mult(struct pcu_node *rnp,
				    unsigned long mask, bool wake)
{
	int cpu;
	unsigned long flags;
	struct pcu_data *rdp;

	raw_spin_lock_irqsave_pcu_node(rnp, flags);
	if (!(rnp->expmask & mask)) {
		raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
		return;
	}
	WRITE_ONCE(rnp->expmask, rnp->expmask & ~mask);
	for_each_leaf_node_cpu_mask(rnp, cpu, mask) {
		rdp = per_cpu_ptr(&pcu_data, cpu);
		if (!IS_ENABLED(CONFIG_NO_HZ_FULL) || !rdp->pcu_forced_tick_exp)
			continue;
		rdp->pcu_forced_tick_exp = false;
		tick_dep_clear_cpu(cpu, TICK_DEP_BIT_RCU_EXP);
	}
	__pcu_report_exp_rnp(rnp, wake, flags); /* Releases rnp->lock. */
}

/*
 * Report expedited quiescent state for specified pcu_data (CPU).
 */
static void pcu_report_exp_rdp(struct pcu_data *rdp)
{
	WRITE_ONCE(rdp->exp_deferred_qs, false);
	pcu_report_exp_cpu_mult(rdp->mynode, rdp->grpmask, true);
}

/* Common code for work-done checking. */
static bool sync_exp_work_done(unsigned long s)
{
	if (pcu_exp_gp_seq_done(s)) {
		//trace_pcu_exp_grace_period(pcu_state.name, s, TPS("done"));
		smp_mb(); /* Ensure test happens before caller kfree(). */
		return true;
	}
	return false;
}

/*
 * Funnel-lock acquisition for expedited grace periods.  Returns true
 * if some other task completed an expedited grace period that this task
 * can piggy-back on, and with no mutex held.  Otherwise, returns false
 * with the mutex held, indicating that the caller must actually do the
 * expedited grace period.
 */
static bool exp_funnel_lock(unsigned long s)
{
	struct pcu_data *rdp = per_cpu_ptr(&pcu_data, raw_smp_processor_id());
	struct pcu_node *rnp = rdp->mynode;
	struct pcu_node *rnp_root = pcu_get_root();

	/* Low-contention fastpath. */
	if (ULONG_CMP_LT(READ_ONCE(rnp->exp_seq_rq), s) &&
	    (rnp == rnp_root ||
	     ULONG_CMP_LT(READ_ONCE(rnp_root->exp_seq_rq), s)) &&
	    mutex_trylock(&pcu_state.exp_mutex))
		goto fastpath;

	/*
	 * Each pass through the following loop works its way up
	 * the pcu_node tree, returning if others have done the work or
	 * otherwise falls through to acquire ->exp_mutex.  The mapping
	 * from CPU to pcu_node structure can be inexact, as it is just
	 * promoting locality and is not strictly needed for correctness.
	 */
	for (; rnp != NULL; rnp = rnp->parent) {
		if (sync_exp_work_done(s))
			return true;

		/* Work not done, either wait here or go up. */
		spin_lock(&rnp->exp_lock);
		if (ULONG_CMP_GE(rnp->exp_seq_rq, s)) {

			/* Someone else doing GP, so wait for them. */
			spin_unlock(&rnp->exp_lock);
			//trace_pcu_exp_funnel_lock(pcu_state.name, rnp->level,
			//			  rnp->grplo, rnp->grphi,
			//			  TPS("wait"));
			wait_event(rnp->exp_wq[pcu_seq_ctr(s) & 0x3],
				   sync_exp_work_done(s));
			return true;
		}
		WRITE_ONCE(rnp->exp_seq_rq, s); /* Followers can wait on us. */
		spin_unlock(&rnp->exp_lock);
		//trace_pcu_exp_funnel_lock(pcu_state.name, rnp->level,
		//			  rnp->grplo, rnp->grphi, TPS("nxtlvl"));
	}
	mutex_lock(&pcu_state.exp_mutex);
fastpath:
	if (sync_exp_work_done(s)) {
		mutex_unlock(&pcu_state.exp_mutex);
		return true;
	}
	pcu_exp_gp_seq_start();
	//trace_pcu_exp_grace_period(pcu_state.name, s, TPS("start"));
	return false;
}

/*
 * Select the CPUs within the specified pcu_node that the upcoming
 * expedited grace period needs to wait for.
 */
static void sync_pcu_exp_select_node_cpus(struct work_struct *wp)
{
	int cpu;
	unsigned long flags;
	unsigned long mask_ofl_test;
	unsigned long mask_ofl_ipi;
	int ret;
	struct pcu_exp_work *rewp =
		container_of(wp, struct pcu_exp_work, rew_work);
	struct pcu_node *rnp = container_of(rewp, struct pcu_node, rew);

	raw_spin_lock_irqsave_pcu_node(rnp, flags);

	/* Each pass checks a CPU for identity, offline, and idle. */
	mask_ofl_test = 0;
	for_each_leaf_node_cpu_mask(rnp, cpu, rnp->expmask) {
		struct pcu_data *rdp = per_cpu_ptr(&pcu_data, cpu);
		unsigned long mask = rdp->grpmask;
		int snap;

		if (raw_smp_processor_id() == cpu ||
		    !(rnp->qsmaskinitnext & mask)) {
			mask_ofl_test |= mask;
		} else {
			snap = pcu_dynticks_snap(rdp);
			if (pcu_dynticks_in_eqs(snap))
				mask_ofl_test |= mask;
			else
				rdp->exp_dynticks_snap = snap;
		}
	}
	mask_ofl_ipi = rnp->expmask & ~mask_ofl_test;

	/*
	 * Need to wait for any blocked tasks as well.	Note that
	 * additional blocking tasks will also block the expedited GP
	 * until such time as the ->expmask bits are cleared.
	 */
	if (pcu_preempt_has_tasks(rnp))
		WRITE_ONCE(rnp->exp_tasks, rnp->blkd_tasks.next);
	raw_spin_unlock_irqrestore_pcu_node(rnp, flags);

	/* IPI the remaining CPUs for expedited quiescent state. */
	for_each_leaf_node_cpu_mask(rnp, cpu, mask_ofl_ipi) {
		struct pcu_data *rdp = per_cpu_ptr(&pcu_data, cpu);
		unsigned long mask = rdp->grpmask;

retry_ipi:
		if (pcu_dynticks_in_eqs_since(rdp, rdp->exp_dynticks_snap)) {
			mask_ofl_test |= mask;
			continue;
		}
		if (get_cpu() == cpu) {
			mask_ofl_test |= mask;
			put_cpu();
			continue;
		}
		ret = smp_call_function_single(cpu, pcu_exp_handler, NULL, 0);
		put_cpu();
		/* The CPU will report the QS in response to the IPI. */
		if (!ret)
			continue;

		/* Failed, raced with CPU hotplug operation. */
		raw_spin_lock_irqsave_pcu_node(rnp, flags);
		if ((rnp->qsmaskinitnext & mask) &&
		    (rnp->expmask & mask)) {
			/* Online, so delay for a bit and try again. */
			raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
			//trace_pcu_exp_grace_period(pcu_state.name, pcu_exp_gp_seq_endval(), TPS("selectofl"));
			schedule_timeout_idle(1);
			goto retry_ipi;
		}
		/* CPU really is offline, so we must report its QS. */
		if (rnp->expmask & mask)
			mask_ofl_test |= mask;
		raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
	}
	/* Report quiescent states for those that went offline. */
	if (mask_ofl_test)
		pcu_report_exp_cpu_mult(rnp, mask_ofl_test, false);
}

/*
 * Select the nodes that the upcoming expedited grace period needs
 * to wait for.
 */
static void sync_pcu_exp_select_cpus(void)
{
	int cpu;
	struct pcu_node *rnp;

	//trace_pcu_exp_grace_period(pcu_state.name, pcu_exp_gp_seq_endval(), TPS("reset"));
	sync_exp_reset_tree();
	//trace_pcu_exp_grace_period(pcu_state.name, pcu_exp_gp_seq_endval(), TPS("select"));

	/* Schedule work for each leaf pcu_node structure. */
	pcu_for_each_leaf_node(rnp) {
		rnp->exp_need_flush = false;
		if (!READ_ONCE(rnp->expmask))
			continue; /* Avoid early boot non-existent wq. */
		if (!READ_ONCE(pcu_par_gp_wq) ||
		    pcu_scheduler_active != PCU_SCHEDULER_RUNNING ||
		    pcu_is_last_leaf_node(rnp)) {
			/* No workqueues yet or last leaf, do direct call. */
			sync_pcu_exp_select_node_cpus(&rnp->rew.rew_work);
			continue;
		}
		INIT_WORK(&rnp->rew.rew_work, sync_pcu_exp_select_node_cpus);
		cpu = find_next_bit(&rnp->ffmask, BITS_PER_LONG, -1);
		/* If all offline, queue the work on an unbound CPU. */
		if (unlikely(cpu > rnp->grphi - rnp->grplo))
			cpu = WORK_CPU_UNBOUND;
		else
			cpu += rnp->grplo;
		queue_work_on(cpu, pcu_par_gp_wq, &rnp->rew.rew_work);
		rnp->exp_need_flush = true;
	}

	/* Wait for workqueue jobs (if any) to complete. */
	pcu_for_each_leaf_node(rnp)
		if (rnp->exp_need_flush)
			flush_work(&rnp->rew.rew_work);
}

/*
 * Wait for the expedited grace period to elapse, within time limit.
 * If the time limit is exceeded without the grace period elapsing,
 * return false, otherwise return true.
 */
static bool synchronize_pcu_expedited_wait_once(long tlimit)
{
	int t;
	struct pcu_node *rnp_root = pcu_get_root();

	t = swait_event_timeout_exclusive(pcu_state.expedited_wq,
					  sync_pcu_exp_done_unlocked(rnp_root),
					  tlimit);
	// Workqueues should not be signaled.
	if (t > 0 || sync_pcu_exp_done_unlocked(rnp_root))
		return true;
	WARN_ON(t < 0);  /* workqueues should not be signaled. */
	return false;
}

/*
 * Wait for the expedited grace period to elapse, issuing any needed
 * PCU CPU stall warnings along the way.
 */
static void synchronize_pcu_expedited_wait(void)
{
	int cpu;
	unsigned long j;
	unsigned long jiffies_stall;
	unsigned long jiffies_start;
	unsigned long mask;
	int ndetected;
	struct pcu_data *rdp;
	struct pcu_node *rnp;
	struct pcu_node *rnp_root = pcu_get_root();

	//trace_pcu_exp_grace_period(pcu_state.name, pcu_exp_gp_seq_endval(), TPS("startwait"));
	jiffies_stall = pcu_jiffies_till_stall_check();
	jiffies_start = jiffies;
	if (tick_nohz_full_enabled() && pcu_inkernel_boot_has_ended()) {
		if (synchronize_pcu_expedited_wait_once(1))
			return;
		pcu_for_each_leaf_node(rnp) {
			for_each_leaf_node_cpu_mask(rnp, cpu, rnp->expmask) {
				rdp = per_cpu_ptr(&pcu_data, cpu);
				if (rdp->pcu_forced_tick_exp)
					continue;
				rdp->pcu_forced_tick_exp = true;
				tick_dep_set_cpu(cpu, TICK_DEP_BIT_RCU_EXP);
			}
		}
		j = READ_ONCE(jiffies_till_first_fqs);
		if (synchronize_pcu_expedited_wait_once(j + HZ))
			return;
		WARN_ON_ONCE(IS_ENABLED(CONFIG_PREEMPT_RT));
	}

	for (;;) {
		if (synchronize_pcu_expedited_wait_once(jiffies_stall))
			return;
		if (pcu_stall_is_suppressed())
			continue;
		panic_on_pcu_stall();
		//trace_pcu_stall_warning(pcu_state.name, TPS("ExpeditedStall"));
		pr_err("INFO: %s detected expedited stalls on CPUs/tasks: {",
		       pcu_state.name);
		ndetected = 0;
		pcu_for_each_leaf_node(rnp) {
			ndetected += pcu_print_task_exp_stall(rnp);
			for_each_leaf_node_possible_cpu(rnp, cpu) {
				struct pcu_data *rdp;

				mask = leaf_node_cpu_bit(rnp, cpu);
				if (!(READ_ONCE(rnp->expmask) & mask))
					continue;
				ndetected++;
				rdp = per_cpu_ptr(&pcu_data, cpu);
				pr_cont(" %d-%c%c%c", cpu,
					"O."[!!cpu_online(cpu)],
					"o."[!!(rdp->grpmask & rnp->expmaskinit)],
					"N."[!!(rdp->grpmask & rnp->expmaskinitnext)]);
			}
		}
		pr_cont(" } %lu jiffies s: %lu root: %#lx/%c\n",
			jiffies - jiffies_start, pcu_state.expedited_sequence,
			data_race(rnp_root->expmask),
			".T"[!!data_race(rnp_root->exp_tasks)]);
		if (ndetected) {
			pr_err("blocking pcu_node structures (internal PCU debug):");
			pcu_for_each_node_breadth_first(rnp) {
				if (rnp == rnp_root)
					continue; /* printed unconditionally */
				if (sync_pcu_exp_done_unlocked(rnp))
					continue;
				pr_cont(" l=%u:%d-%d:%#lx/%c",
					rnp->level, rnp->grplo, rnp->grphi,
					data_race(rnp->expmask),
					".T"[!!data_race(rnp->exp_tasks)]);
			}
			pr_cont("\n");
		}
		pcu_for_each_leaf_node(rnp) {
			for_each_leaf_node_possible_cpu(rnp, cpu) {
				mask = leaf_node_cpu_bit(rnp, cpu);
				if (!(READ_ONCE(rnp->expmask) & mask))
					continue;
				dump_cpu_task(cpu);
			}
		}
		jiffies_stall = 3 * pcu_jiffies_till_stall_check() + 3;
	}
}

/*
 * Wait for the current expedited grace period to complete, and then
 * wake up everyone who piggybacked on the just-completed expedited
 * grace period.  Also update all the ->exp_seq_rq counters as needed
 * in order to avoid counter-wrap problems.
 */
static void pcu_exp_wait_wake(unsigned long s)
{
	struct pcu_node *rnp;

	synchronize_pcu_expedited_wait();

	// Switch over to wakeup mode, allowing the next GP to proceed.
	// End the previous grace period only after acquiring the mutex
	// to ensure that only one GP runs concurrently with wakeups.
	mutex_lock(&pcu_state.exp_wake_mutex);
	pcu_exp_gp_seq_end();
	//trace_pcu_exp_grace_period(pcu_state.name, s, TPS("end"));

	pcu_for_each_node_breadth_first(rnp) {
		if (ULONG_CMP_LT(READ_ONCE(rnp->exp_seq_rq), s)) {
			spin_lock(&rnp->exp_lock);
			/* Recheck, avoid hang in case someone just arrived. */
			if (ULONG_CMP_LT(rnp->exp_seq_rq, s))
				WRITE_ONCE(rnp->exp_seq_rq, s);
			spin_unlock(&rnp->exp_lock);
		}
		smp_mb(); /* All above changes before wakeup. */
		wake_up_all(&rnp->exp_wq[pcu_seq_ctr(s) & 0x3]);
	}
	//trace_pcu_exp_grace_period(pcu_state.name, s, TPS("endwake"));
	mutex_unlock(&pcu_state.exp_wake_mutex);
}

/*
 * Common code to drive an expedited grace period forward, used by
 * workqueues and mid-boot-time tasks.
 */
static void pcu_exp_sel_wait_wake(unsigned long s)
{
	/* Initialize the pcu_node tree in preparation for the wait. */
	sync_pcu_exp_select_cpus();

	/* Wait and clean up, including waking everyone. */
	pcu_exp_wait_wake(s);
}

/*
 * Work-queue handler to drive an expedited grace period forward.
 */
static void wait_pcu_exp_gp(struct work_struct *wp)
{
	struct pcu_exp_work *rewp;

	rewp = container_of(wp, struct pcu_exp_work, rew_work);
	pcu_exp_sel_wait_wake(rewp->rew_s);
}

#ifdef CONFIG_PREEMPT_PCU

/*
 * Remote handler for smp_call_function_single().  If there is an
 * PCU read-side critical section in effect, request that the
 * next pcu_read_unlock() record the quiescent state up the
 * ->expmask fields in the pcu_node tree.  Otherwise, immediately
 * report the quiescent state.
 */
static void pcu_exp_handler(void *unused)
{
	int depth = pcu_preempt_depth();
	unsigned long flags;
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);
	struct pcu_node *rnp = rdp->mynode;
	struct task_struct *t = current;

	/*
	 * First, the common case of not being in an PCU read-side
	 * critical section.  If also enabled or idle, immediately
	 * report the quiescent state, otherwise defer.
	 */
	if (!depth) {
		if (!(preempt_count() & (PREEMPT_MASK | SOFTIRQ_MASK)) ||
		    pcu_dynticks_curr_cpu_in_eqs()) {
			pcu_report_exp_rdp(rdp);
		} else {
			rdp->exp_deferred_qs = true;
			set_tsk_need_resched(t);
			set_preempt_need_resched();
		}
		return;
	}

	/*
	 * Second, the less-common case of being in an PCU read-side
	 * critical section.  In this case we can count on a future
	 * pcu_read_unlock().  However, this pcu_read_unlock() might
	 * execute on some other CPU, but in that case there will be
	 * a future context switch.  Either way, if the expedited
	 * grace period is still waiting on this CPU, set ->deferred_qs
	 * so that the eventual quiescent state will be reported.
	 * Note that there is a large group of race conditions that
	 * can have caused this quiescent state to already have been
	 * reported, so we really do need to check ->expmask.
	 */
	if (depth > 0) {
		raw_spin_lock_irqsave_pcu_node(rnp, flags);
		if (rnp->expmask & rdp->grpmask) {
			rdp->exp_deferred_qs = true;
			t->pcu_read_unlock_special.b.exp_hint = true;
		}
		raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
		return;
	}

	// Finally, negative nesting depth should not happen.
	WARN_ON_ONCE(1);
}

/* PREEMPTION=y, so no PREEMPTION=n expedited grace period to clean up after. */
static void sync_sched_exp_online_cleanup(int cpu)
{
}

/*
 * Scan the current list of tasks blocked within PCU read-side critical
 * sections, printing out the tid of each that is blocking the current
 * expedited grace period.
 */
static int pcu_print_task_exp_stall(struct pcu_node *rnp)
{
	unsigned long flags;
	int ndetected = 0;
	struct task_struct *t;

	if (!READ_ONCE(rnp->exp_tasks))
		return 0;
	raw_spin_lock_irqsave_pcu_node(rnp, flags);
	t = list_entry(rnp->exp_tasks->prev,
		       struct task_struct, pcu_node_entry);
	list_for_each_entry_continue(t, &rnp->blkd_tasks, pcu_node_entry) {
		pr_cont(" P%d", t->pid);
		ndetected++;
	}
	raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
	return ndetected;
}

#else /* #ifdef CONFIG_PREEMPT_PCU */

/* Request an expedited quiescent state. */
static void pcu_exp_need_qs(void)
{
	__this_cpu_write(pcu_data.cpu_no_qs.b.exp, true);
	/* Store .exp before .pcu_urgent_qs. */
	smp_store_release(this_cpu_ptr(&pcu_data.pcu_urgent_qs), true);
	set_tsk_need_resched(current);
	set_preempt_need_resched();
}

/* Invoked on each online non-idle CPU for expedited quiescent state. */
static void pcu_exp_handler(void *unused)
{
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);
	struct pcu_node *rnp = rdp->mynode;

	if (!(READ_ONCE(rnp->expmask) & rdp->grpmask) ||
	    __this_cpu_read(pcu_data.cpu_no_qs.b.exp))
		return;
	if (pcu_is_cpu_rrupt_from_idle()) {
		pcu_report_exp_rdp(this_cpu_ptr(&pcu_data));
		return;
	}
	pcu_exp_need_qs();
}

/* Send IPI for expedited cleanup if needed at end of CPU-hotplug operation. */
static void sync_sched_exp_online_cleanup(int cpu)
{
	unsigned long flags;
	int my_cpu;
	struct pcu_data *rdp;
	int ret;
	struct pcu_node *rnp;

	rdp = per_cpu_ptr(&pcu_data, cpu);
	rnp = rdp->mynode;
	my_cpu = get_cpu();
	/* Quiescent state either not needed or already requested, leave. */
	if (!(READ_ONCE(rnp->expmask) & rdp->grpmask) ||
	    rdp->cpu_no_qs.b.exp) {
		put_cpu();
		return;
	}
	/* Quiescent state needed on current CPU, so set it up locally. */
	if (my_cpu == cpu) {
		local_irq_save(flags);
		pcu_exp_need_qs();
		local_irq_restore(flags);
		put_cpu();
		return;
	}
	/* Quiescent state needed on some other CPU, send IPI. */
	ret = smp_call_function_single(cpu, pcu_exp_handler, NULL, 0);
	put_cpu();
	WARN_ON_ONCE(ret);
}

/*
 * Because preemptible PCU does not exist, we never have to check for
 * tasks blocked within PCU read-side critical sections that are
 * blocking the current expedited grace period.
 */
static int pcu_print_task_exp_stall(struct pcu_node *rnp)
{
	return 0;
}

#endif /* #else #ifdef CONFIG_PREEMPT_PCU */

/**
 * synchronize_pcu_expedited - Brute-force PCU grace period
 *
 * Wait for an PCU grace period, but expedite it.  The basic idea is to
 * IPI all non-idle non-nohz online CPUs.  The IPI handler checks whether
 * the CPU is in an PCU critical section, and if so, it sets a flag that
 * causes the outermost pcu_read_unlock() to report the quiescent state
 * for PCU-preempt or asks the scheduler for help for PCU-sched.  On the
 * other hand, if the CPU is not in an PCU read-side critical section,
 * the IPI handler reports the quiescent state immediately.
 *
 * Although this is a greate improvement over previous expedited
 * implementations, it is still unfriendly to real-time workloads, so is
 * thus not recommended for any sort of common-case code.  In fact, if
 * you are using synchronize_pcu_expedited() in a loop, please restructure
 * your code to batch your updates, and then use a single synchronize_pcu()
 * instead.
 *
 * This has the same semantics as (but is more brutal than) synchronize_pcu().
 */
void synchronize_pcu_expedited(void)
{
	bool boottime = (pcu_scheduler_active == PCU_SCHEDULER_INIT);
	struct pcu_exp_work rew;
	struct pcu_node *rnp;
	unsigned long s;

	PCU_LOCKDEP_WARN(lock_is_held(&pcu_bh_lock_map) ||
			 lock_is_held(&pcu_lock_map) ||
			 lock_is_held(&pcu_sched_lock_map),
			 "Illegal synchronize_pcu_expedited() in PCU read-side critical section");

	/* Is the state is such that the call is a grace period? */
	if (pcu_blocking_is_gp())
		return;

	/* If expedited grace periods are prohibited, fall back to normal. */
	if (pcu_gp_is_normal()) {
		wait_pcu_gp(call_pcu);
		return;
	}

	/* Take a snapshot of the sequence number.  */
	s = pcu_exp_gp_seq_snap();
	if (exp_funnel_lock(s))
		return;  /* Someone else did our work for us. */

	/* Ensure that load happens before action based on it. */
	if (unlikely(boottime)) {
		/* Direct call during scheduler init and early_initcalls(). */
		pcu_exp_sel_wait_wake(s);
	} else {
		/* Marshall arguments & schedule the expedited grace period. */
		rew.rew_s = s;
		INIT_WORK_ONSTACK(&rew.rew_work, wait_pcu_exp_gp);
		queue_work(pcu_gp_wq, &rew.rew_work);
	}

	/* Wait for expedited grace period to complete. */
	rnp = pcu_get_root();
	wait_event(rnp->exp_wq[pcu_seq_ctr(s) & 0x3],
		   sync_exp_work_done(s));
	smp_mb(); /* Workqueue actions happen before return. */

	/* Let the next expedited grace period start. */
	mutex_unlock(&pcu_state.exp_mutex);

	if (likely(!boottime))
		destroy_work_on_stack(&rew.rew_work);
}
EXPORT_SYMBOL_GPL(synchronize_pcu_expedited);
