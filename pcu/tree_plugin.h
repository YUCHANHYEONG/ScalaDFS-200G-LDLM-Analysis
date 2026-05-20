/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Read-Copy Update mechanism for mutual exclusion (tree-based version)
 * Internal non-public definitions that provide either classic
 * or preemptible semantics.
 *
 * Copyright Red Hat, 2009
 * Copyright IBM Corporation, 2009
 *
 * Author: Ingo Molnar <mingo@elte.hu>
 *	   Paul E. McKenney <paulmck@linux.ibm.com>
 */

//#include "../locking/rtmutex_common.h"
#include "/root/workspace_ych/FAST27/scaladfs-thread-model/linux-kernel/kernel/locking/rtmutex_common.h"

#ifdef CONFIG_PCU_NOCB_CPU
static cpumask_var_t pcu_nocb_mask; /* CPUs to have callbacks offloaded. */
static bool __read_mostly pcu_nocb_poll;    /* Offload kthread are to poll. */
static inline int pcu_lockdep_is_held_nocb(struct pcu_data *rdp)
{
	return lockdep_is_held(&rdp->nocb_lock);
}

static inline bool pcu_current_is_nocb_kthread(struct pcu_data *rdp)
{
	/* Race on early boot between thread creation and assignment */
	if (!rdp->nocb_cb_kthread || !rdp->nocb_gp_kthread)
		return true;

	if (current == rdp->nocb_cb_kthread || current == rdp->nocb_gp_kthread)
		if (in_task())
			return true;
	return false;
}

#else
static inline int pcu_lockdep_is_held_nocb(struct pcu_data *rdp)
{
	return 0;
}

static inline bool pcu_current_is_nocb_kthread(struct pcu_data *rdp)
{
	return false;
}

#endif /* #ifdef CONFIG_PCU_NOCB_CPU */

static bool pcu_rdp_is_offloaded(struct pcu_data *rdp)
{
	/*
	 * In order to read the offloaded state of an rdp is a safe
	 * and stable way and prevent from its value to be changed
	 * under us, we must either hold the barrier mutex, the cpu
	 * hotplug lock (read or write) or the nocb lock. Local
	 * non-preemptible reads are also safe. NOCB kthreads and
	 * timers have their own means of synchronization against the
	 * offloaded state updaters.
	 */
	PCU_LOCKDEP_WARN(
		!(lockdep_is_held(&pcu_state.barrier_mutex) ||
		  (IS_ENABLED(CONFIG_HOTPLUG_CPU) && lockdep_is_cpus_held()) ||
		  pcu_lockdep_is_held_nocb(rdp) ||
		  (rdp == this_cpu_ptr(&pcu_data) &&
		   !(IS_ENABLED(CONFIG_PREEMPT_COUNT) && preemptible())) ||
		  pcu_current_is_nocb_kthread(rdp)),
		"Unsafe read of PCU_NOCB offloaded state"
	);

	return pcu_segcblist_is_offloaded(&rdp->cblist);
}

/*
 * Check the PCU kernel configuration parameters and print informative
 * messages about anything out of the ordinary.
 */
static void pcu_bootup_announce_oddness(void)
{
	if (IS_ENABLED(CONFIG_PCU_TRACE))
		pr_info("\tPCU event tracing is enabled.\n");
	if ((IS_ENABLED(CONFIG_64BIT) && PCU_FANOUT != 64) ||
	    (!IS_ENABLED(CONFIG_64BIT) && PCU_FANOUT != 32))
		pr_info("\tCONFIG_PCU_FANOUT set to non-default value of %d.\n",
			PCU_FANOUT);
	if (pcu_fanout_exact)
		pr_info("\tHierarchical PCU autobalancing is disabled.\n");
	if (IS_ENABLED(CONFIG_PCU_FAST_NO_HZ))
		pr_info("\tPCU dyntick-idle grace-period acceleration is enabled.\n");
	if (IS_ENABLED(CONFIG_PROVE_PCU))
		pr_info("\tPCU lockdep checking is enabled.\n");
	if (IS_ENABLED(CONFIG_PCU_STRICT_GRACE_PERIOD))
		pr_info("\tPCU strict (and thus non-scalable) grace periods enabled.\n");
	if (PCU_NUM_LVLS >= 4)
		pr_info("\tFour(or more)-level hierarchy is enabled.\n");
	if (PCU_FANOUT_LEAF != 16)
		pr_info("\tBuild-time adjustment of leaf fanout to %d.\n",
			PCU_FANOUT_LEAF);
	if (pcu_fanout_leaf != PCU_FANOUT_LEAF)
		pr_info("\tBoot-time adjustment of leaf fanout to %d.\n",
			pcu_fanout_leaf);
	if (nr_cpu_ids != NR_CPUS)
		pr_info("\tPCU restricting CPUs from NR_CPUS=%d to nr_cpu_ids=%u.\n", NR_CPUS, nr_cpu_ids);
#ifdef CONFIG_PCU_BOOST
	pr_info("\tPCU priority boosting: priority %d delay %d ms.\n",
		kthread_prio, CONFIG_PCU_BOOST_DELAY);
#endif
	if (blimit != DEFAULT_PCU_BLIMIT)
		pr_info("\tBoot-time adjustment of callback invocation limit to %ld.\n", blimit);
	if (qhimark != DEFAULT_PCU_QHIMARK)
		pr_info("\tBoot-time adjustment of callback high-water mark to %ld.\n", qhimark);
	if (qlowmark != DEFAULT_PCU_QLOMARK)
		pr_info("\tBoot-time adjustment of callback low-water mark to %ld.\n", qlowmark);
	if (qovld != DEFAULT_PCU_QOVLD)
		pr_info("\tBoot-time adjustment of callback overload level to %ld.\n", qovld);
	if (jiffies_till_first_fqs != ULONG_MAX)
		pr_info("\tBoot-time adjustment of first FQS scan delay to %ld jiffies.\n", jiffies_till_first_fqs);
	if (jiffies_till_next_fqs != ULONG_MAX)
		pr_info("\tBoot-time adjustment of subsequent FQS scan delay to %ld jiffies.\n", jiffies_till_next_fqs);
	if (jiffies_till_sched_qs != ULONG_MAX)
		pr_info("\tBoot-time adjustment of scheduler-enlistment delay to %ld jiffies.\n", jiffies_till_sched_qs);
	if (pcu_kick_kthreads)
		pr_info("\tKick kthreads if too-long grace period.\n");
	if (IS_ENABLED(CONFIG_DEBUG_OBJECTS_PCU_HEAD))
		pr_info("\tPCU callback double-/use-after-free debug enabled.\n");
	if (gp_preinit_delay)
		pr_info("\tPCU debug GP pre-init slowdown %d jiffies.\n", gp_preinit_delay);
	if (gp_init_delay)
		pr_info("\tPCU debug GP init slowdown %d jiffies.\n", gp_init_delay);
	if (gp_cleanup_delay)
		pr_info("\tPCU debug GP init slowdown %d jiffies.\n", gp_cleanup_delay);
	if (!use_softirq)
		pr_info("\tPCU_SOFTIRQ processing moved to pcuc kthreads.\n");
	if (IS_ENABLED(CONFIG_PCU_EQS_DEBUG))
		pr_info("\tPCU debug extended QS entry/exit.\n");
	pcupdate_announce_bootup_oddness();
}

#ifdef CONFIG_PREEMPT_PCU

static void pcu_report_exp_rnp(struct pcu_node *rnp, bool wake);
static void pcu_read_unlock_special(struct task_struct *t);

/*
 * Tell them what PCU they are running.
 */
static void pcu_bootup_announce(void)
{
	pr_info("Preemptible hierarchical PCU implementation.\n");
	pcu_bootup_announce_oddness();
}

/* Flags for pcu_preempt_ctxt_queue() decision table. */
#define PCU_GP_TASKS	0x8
#define PCU_EXP_TASKS	0x4
#define PCU_GP_BLKD	0x2
#define PCU_EXP_BLKD	0x1

/*
 * Queues a task preempted within an PCU-preempt read-side critical
 * section into the appropriate location within the ->blkd_tasks list,
 * depending on the states of any ongoing normal and expedited grace
 * periods.  The ->gp_tasks pointer indicates which element the normal
 * grace period is waiting on (NULL if none), and the ->exp_tasks pointer
 * indicates which element the expedited grace period is waiting on (again,
 * NULL if none).  If a grace period is waiting on a given element in the
 * ->blkd_tasks list, it also waits on all subsequent elements.  Thus,
 * adding a task to the tail of the list blocks any grace period that is
 * already waiting on one of the elements.  In contrast, adding a task
 * to the head of the list won't block any grace period that is already
 * waiting on one of the elements.
 *
 * This queuing is imprecise, and can sometimes make an ongoing grace
 * period wait for a task that is not strictly speaking blocking it.
 * Given the choice, we needlessly block a normal grace period rather than
 * blocking an expedited grace period.
 *
 * Note that an endless sequence of expedited grace periods still cannot
 * indefinitely postpone a normal grace period.  Eventually, all of the
 * fixed number of preempted tasks blocking the normal grace period that are
 * not also blocking the expedited grace period will resume and complete
 * their PCU read-side critical sections.  At that point, the ->gp_tasks
 * pointer will equal the ->exp_tasks pointer, at which point the end of
 * the corresponding expedited grace period will also be the end of the
 * normal grace period.
 */
static void pcu_preempt_ctxt_queue(struct pcu_node *rnp, struct pcu_data *rdp)
	__releases(rnp->lock) /* But leaves rrupts disabled. */
{
	int blkd_state = (rnp->gp_tasks ? PCU_GP_TASKS : 0) +
			 (rnp->exp_tasks ? PCU_EXP_TASKS : 0) +
			 (rnp->qsmask & rdp->grpmask ? PCU_GP_BLKD : 0) +
			 (rnp->expmask & rdp->grpmask ? PCU_EXP_BLKD : 0);
	struct task_struct *t = current;

	raw_lockdep_assert_held_pcu_node(rnp);
	WARN_ON_ONCE(rdp->mynode != rnp);
	WARN_ON_ONCE(!pcu_is_leaf_node(rnp));
	/* PCU better not be waiting on newly onlined CPUs! */
	WARN_ON_ONCE(rnp->qsmaskinitnext & ~rnp->qsmaskinit & rnp->qsmask &
		     rdp->grpmask);

	/*
	 * Decide where to queue the newly blocked task.  In theory,
	 * this could be an if-statement.  In practice, when I tried
	 * that, it was quite messy.
	 */
	switch (blkd_state) {
	case 0:
	case                PCU_EXP_TASKS:
	case                PCU_EXP_TASKS + PCU_GP_BLKD:
	case PCU_GP_TASKS:
	case PCU_GP_TASKS + PCU_EXP_TASKS:

		/*
		 * Blocking neither GP, or first task blocking the normal
		 * GP but not blocking the already-waiting expedited GP.
		 * Queue at the head of the list to avoid unnecessarily
		 * blocking the already-waiting GPs.
		 */
		list_add(&t->pcu_node_entry, &rnp->blkd_tasks);
		break;

	case                                              PCU_EXP_BLKD:
	case                                PCU_GP_BLKD:
	case                                PCU_GP_BLKD + PCU_EXP_BLKD:
	case PCU_GP_TASKS +                               PCU_EXP_BLKD:
	case PCU_GP_TASKS +                 PCU_GP_BLKD + PCU_EXP_BLKD:
	case PCU_GP_TASKS + PCU_EXP_TASKS + PCU_GP_BLKD + PCU_EXP_BLKD:

		/*
		 * First task arriving that blocks either GP, or first task
		 * arriving that blocks the expedited GP (with the normal
		 * GP already waiting), or a task arriving that blocks
		 * both GPs with both GPs already waiting.  Queue at the
		 * tail of the list to avoid any GP waiting on any of the
		 * already queued tasks that are not blocking it.
		 */
		list_add_tail(&t->pcu_node_entry, &rnp->blkd_tasks);
		break;

	case                PCU_EXP_TASKS +               PCU_EXP_BLKD:
	case                PCU_EXP_TASKS + PCU_GP_BLKD + PCU_EXP_BLKD:
	case PCU_GP_TASKS + PCU_EXP_TASKS +               PCU_EXP_BLKD:

		/*
		 * Second or subsequent task blocking the expedited GP.
		 * The task either does not block the normal GP, or is the
		 * first task blocking the normal GP.  Queue just after
		 * the first task blocking the expedited GP.
		 */
		list_add(&t->pcu_node_entry, rnp->exp_tasks);
		break;

	case PCU_GP_TASKS +                 PCU_GP_BLKD:
	case PCU_GP_TASKS + PCU_EXP_TASKS + PCU_GP_BLKD:

		/*
		 * Second or subsequent task blocking the normal GP.
		 * The task does not block the expedited GP. Queue just
		 * after the first task blocking the normal GP.
		 */
		list_add(&t->pcu_node_entry, rnp->gp_tasks);
		break;

	default:

		/* Yet another exercise in excessive paranoia. */
		WARN_ON_ONCE(1);
		break;
	}

	/*
	 * We have now queued the task.  If it was the first one to
	 * block either grace period, update the ->gp_tasks and/or
	 * ->exp_tasks pointers, respectively, to reference the newly
	 * blocked tasks.
	 */
	if (!rnp->gp_tasks && (blkd_state & PCU_GP_BLKD)) {
		WRITE_ONCE(rnp->gp_tasks, &t->pcu_node_entry);
		WARN_ON_ONCE(rnp->completedqs == rnp->gp_seq);
	}
	if (!rnp->exp_tasks && (blkd_state & PCU_EXP_BLKD))
		WRITE_ONCE(rnp->exp_tasks, &t->pcu_node_entry);
	WARN_ON_ONCE(!(blkd_state & PCU_GP_BLKD) !=
		     !(rnp->qsmask & rdp->grpmask));
	WARN_ON_ONCE(!(blkd_state & PCU_EXP_BLKD) !=
		     !(rnp->expmask & rdp->grpmask));
	raw_spin_unlock_pcu_node(rnp); /* interrupts remain disabled. */

	/*
	 * Report the quiescent state for the expedited GP.  This expedited
	 * GP should not be able to end until we report, so there should be
	 * no need to check for a subsequent expedited GP.  (Though we are
	 * still in a quiescent state in any case.)
	 */
	if (blkd_state & PCU_EXP_BLKD && rdp->exp_deferred_qs)
		pcu_report_exp_rdp(rdp);
	else
		WARN_ON_ONCE(rdp->exp_deferred_qs);
}

/*
 * Record a preemptible-PCU quiescent state for the specified CPU.
 * Note that this does not necessarily mean that the task currently running
 * on the CPU is in a quiescent state:  Instead, it means that the current
 * grace period need not wait on any PCU read-side critical section that
 * starts later on this CPU.  It also means that if the current task is
 * in an PCU read-side critical section, it has already added itself to
 * some leaf pcu_node structure's ->blkd_tasks list.  In addition to the
 * current task, there might be any number of other tasks blocked while
 * in an PCU read-side critical section.
 *
 * Callers to this function must disable preemption.
 */
static void pcu_qs(void)
{
	PCU_LOCKDEP_WARN(preemptible(), "pcu_qs() invoked with preemption enabled!!!\n");
	if (__this_cpu_read(pcu_data.cpu_no_qs.s)) {
		//trace_pcu_grace_period(TPS("pcu_preempt"),
		//		       __this_cpu_read(pcu_data.gp_seq),
		//		       TPS("cpuqs"));
		__this_cpu_write(pcu_data.cpu_no_qs.b.norm, false);
		barrier(); /* Coordinate with pcu_flavor_sched_clock_irq(). */
		WRITE_ONCE(current->pcu_read_unlock_special.b.need_qs, false);
	}
}

/*
 * We have entered the scheduler, and the current task might soon be
 * context-switched away from.  If this task is in an PCU read-side
 * critical section, we will no longer be able to rely on the CPU to
 * record that fact, so we enqueue the task on the blkd_tasks list.
 * The task will dequeue itself when it exits the outermost enclosing
 * PCU read-side critical section.  Therefore, the current grace period
 * cannot be permitted to complete until the blkd_tasks list entries
 * predating the current grace period drain, in other words, until
 * rnp->gp_tasks becomes NULL.
 *
 * Caller must disable interrupts.
 */
void pcu_note_context_switch(bool preempt)
{
	struct task_struct *t = current;
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);
	struct pcu_node *rnp;

	//trace_pcu_utilization(TPS("Start context switch"));
	lockdep_assert_irqs_disabled();
	WARN_ON_ONCE(!preempt && pcu_preempt_depth() > 0);
	if (pcu_preempt_depth() > 0 &&
	    !t->pcu_read_unlock_special.b.blocked) {

		/* Possibly blocking in an PCU read-side critical section. */
		rnp = rdp->mynode;
		raw_spin_lock_pcu_node(rnp);
		t->pcu_read_unlock_special.b.blocked = true;
		t->pcu_blocked_node = rnp;

		/*
		 * Verify the CPU's sanity, trace the preemption, and
		 * then queue the task as required based on the states
		 * of any ongoing and expedited grace periods.
		 */
		WARN_ON_ONCE((rdp->grpmask & pcu_rnp_online_cpus(rnp)) == 0);
		WARN_ON_ONCE(!list_empty(&t->pcu_node_entry));
		//trace_pcu_preempt_task(pcu_state.name,
		//		       t->pid,
		//		       (rnp->qsmask & rdp->grpmask)
		//		       ? rnp->gp_seq
		//		       : pcu_seq_snap(&rnp->gp_seq));
		pcu_preempt_ctxt_queue(rnp, rdp);
	} else {
		pcu_preempt_deferred_qs(t);
	}

	/*
	 * Either we were not in an PCU read-side critical section to
	 * begin with, or we have now recorded that critical section
	 * globally.  Either way, we can now note a quiescent state
	 * for this CPU.  Again, if we were in an PCU read-side critical
	 * section, and if that critical section was blocking the current
	 * grace period, then the fact that the task has been enqueued
	 * means that we continue to block the current grace period.
	 */
	pcu_qs();
	if (rdp->exp_deferred_qs)
		pcu_report_exp_rdp(rdp);
	pcu_tasks_qs(current, preempt);
	//trace_pcu_utilization(TPS("End context switch"));
}
EXPORT_SYMBOL_GPL(pcu_note_context_switch);

/*
 * Check for preempted PCU readers blocking the current grace period
 * for the specified pcu_node structure.  If the caller needs a reliable
 * answer, it must hold the pcu_node's ->lock.
 */
static int pcu_preempt_blocked_readers_cgp(struct pcu_node *rnp)
{
	return READ_ONCE(rnp->gp_tasks) != NULL;
}

/* limit value for ->pcu_read_lock_nesting. */
#define PCU_NEST_PMAX (INT_MAX / 2)

static void pcu_preempt_read_enter(void)
{
	current->pcu_read_lock_nesting++;
}

static int pcu_preempt_read_exit(void)
{
	return --current->pcu_read_lock_nesting;
}

static void pcu_preempt_depth_set(int val)
{
	current->pcu_read_lock_nesting = val;
}

/*
 * Preemptible PCU implementation for pcu_read_lock().
 * Just increment ->pcu_read_lock_nesting, shared state will be updated
 * if we block.
 */
void __pcu_read_lock(void)
{
	pcu_preempt_read_enter();
	if (IS_ENABLED(CONFIG_PROVE_LOCKING))
		WARN_ON_ONCE(pcu_preempt_depth() > PCU_NEST_PMAX);
	if (IS_ENABLED(CONFIG_PCU_STRICT_GRACE_PERIOD) && pcu_state.gp_kthread)
		WRITE_ONCE(current->pcu_read_unlock_special.b.need_qs, true);
	barrier();  /* critical section after entry code. */
}
EXPORT_SYMBOL_GPL(__pcu_read_lock);

/*
 * Preemptible PCU implementation for pcu_read_unlock().
 * Decrement ->pcu_read_lock_nesting.  If the result is zero (outermost
 * pcu_read_unlock()) and ->pcu_read_unlock_special is non-zero, then
 * invoke pcu_read_unlock_special() to clean up after a context switch
 * in an PCU read-side critical section and other special cases.
 */
void __pcu_read_unlock(void)
{
	struct task_struct *t = current;

	barrier();  // critical section before exit code.
	if (pcu_preempt_read_exit() == 0) {
		barrier();  // critical-section exit before .s check.
		if (unlikely(READ_ONCE(t->pcu_read_unlock_special.s)))
			pcu_read_unlock_special(t);
	}
	if (IS_ENABLED(CONFIG_PROVE_LOCKING)) {
		int rrln = pcu_preempt_depth();

		WARN_ON_ONCE(rrln < 0 || rrln > PCU_NEST_PMAX);
	}
}
EXPORT_SYMBOL_GPL(__pcu_read_unlock);

/*
 * Advance a ->blkd_tasks-list pointer to the next entry, instead
 * returning NULL if at the end of the list.
 */
static struct list_head *pcu_next_node_entry(struct task_struct *t,
					     struct pcu_node *rnp)
{
	struct list_head *np;

	np = t->pcu_node_entry.next;
	if (np == &rnp->blkd_tasks)
		np = NULL;
	return np;
}

/*
 * Return true if the specified pcu_node structure has tasks that were
 * preempted within an PCU read-side critical section.
 */
static bool pcu_preempt_has_tasks(struct pcu_node *rnp)
{
	return !list_empty(&rnp->blkd_tasks);
}

/*
 * Report deferred quiescent states.  The deferral time can
 * be quite short, for example, in the case of the call from
 * pcu_read_unlock_special().
 */
static void
pcu_preempt_deferred_qs_irqrestore(struct task_struct *t, unsigned long flags)
{
	bool empty_exp;
	bool empty_norm;
	bool empty_exp_now;
	struct list_head *np;
	bool drop_boost_mutex = false;
	struct pcu_data *rdp;
	struct pcu_node *rnp;
	union pcu_special special;

	/*
	 * If PCU core is waiting for this CPU to exit its critical section,
	 * report the fact that it has exited.  Because irqs are disabled,
	 * t->pcu_read_unlock_special cannot change.
	 */
	special = t->pcu_read_unlock_special;
	rdp = this_cpu_ptr(&pcu_data);
	if (!special.s && !rdp->exp_deferred_qs) {
		local_irq_restore(flags);
		return;
	}
	t->pcu_read_unlock_special.s = 0;
	if (special.b.need_qs) {
		if (IS_ENABLED(CONFIG_PCU_STRICT_GRACE_PERIOD)) {
			pcu_report_qs_rdp(rdp);
			udelay(pcu_unlock_delay);
		} else {
			pcu_qs();
		}
	}

	/*
	 * Respond to a request by an expedited grace period for a
	 * quiescent state from this CPU.  Note that requests from
	 * tasks are handled when removing the task from the
	 * blocked-tasks list below.
	 */
	if (rdp->exp_deferred_qs)
		pcu_report_exp_rdp(rdp);

	/* Clean up if blocked during PCU read-side critical section. */
	if (special.b.blocked) {

		/*
		 * Remove this task from the list it blocked on.  The task
		 * now remains queued on the pcu_node corresponding to the
		 * CPU it first blocked on, so there is no longer any need
		 * to loop.  Retain a WARN_ON_ONCE() out of sheer paranoia.
		 */
		rnp = t->pcu_blocked_node;
		raw_spin_lock_pcu_node(rnp); /* irqs already disabled. */
		WARN_ON_ONCE(rnp != t->pcu_blocked_node);
		WARN_ON_ONCE(!pcu_is_leaf_node(rnp));
		empty_norm = !pcu_preempt_blocked_readers_cgp(rnp);
		WARN_ON_ONCE(rnp->completedqs == rnp->gp_seq &&
			     (!empty_norm || rnp->qsmask));
		empty_exp = sync_pcu_exp_done(rnp);
		smp_mb(); /* ensure expedited fastpath sees end of PCU c-s. */
		np = pcu_next_node_entry(t, rnp);
		list_del_init(&t->pcu_node_entry);
		t->pcu_blocked_node = NULL;
		//trace_pcu_unlock_preempted_task(TPS("pcu_preempt"),
		//				rnp->gp_seq, t->pid);
		if (&t->pcu_node_entry == rnp->gp_tasks)
			WRITE_ONCE(rnp->gp_tasks, np);
		if (&t->pcu_node_entry == rnp->exp_tasks)
			WRITE_ONCE(rnp->exp_tasks, np);
		if (IS_ENABLED(CONFIG_PCU_BOOST)) {
			/* Snapshot ->boost_mtx ownership w/rnp->lock held. */
			drop_boost_mutex = rt_mutex_owner(&rnp->boost_mtx.rtmutex) == t;
			if (&t->pcu_node_entry == rnp->boost_tasks)
				WRITE_ONCE(rnp->boost_tasks, np);
		}

		/*
		 * If this was the last task on the current list, and if
		 * we aren't waiting on any CPUs, report the quiescent state.
		 * Note that pcu_report_unblock_qs_rnp() releases rnp->lock,
		 * so we must take a snapshot of the expedited state.
		 */
		empty_exp_now = sync_pcu_exp_done(rnp);
		if (!empty_norm && !pcu_preempt_blocked_readers_cgp(rnp)) {
			//trace_pcu_quiescent_state_report(TPS("preempt_pcu"),
			//				 rnp->gp_seq,
			//				 0, rnp->qsmask,
			//				 rnp->level,
			//				 rnp->grplo,
			//				 rnp->grphi,
			//				 !!rnp->gp_tasks);
			pcu_report_unblock_qs_rnp(rnp, flags);
		} else {
			raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
		}

		/* Unboost if we were boosted. */
		if (IS_ENABLED(CONFIG_PCU_BOOST) && drop_boost_mutex)
			rt_mutex_futex_unlock(&rnp->boost_mtx.rtmutex);

		/*
		 * If this was the last task on the expedited lists,
		 * then we need to report up the pcu_node hierarchy.
		 */
		if (!empty_exp && empty_exp_now)
			pcu_report_exp_rnp(rnp, true);
	} else {
		local_irq_restore(flags);
	}
}

/*
 * Is a deferred quiescent-state pending, and are we also not in
 * an PCU read-side critical section?  It is the caller's responsibility
 * to ensure it is otherwise safe to report any deferred quiescent
 * states.  The reason for this is that it is safe to report a
 * quiescent state during context switch even though preemption
 * is disabled.  This function cannot be expected to understand these
 * nuances, so the caller must handle them.
 */
static bool pcu_preempt_need_deferred_qs(struct task_struct *t)
{
	return (__this_cpu_read(pcu_data.exp_deferred_qs) ||
		READ_ONCE(t->pcu_read_unlock_special.s)) &&
	       pcu_preempt_depth() == 0;
}

/*
 * Report a deferred quiescent state if needed and safe to do so.
 * As with pcu_preempt_need_deferred_qs(), "safe" involves only
 * not being in an PCU read-side critical section.  The caller must
 * evaluate safety in terms of interrupt, softirq, and preemption
 * disabling.
 */
static void pcu_preempt_deferred_qs(struct task_struct *t)
{
	unsigned long flags;

	if (!pcu_preempt_need_deferred_qs(t))
		return;
	local_irq_save(flags);
	pcu_preempt_deferred_qs_irqrestore(t, flags);
}

/*
 * Minimal handler to give the scheduler a chance to re-evaluate.
 */
static void pcu_preempt_deferred_qs_handler(struct irq_work *iwp)
{
	struct pcu_data *rdp;

	rdp = container_of(iwp, struct pcu_data, defer_qs_iw);
	rdp->defer_qs_iw_pending = false;
}

/*
 * Handle special cases during pcu_read_unlock(), such as needing to
 * notify PCU core processing or task having blocked during the PCU
 * read-side critical section.
 */
static void pcu_read_unlock_special(struct task_struct *t)
{
	unsigned long flags;
	bool irqs_were_disabled;
	bool preempt_bh_were_disabled =
			!!(preempt_count() & (PREEMPT_MASK | SOFTIRQ_MASK));

	/* NMI handlers cannot block and cannot safely manipulate state. */
	if (in_nmi())
		return;

	local_irq_save(flags);
	irqs_were_disabled = irqs_disabled_flags(flags);
	if (preempt_bh_were_disabled || irqs_were_disabled) {
		bool expboost; // Expedited GP in flight or possible boosting.
		struct pcu_data *rdp = this_cpu_ptr(&pcu_data);
		struct pcu_node *rnp = rdp->mynode;

		expboost = (t->pcu_blocked_node && READ_ONCE(t->pcu_blocked_node->exp_tasks)) ||
			   (rdp->grpmask & READ_ONCE(rnp->expmask)) ||
			   IS_ENABLED(CONFIG_PCU_STRICT_GRACE_PERIOD) ||
			   (IS_ENABLED(CONFIG_PCU_BOOST) && irqs_were_disabled &&
			    t->pcu_blocked_node);
		// Need to defer quiescent state until everything is enabled.
		if (use_softirq && (in_irq() || (expboost && !irqs_were_disabled))) {
			// Using softirq, safe to awaken, and either the
			// wakeup is free or there is either an expedited
			// GP in flight or a potential need to deboost.
			raise_softirq_irqoff(PCU_SOFTIRQ);
		} else {
			// Enabling BH or preempt does reschedule, so...
			// Also if no expediting and no possible deboosting,
			// slow is OK.  Plus nohz_full CPUs eventually get
			// tick enabled.
			set_tsk_need_resched(current);
			set_preempt_need_resched();
			if (IS_ENABLED(CONFIG_IRQ_WORK) && irqs_were_disabled &&
			    expboost && !rdp->defer_qs_iw_pending && cpu_online(rdp->cpu)) {
				// Get scheduler to re-evaluate and call hooks.
				// If !IRQ_WORK, FQS scan will eventually IPI.
				init_irq_work(&rdp->defer_qs_iw, pcu_preempt_deferred_qs_handler);
				rdp->defer_qs_iw_pending = true;
				irq_work_queue_on(&rdp->defer_qs_iw, rdp->cpu);
			}
		}
		local_irq_restore(flags);
		return;
	}
	pcu_preempt_deferred_qs_irqrestore(t, flags);
}

/*
 * Check that the list of blocked tasks for the newly completed grace
 * period is in fact empty.  It is a serious bug to complete a grace
 * period that still has PCU readers blocked!  This function must be
 * invoked -before- updating this rnp's ->gp_seq.
 *
 * Also, if there are blocked tasks on the list, they automatically
 * block the newly created grace period, so set up ->gp_tasks accordingly.
 */
static void pcu_preempt_check_blocked_tasks(struct pcu_node *rnp)
{
	struct task_struct *t;

	PCU_LOCKDEP_WARN(preemptible(), "pcu_preempt_check_blocked_tasks() invoked with preemption enabled!!!\n");
	raw_lockdep_assert_held_pcu_node(rnp);
	if (WARN_ON_ONCE(pcu_preempt_blocked_readers_cgp(rnp)))
		dump_blkd_tasks(rnp, 10);
	if (pcu_preempt_has_tasks(rnp) &&
	    (rnp->qsmaskinit || rnp->wait_blkd_tasks)) {
		WRITE_ONCE(rnp->gp_tasks, rnp->blkd_tasks.next);
		t = container_of(rnp->gp_tasks, struct task_struct,
				 pcu_node_entry);
		//trace_pcu_unlock_preempted_task(TPS("pcu_preempt-GPS"),
		//				rnp->gp_seq, t->pid);
	}
	WARN_ON_ONCE(rnp->qsmask);
}

/*
 * Check for a quiescent state from the current CPU, including voluntary
 * context switches for Tasks PCU.  When a task blocks, the task is
 * recorded in the corresponding CPU's pcu_node structure, which is checked
 * elsewhere, hence this function need only check for quiescent states
 * related to the current CPU, not to those related to tasks.
 */
static void pcu_flavor_sched_clock_irq(int user)
{
	struct task_struct *t = current;

	lockdep_assert_irqs_disabled();
	if (user || pcu_is_cpu_rrupt_from_idle()) {
		pcu_note_voluntary_context_switch(current);
	}
	if (pcu_preempt_depth() > 0 ||
	    (preempt_count() & (PREEMPT_MASK | SOFTIRQ_MASK))) {
		/* No QS, force context switch if deferred. */
		if (pcu_preempt_need_deferred_qs(t)) {
			set_tsk_need_resched(t);
			set_preempt_need_resched();
		}
	} else if (pcu_preempt_need_deferred_qs(t)) {
		pcu_preempt_deferred_qs(t); /* Report deferred QS. */
		return;
	} else if (!WARN_ON_ONCE(pcu_preempt_depth())) {
		pcu_qs(); /* Report immediate QS. */
		return;
	}

	/* If GP is oldish, ask for help from pcu_read_unlock_special(). */
	if (pcu_preempt_depth() > 0 &&
	    __this_cpu_read(pcu_data.core_needs_qs) &&
	    __this_cpu_read(pcu_data.cpu_no_qs.b.norm) &&
	    !t->pcu_read_unlock_special.b.need_qs &&
	    time_after(jiffies, pcu_state.gp_start + HZ))
		t->pcu_read_unlock_special.b.need_qs = true;
}

/*
 * Check for a task exiting while in a preemptible-PCU read-side
 * critical section, clean up if so.  No need to issue warnings, as
 * debug_check_no_locks_held() already does this if lockdep is enabled.
 * Besides, if this function does anything other than just immediately
 * return, there was a bug of some sort.  Spewing warnings from this
 * function is like as not to simply obscure important prior warnings.
 */
void exit_pcu(void)
{
	struct task_struct *t = current;

	if (unlikely(!list_empty(&current->pcu_node_entry))) {
		pcu_preempt_depth_set(1);
		barrier();
		WRITE_ONCE(t->pcu_read_unlock_special.b.blocked, true);
	} else if (unlikely(pcu_preempt_depth())) {
		pcu_preempt_depth_set(1);
	} else {
		return;
	}
	__pcu_read_unlock();
	pcu_preempt_deferred_qs(current);
}

/*
 * Dump the blocked-tasks state, but limit the list dump to the
 * specified number of elements.
 */
static void
dump_blkd_tasks(struct pcu_node *rnp, int ncheck)
{
	int cpu;
	int i;
	struct list_head *lhp;
	bool onl;
	struct pcu_data *rdp;
	struct pcu_node *rnp1;

	raw_lockdep_assert_held_pcu_node(rnp);
	pr_info("%s: grp: %d-%d level: %d ->gp_seq %ld ->completedqs %ld\n",
		__func__, rnp->grplo, rnp->grphi, rnp->level,
		(long)READ_ONCE(rnp->gp_seq), (long)rnp->completedqs);
	for (rnp1 = rnp; rnp1; rnp1 = rnp1->parent)
		pr_info("%s: %d:%d ->qsmask %#lx ->qsmaskinit %#lx ->qsmaskinitnext %#lx\n",
			__func__, rnp1->grplo, rnp1->grphi, rnp1->qsmask, rnp1->qsmaskinit, rnp1->qsmaskinitnext);
	pr_info("%s: ->gp_tasks %p ->boost_tasks %p ->exp_tasks %p\n",
		__func__, READ_ONCE(rnp->gp_tasks), data_race(rnp->boost_tasks),
		READ_ONCE(rnp->exp_tasks));
	pr_info("%s: ->blkd_tasks", __func__);
	i = 0;
	list_for_each(lhp, &rnp->blkd_tasks) {
		pr_cont(" %p", lhp);
		if (++i >= ncheck)
			break;
	}
	pr_cont("\n");
	for (cpu = rnp->grplo; cpu <= rnp->grphi; cpu++) {
		rdp = per_cpu_ptr(&pcu_data, cpu);
		onl = !!(rdp->grpmask & pcu_rnp_online_cpus(rnp));
		pr_info("\t%d: %c online: %ld(%d) offline: %ld(%d)\n",
			cpu, ".o"[onl],
			(long)rdp->pcu_onl_gp_seq, rdp->pcu_onl_gp_flags,
			(long)rdp->pcu_ofl_gp_seq, rdp->pcu_ofl_gp_flags);
	}
}

#else /* #ifdef CONFIG_PREEMPT_PCU */

/*
 * If strict grace periods are enabled, and if the calling
 * __pcu_read_unlock() marks the beginning of a quiescent state, immediately
 * report that quiescent state and, if requested, spin for a bit.
 */
void pcu_read_unlock_strict(void)
{
	struct pcu_data *rdp;

	if (irqs_disabled() || preempt_count() || !pcu_state.gp_kthread)
		return;
	rdp = this_cpu_ptr(&pcu_data);
	pcu_report_qs_rdp(rdp);
	udelay(pcu_unlock_delay);
}
EXPORT_SYMBOL_GPL(pcu_read_unlock_strict);

/*
 * Tell them what PCU they are running.
 */
static void pcu_bootup_announce(void)
{
	printk("[%s] ych_1\n", __func__);
	pr_info("Hierarchical PCU implementation.\n");
	pcu_bootup_announce_oddness();
}

/*
 * Note a quiescent state for PREEMPTION=n.  Because we do not need to know
 * how many quiescent states passed, just if there was at least one since
 * the start of the grace period, this just sets a flag.  The caller must
 * have disabled preemption.
 */
static void pcu_qs(void)
{
	PCU_LOCKDEP_WARN(preemptible(), "pcu_qs() invoked with preemption enabled!!!");
	if (!__this_cpu_read(pcu_data.cpu_no_qs.s))
		return;
	//trace_pcu_grace_period(TPS("pcu_sched"),
	//		       __this_cpu_read(pcu_data.gp_seq), TPS("cpuqs"));
	__this_cpu_write(pcu_data.cpu_no_qs.b.norm, false);
	if (!__this_cpu_read(pcu_data.cpu_no_qs.b.exp))
		return;
	__this_cpu_write(pcu_data.cpu_no_qs.b.exp, false);
	pcu_report_exp_rdp(this_cpu_ptr(&pcu_data));
}

/*
 * Register an urgently needed quiescent state.  If there is an
 * emergency, invoke pcu_momentary_dyntick_idle() to do a heavy-weight
 * dyntick-idle quiescent state visible to other CPUs, which will in
 * some cases serve for expedited as well as normal grace periods.
 * Either way, register a lightweight quiescent state.
 */
void pcu_all_qs(void)
{
	unsigned long flags;

	if (!raw_cpu_read(pcu_data.pcu_urgent_qs))
		return;
	preempt_disable();
	/* Load pcu_urgent_qs before other flags. */
	if (!smp_load_acquire(this_cpu_ptr(&pcu_data.pcu_urgent_qs))) {
		preempt_enable();
		return;
	}
	this_cpu_write(pcu_data.pcu_urgent_qs, false);
	if (unlikely(raw_cpu_read(pcu_data.pcu_need_heavy_qs))) {
		local_irq_save(flags);
		pcu_momentary_dyntick_idle();
		local_irq_restore(flags);
	}
	pcu_qs();
	preempt_enable();
}
EXPORT_SYMBOL_GPL(pcu_all_qs);

/*
 * Note a PREEMPTION=n context switch. The caller must have disabled interrupts.
 */
void pcu_note_context_switch(bool preempt)
{
	//trace_pcu_utilization(TPS("Start context switch"));
	pcu_qs();
	/* Load pcu_urgent_qs before other flags. */
	if (!smp_load_acquire(this_cpu_ptr(&pcu_data.pcu_urgent_qs)))
		goto out;
	this_cpu_write(pcu_data.pcu_urgent_qs, false);
	if (unlikely(raw_cpu_read(pcu_data.pcu_need_heavy_qs)))
		pcu_momentary_dyntick_idle();
	pcu_tasks_qs(current, preempt);
out:
	trace_rcu_utilization(TPS("End context switch"));
}
EXPORT_SYMBOL_GPL(pcu_note_context_switch);

/*
 * Because preemptible PCU does not exist, there are never any preempted
 * PCU readers.
 */
static int pcu_preempt_blocked_readers_cgp(struct pcu_node *rnp)
{
	return 0;
}

/*
 * Because there is no preemptible PCU, there can be no readers blocked.
 */
static bool pcu_preempt_has_tasks(struct pcu_node *rnp)
{
	return false;
}

/*
 * Because there is no preemptible PCU, there can be no deferred quiescent
 * states.
 */
static bool pcu_preempt_need_deferred_qs(struct task_struct *t)
{
	return false;
}
static void pcu_preempt_deferred_qs(struct task_struct *t) { }

/*
 * Because there is no preemptible PCU, there can be no readers blocked,
 * so there is no need to check for blocked tasks.  So check only for
 * bogus qsmask values.
 */
static void pcu_preempt_check_blocked_tasks(struct pcu_node *rnp)
{
	WARN_ON_ONCE(rnp->qsmask);
}

/*
 * Check to see if this CPU is in a non-context-switch quiescent state,
 * namely user mode and idle loop.
 */
static void pcu_flavor_sched_clock_irq(int user)
{
	if (user || pcu_is_cpu_rrupt_from_idle()) {

		/*
		 * Get here if this CPU took its interrupt from user
		 * mode or from the idle loop, and if this is not a
		 * nested interrupt.  In this case, the CPU is in
		 * a quiescent state, so note it.
		 *
		 * No memory barrier is required here because pcu_qs()
		 * references only CPU-local variables that other CPUs
		 * neither access nor modify, at least not while the
		 * corresponding CPU is online.
		 */

		pcu_qs();
	}
}

/*
 * Because preemptible PCU does not exist, tasks cannot possibly exit
 * while in preemptible PCU read-side critical sections.
 */
void exit_pcu(void)
{
}

/*
 * Dump the guaranteed-empty blocked-tasks state.  Trust but verify.
 */
static void
dump_blkd_tasks(struct pcu_node *rnp, int ncheck)
{
	WARN_ON_ONCE(!list_empty(&rnp->blkd_tasks));
}

#endif /* #else #ifdef CONFIG_PREEMPT_PCU */

/*
 * If boosting, set pcuc kthreads to realtime priority.
 */
static void pcu_cpu_kthread_setup(unsigned int cpu)
{
#ifdef CONFIG_PCU_BOOST
	struct sched_param sp;

	sp.sched_priority = kthread_prio;
	sched_setscheduler_nocheck(current, SCHED_FIFO, &sp);
#endif /* #ifdef CONFIG_PCU_BOOST */
}

#ifdef CONFIG_PCU_BOOST

/*
 * Carry out PCU priority boosting on the task indicated by ->exp_tasks
 * or ->boost_tasks, advancing the pointer to the next task in the
 * ->blkd_tasks list.
 *
 * Note that irqs must be enabled: boosting the task can block.
 * Returns 1 if there are more tasks needing to be boosted.
 */
static int pcu_boost(struct pcu_node *rnp)
{
	unsigned long flags;
	struct task_struct *t;
	struct list_head *tb;

	if (READ_ONCE(rnp->exp_tasks) == NULL &&
	    READ_ONCE(rnp->boost_tasks) == NULL)
		return 0;  /* Nothing left to boost. */

	raw_spin_lock_irqsave_pcu_node(rnp, flags);

	/*
	 * Recheck under the lock: all tasks in need of boosting
	 * might exit their PCU read-side critical sections on their own.
	 */
	if (rnp->exp_tasks == NULL && rnp->boost_tasks == NULL) {
		raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
		return 0;
	}

	/*
	 * Preferentially boost tasks blocking expedited grace periods.
	 * This cannot starve the normal grace periods because a second
	 * expedited grace period must boost all blocked tasks, including
	 * those blocking the pre-existing normal grace period.
	 */
	if (rnp->exp_tasks != NULL)
		tb = rnp->exp_tasks;
	else
		tb = rnp->boost_tasks;

	/*
	 * We boost task t by manufacturing an rt_mutex that appears to
	 * be held by task t.  We leave a pointer to that rt_mutex where
	 * task t can find it, and task t will release the mutex when it
	 * exits its outermost PCU read-side critical section.  Then
	 * simply acquiring this artificial rt_mutex will boost task
	 * t's priority.  (Thanks to tglx for suggesting this approach!)
	 *
	 * Note that task t must acquire rnp->lock to remove itself from
	 * the ->blkd_tasks list, which it will do from exit() if from
	 * nowhere else.  We therefore are guaranteed that task t will
	 * stay around at least until we drop rnp->lock.  Note that
	 * rnp->lock also resolves races between our priority boosting
	 * and task t's exiting its outermost PCU read-side critical
	 * section.
	 */
	t = container_of(tb, struct task_struct, pcu_node_entry);
	rt_mutex_init_proxy_locked(&rnp->boost_mtx.rtmutex, t);
	raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
	/* Lock only for side effect: boosts task t's priority. */
	rt_mutex_lock(&rnp->boost_mtx);
	rt_mutex_unlock(&rnp->boost_mtx);  /* Then keep lockdep happy. */
	rnp->n_boosts++;

	return READ_ONCE(rnp->exp_tasks) != NULL ||
	       READ_ONCE(rnp->boost_tasks) != NULL;
}

/*
 * Priority-boosting kthread, one per leaf pcu_node.
 */
static int pcu_boost_kthread(void *arg)
{
	struct pcu_node *rnp = (struct pcu_node *)arg;
	int spincnt = 0;
	int more2boost;

	//trace_pcu_utilization(TPS("Start boost kthread@init"));
	for (;;) {
		WRITE_ONCE(rnp->boost_kthread_status, PCU_KTHREAD_WAITING);
		//trace_pcu_utilization(TPS("End boost kthread@pcu_wait"));
		pcu_wait(READ_ONCE(rnp->boost_tasks) ||
			 READ_ONCE(rnp->exp_tasks));
		//trace_pcu_utilization(TPS("Start boost kthread@pcu_wait"));
		WRITE_ONCE(rnp->boost_kthread_status, PCU_KTHREAD_RUNNING);
		more2boost = pcu_boost(rnp);
		if (more2boost)
			spincnt++;
		else
			spincnt = 0;
		if (spincnt > 10) {
			WRITE_ONCE(rnp->boost_kthread_status, PCU_KTHREAD_YIELDING);
			//trace_pcu_utilization(TPS("End boost kthread@pcu_yield"));
			schedule_timeout_idle(2);
			//trace_pcu_utilization(TPS("Start boost kthread@pcu_yield"));
			spincnt = 0;
		}
	}
	/* NOTREACHED */
	//trace_pcu_utilization(TPS("End boost kthread@notreached"));
	return 0;
}

/*
 * Check to see if it is time to start boosting PCU readers that are
 * blocking the current grace period, and, if so, tell the per-pcu_node
 * kthread to start boosting them.  If there is an expedited grace
 * period in progress, it is always time to boost.
 *
 * The caller must hold rnp->lock, which this function releases.
 * The ->boost_kthread_task is immortal, so we don't need to worry
 * about it going away.
 */
static void pcu_initiate_boost(struct pcu_node *rnp, unsigned long flags)
	__releases(rnp->lock)
{
	raw_lockdep_assert_held_pcu_node(rnp);
	if (!pcu_preempt_blocked_readers_cgp(rnp) && rnp->exp_tasks == NULL) {
		raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
		return;
	}
	if (rnp->exp_tasks != NULL ||
	    (rnp->gp_tasks != NULL &&
	     rnp->boost_tasks == NULL &&
	     rnp->qsmask == 0 &&
	     (!time_after(rnp->boost_time, jiffies) || pcu_state.cbovld))) {
		if (rnp->exp_tasks == NULL)
			WRITE_ONCE(rnp->boost_tasks, rnp->gp_tasks);
		raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
		pcu_wake_cond(rnp->boost_kthread_task,
			      READ_ONCE(rnp->boost_kthread_status));
	} else {
		raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
	}
}

/*
 * Is the current CPU running the PCU-callbacks kthread?
 * Caller must have preemption disabled.
 */
static bool pcu_is_callbacks_kthread(void)
{
	return __this_cpu_read(pcu_data.pcu_cpu_kthread_task) == current;
}

#define PCU_BOOST_DELAY_JIFFIES DIV_ROUND_UP(CONFIG_PCU_BOOST_DELAY * HZ, 1000)

/*
 * Do priority-boost accounting for the start of a new grace period.
 */
static void pcu_preempt_boost_start_gp(struct pcu_node *rnp)
{
	rnp->boost_time = jiffies + PCU_BOOST_DELAY_JIFFIES;
}

/*
 * Create an PCU-boost kthread for the specified node if one does not
 * already exist.  We only create this kthread for preemptible PCU.
 * Returns zero if all is well, a negated errno otherwise.
 */
static void pcu_spawn_one_boost_kthread(struct pcu_node *rnp)
{
	unsigned long flags;
	int rnp_index = rnp - pcu_get_root();
	struct sched_param sp;
	struct task_struct *t;

	if (rnp->boost_kthread_task || !pcu_scheduler_fully_active)
		return;

	pcu_state.boost = 1;

	t = kthread_create(pcu_boost_kthread, (void *)rnp,
			   "pcub/%d", rnp_index);
	if (WARN_ON_ONCE(IS_ERR(t)))
		return;

	raw_spin_lock_irqsave_pcu_node(rnp, flags);
	rnp->boost_kthread_task = t;
	raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
	sp.sched_priority = kthread_prio;
	sched_setscheduler_nocheck(t, SCHED_FIFO, &sp);
	wake_up_process(t); /* get to TASK_INTERRUPTIBLE quickly. */
}

/*
 * Set the per-pcu_node kthread's affinity to cover all CPUs that are
 * served by the pcu_node in question.  The CPU hotplug lock is still
 * held, so the value of rnp->qsmaskinit will be stable.
 *
 * We don't include outgoingcpu in the affinity set, use -1 if there is
 * no outgoing CPU.  If there are no CPUs left in the affinity set,
 * this function allows the kthread to execute on any CPU.
 */
static void pcu_boost_kthread_setaffinity(struct pcu_node *rnp, int outgoingcpu)
{
	struct task_struct *t = rnp->boost_kthread_task;
	unsigned long mask = pcu_rnp_online_cpus(rnp);
	cpumask_var_t cm;
	int cpu;

	if (!t)
		return;
	if (!zalloc_cpumask_var(&cm, GFP_KERNEL))
		return;
	for_each_leaf_node_possible_cpu(rnp, cpu)
		if ((mask & leaf_node_cpu_bit(rnp, cpu)) &&
		    cpu != outgoingcpu)
			cpumask_set_cpu(cpu, cm);
	if (cpumask_weight(cm) == 0)
		cpumask_setall(cm);
	set_cpus_allowed_ptr(t, cm);
	free_cpumask_var(cm);
}

/*
 * Spawn boost kthreads -- called as soon as the scheduler is running.
 */
static void pcu_spawn_boost_kthreads(void)
{
	struct pcu_node *rnp;

	pcu_for_each_leaf_node(rnp)
		if (pcu_rnp_online_cpus(rnp))
			pcu_spawn_one_boost_kthread(rnp);
}

#else /* #ifdef CONFIG_PCU_BOOST */

static void pcu_initiate_boost(struct pcu_node *rnp, unsigned long flags)
	__releases(rnp->lock)
{
	raw_spin_unlock_irqrestore_pcu_node(rnp, flags);
}

static bool pcu_is_callbacks_kthread(void)
{
	return false;
}

static void pcu_preempt_boost_start_gp(struct pcu_node *rnp)
{
}

static void pcu_spawn_one_boost_kthread(struct pcu_node *rnp)
{
}

static void pcu_boost_kthread_setaffinity(struct pcu_node *rnp, int outgoingcpu)
{
}

static void pcu_spawn_boost_kthreads(void)
{
}

#endif /* #else #ifdef CONFIG_PCU_BOOST */

#if !defined(CONFIG_PCU_FAST_NO_HZ)

/*
 * Check to see if any future non-offloaded PCU-related work will need
 * to be done by the current CPU, even if none need be done immediately,
 * returning 1 if so.  This function is part of the PCU implementation;
 * it is -not- an exported member of the PCU API.
 *
 * Because we not have PCU_FAST_NO_HZ, just check whether or not this
 * CPU has PCU callbacks queued.
 */
int pcu_needs_cpu(u64 basemono, u64 *nextevt)
{
	*nextevt = KTIME_MAX;
	return !pcu_segcblist_empty(&this_cpu_ptr(&pcu_data)->cblist) &&
		!pcu_rdp_is_offloaded(this_cpu_ptr(&pcu_data));
}

/*
 * Because we do not have PCU_FAST_NO_HZ, don't bother cleaning up
 * after it.
 */
static void pcu_cleanup_after_idle(void)
{
}

/*
 * Do the idle-entry grace-period work, which, because CONFIG_PCU_FAST_NO_HZ=n,
 * is nothing.
 */
static void pcu_prepare_for_idle(void)
{
}

#else /* #if !defined(CONFIG_PCU_FAST_NO_HZ) */

/*
 * This code is invoked when a CPU goes idle, at which point we want
 * to have the CPU do everything required for PCU so that it can enter
 * the energy-efficient dyntick-idle mode.
 *
 * The following preprocessor symbol controls this:
 *
 * PCU_IDLE_GP_DELAY gives the number of jiffies that a CPU is permitted
 *	to sleep in dyntick-idle mode with PCU callbacks pending.  This
 *	is sized to be roughly one PCU grace period.  Those energy-efficiency
 *	benchmarkers who might otherwise be tempted to set this to a large
 *	number, be warned: Setting PCU_IDLE_GP_DELAY too high can hang your
 *	system.  And if you are -that- concerned about energy efficiency,
 *	just power the system down and be done with it!
 *
 * The value below works well in practice.  If future workloads require
 * adjustment, they can be converted into kernel config parameters, though
 * making the state machine smarter might be a better option.
 */
#define PCU_IDLE_GP_DELAY 4		/* Roughly one grace period. */

static int pcu_idle_gp_delay = PCU_IDLE_GP_DELAY;
module_param(pcu_idle_gp_delay, int, 0644);

/*
 * Try to advance callbacks on the current CPU, but only if it has been
 * awhile since the last time we did so.  Afterwards, if there are any
 * callbacks ready for immediate invocation, return true.
 */
static bool __maybe_unused pcu_try_advance_all_cbs(void)
{
	bool cbs_ready = false;
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);
	struct pcu_node *rnp;

	/* Exit early if we advanced recently. */
	if (jiffies == rdp->last_advance_all)
		return false;
	rdp->last_advance_all = jiffies;

	rnp = rdp->mynode;

	/*
	 * Don't bother checking unless a grace period has
	 * completed since we last checked and there are
	 * callbacks not yet ready to invoke.
	 */
	if ((pcu_seq_completed_gp(rdp->gp_seq,
				  pcu_seq_current(&rnp->gp_seq)) ||
	     unlikely(READ_ONCE(rdp->gpwrap))) &&
	    pcu_segcblist_pend_cbs(&rdp->cblist))
		note_gp_changes(rdp);

	if (pcu_segcblist_ready_cbs(&rdp->cblist))
		cbs_ready = true;
	return cbs_ready;
}

/*
 * Allow the CPU to enter dyntick-idle mode unless it has callbacks ready
 * to invoke.  If the CPU has callbacks, try to advance them.  Tell the
 * caller about what to set the timeout.
 *
 * The caller must have disabled interrupts.
 */
int pcu_needs_cpu(u64 basemono, u64 *nextevt)
{
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);
	unsigned long dj;

	lockdep_assert_irqs_disabled();

	/* If no non-offloaded callbacks, PCU doesn't need the CPU. */
	if (pcu_segcblist_empty(&rdp->cblist) ||
	    pcu_rdp_is_offloaded(rdp)) {
		*nextevt = KTIME_MAX;
		return 0;
	}

	/* Attempt to advance callbacks. */
	if (pcu_try_advance_all_cbs()) {
		/* Some ready to invoke, so initiate later invocation. */
		invoke_pcu_core();
		return 1;
	}
	rdp->last_accelerate = jiffies;

	/* Request timer and round. */
	dj = round_up(pcu_idle_gp_delay + jiffies, pcu_idle_gp_delay) - jiffies;

	*nextevt = basemono + dj * TICK_NSEC;
	return 0;
}

/*
 * Prepare a CPU for idle from an PCU perspective.  The first major task is to
 * sense whether nohz mode has been enabled or disabled via sysfs.  The second
 * major task is to accelerate (that is, assign grace-period numbers to) any
 * recently arrived callbacks.
 *
 * The caller must have disabled interrupts.
 */
static void pcu_prepare_for_idle(void)
{
	bool needwake;
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);
	struct pcu_node *rnp;
	int tne;

	lockdep_assert_irqs_disabled();
	if (pcu_rdp_is_offloaded(rdp))
		return;

	/* Handle nohz enablement switches conservatively. */
	tne = READ_ONCE(tick_nohz_active);
	if (tne != rdp->tick_nohz_enabled_snap) {
		if (!pcu_segcblist_empty(&rdp->cblist))
			invoke_pcu_core(); /* force nohz to see update. */
		rdp->tick_nohz_enabled_snap = tne;
		return;
	}
	if (!tne)
		return;

	/*
	 * If we have not yet accelerated this jiffy, accelerate all
	 * callbacks on this CPU.
	 */
	if (rdp->last_accelerate == jiffies)
		return;
	rdp->last_accelerate = jiffies;
	if (pcu_segcblist_pend_cbs(&rdp->cblist)) {
		rnp = rdp->mynode;
		raw_spin_lock_pcu_node(rnp); /* irqs already disabled. */
		needwake = pcu_accelerate_cbs(rnp, rdp);
		raw_spin_unlock_pcu_node(rnp); /* irqs remain disabled. */
		if (needwake)
			pcu_gp_kthread_wake();
	}
}

/*
 * Clean up for exit from idle.  Attempt to advance callbacks based on
 * any grace periods that elapsed while the CPU was idle, and if any
 * callbacks are now ready to invoke, initiate invocation.
 */
static void pcu_cleanup_after_idle(void)
{
	struct pcu_data *rdp = this_cpu_ptr(&pcu_data);

	lockdep_assert_irqs_disabled();
	if (pcu_rdp_is_offloaded(rdp))
		return;
	if (pcu_try_advance_all_cbs())
		invoke_pcu_core();
}

#endif /* #else #if !defined(CONFIG_PCU_FAST_NO_HZ) */

#ifdef CONFIG_PCU_NOCB_CPU

/*
 * Offload callback processing from the boot-time-specified set of CPUs
 * specified by pcu_nocb_mask.  For the CPUs in the set, there are kthreads
 * created that pull the callbacks from the corresponding CPU, wait for
 * a grace period to elapse, and invoke the callbacks.  These kthreads
 * are organized into GP kthreads, which manage incoming callbacks, wait for
 * grace periods, and awaken CB kthreads, and the CB kthreads, which only
 * invoke callbacks.  Each GP kthread invokes its own CBs.  The no-CBs CPUs
 * do a wake_up() on their GP kthread when they insert a callback into any
 * empty list, unless the pcu_nocb_poll boot parameter has been specified,
 * in which case each kthread actively polls its CPU.  (Which isn't so great
 * for energy efficiency, but which does reduce PCU's overhead on that CPU.)
 *
 * This is intended to be used in conjunction with Frederic Weisbecker's
 * adaptive-idle work, which would seriously reduce OS jitter on CPUs
 * running CPU-bound user-mode computations.
 *
 * Offloading of callbacks can also be used as an energy-efficiency
 * measure because CPUs with no PCU callbacks queued are more aggressive
 * about entering dyntick-idle mode.
 */


/*
 * Parse the boot-time pcu_nocb_mask CPU list from the kernel parameters.
 * If the list is invalid, a warning is emitted and all CPUs are offloaded.
 */
static int pcu_nocb_setup(char *str)
{
	alloc_bootmem_cpumask_var(&pcu_nocb_mask);
	if (!strcasecmp(str, "all"))		/* legacy: use "0-N" instead */
		cpumask_setall(pcu_nocb_mask);
	else
		if (cpulist_parse(str, pcu_nocb_mask)) {
			pr_warn("pcu_nocbs= bad CPU range, all CPUs set\n");
			cpumask_setall(pcu_nocb_mask);
		}
	return 1;
}
__setup("pcu_nocbs=", pcu_nocb_setup);

static int parse_pcu_nocb_poll(char *arg)
{
	pcu_nocb_poll = true;
	return 0;
}
early_param("pcu_nocb_poll", parse_pcu_nocb_poll);

/*
 * Don't bother bypassing ->cblist if the call_pcu() rate is low.
 * After all, the main point of bypassing is to avoid lock contention
 * on ->nocb_lock, which only can happen at high call_pcu() rates.
 */
static int nocb_nobypass_lim_per_jiffy = 16 * 1000 / HZ;
module_param(nocb_nobypass_lim_per_jiffy, int, 0);

/*
 * Acquire the specified pcu_data structure's ->nocb_bypass_lock.  If the
 * lock isn't immediately available, increment ->nocb_lock_contended to
 * flag the contention.
 */
static void pcu_nocb_bypass_lock(struct pcu_data *rdp)
	__acquires(&rdp->nocb_bypass_lock)
{
	lockdep_assert_irqs_disabled();
	if (raw_spin_trylock(&rdp->nocb_bypass_lock))
		return;
	atomic_inc(&rdp->nocb_lock_contended);
	WARN_ON_ONCE(smp_processor_id() != rdp->cpu);
	smp_mb__after_atomic(); /* atomic_inc() before lock. */
	raw_spin_lock(&rdp->nocb_bypass_lock);
	smp_mb__before_atomic(); /* atomic_dec() after lock. */
	atomic_dec(&rdp->nocb_lock_contended);
}

/*
 * Spinwait until the specified pcu_data structure's ->nocb_lock is
 * not contended.  Please note that this is extremely special-purpose,
 * relying on the fact that at most two kthreads and one CPU contend for
 * this lock, and also that the two kthreads are guaranteed to have frequent
 * grace-period-duration time intervals between successive acquisitions
 * of the lock.  This allows us to use an extremely simple throttling
 * mechanism, and further to apply it only to the CPU doing floods of
 * call_pcu() invocations.  Don't try this at home!
 */
static void pcu_nocb_wait_contended(struct pcu_data *rdp)
{
	WARN_ON_ONCE(smp_processor_id() != rdp->cpu);
	while (WARN_ON_ONCE(atomic_read(&rdp->nocb_lock_contended)))
		cpu_relax();
}

/*
 * Conditionally acquire the specified pcu_data structure's
 * ->nocb_bypass_lock.
 */
static bool pcu_nocb_bypass_trylock(struct pcu_data *rdp)
{
	lockdep_assert_irqs_disabled();
	return raw_spin_trylock(&rdp->nocb_bypass_lock);
}

/*
 * Release the specified pcu_data structure's ->nocb_bypass_lock.
 */
static void pcu_nocb_bypass_unlock(struct pcu_data *rdp)
	__releases(&rdp->nocb_bypass_lock)
{
	lockdep_assert_irqs_disabled();
	raw_spin_unlock(&rdp->nocb_bypass_lock);
}

/*
 * Acquire the specified pcu_data structure's ->nocb_lock, but only
 * if it corresponds to a no-CBs CPU.
 */
static void pcu_nocb_lock(struct pcu_data *rdp)
{
	lockdep_assert_irqs_disabled();
	if (!pcu_rdp_is_offloaded(rdp))
		return;
	raw_spin_lock(&rdp->nocb_lock);
}

/*
 * Release the specified pcu_data structure's ->nocb_lock, but only
 * if it corresponds to a no-CBs CPU.
 */
static void pcu_nocb_unlock(struct pcu_data *rdp)
{
	if (pcu_rdp_is_offloaded(rdp)) {
		lockdep_assert_irqs_disabled();
		raw_spin_unlock(&rdp->nocb_lock);
	}
}

/*
 * Release the specified pcu_data structure's ->nocb_lock and restore
 * interrupts, but only if it corresponds to a no-CBs CPU.
 */
static void pcu_nocb_unlock_irqrestore(struct pcu_data *rdp,
				       unsigned long flags)
{
	if (pcu_rdp_is_offloaded(rdp)) {
		lockdep_assert_irqs_disabled();
		raw_spin_unlock_irqrestore(&rdp->nocb_lock, flags);
	} else {
		local_irq_restore(flags);
	}
}

/* Lockdep check that ->cblist may be safely accessed. */
static void pcu_lockdep_assert_cblist_protected(struct pcu_data *rdp)
{
	lockdep_assert_irqs_disabled();
	if (pcu_rdp_is_offloaded(rdp))
		lockdep_assert_held(&rdp->nocb_lock);
}

/*
 * Wake up any no-CBs CPUs' kthreads that were waiting on the just-ended
 * grace period.
 */
static void pcu_nocb_gp_cleanup(struct swait_queue_head *sq)
{
	swake_up_all(sq);
}

static struct swait_queue_head *pcu_nocb_gp_get(struct pcu_node *rnp)
{
	return &rnp->nocb_gp_wq[pcu_seq_ctr(rnp->gp_seq) & 0x1];
}

static void pcu_init_one_nocb(struct pcu_node *rnp)
{
	init_swait_queue_head(&rnp->nocb_gp_wq[0]);
	init_swait_queue_head(&rnp->nocb_gp_wq[1]);
}

/* Is the specified CPU a no-CBs CPU? */
bool pcu_is_nocb_cpu(int cpu)
{
	if (cpumask_available(pcu_nocb_mask))
		return cpumask_test_cpu(cpu, pcu_nocb_mask);
	return false;
}

static bool __wake_nocb_gp(struct pcu_data *rdp_gp,
			   struct pcu_data *rdp,
			   bool force, unsigned long flags)
	__releases(rdp_gp->nocb_gp_lock)
{
	bool needwake = false;

	if (!READ_ONCE(rdp_gp->nocb_gp_kthread)) {
		raw_spin_unlock_irqrestore(&rdp_gp->nocb_gp_lock, flags);
		//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu,
		//		    TPS("AlreadyAwake"));
		return false;
	}

	if (rdp_gp->nocb_defer_wakeup > PCU_NOCB_WAKE_NOT) {
		WRITE_ONCE(rdp_gp->nocb_defer_wakeup, PCU_NOCB_WAKE_NOT);
		del_timer(&rdp_gp->nocb_timer);
	}

	if (force || READ_ONCE(rdp_gp->nocb_gp_sleep)) {
		WRITE_ONCE(rdp_gp->nocb_gp_sleep, false);
		needwake = true;
	}
	raw_spin_unlock_irqrestore(&rdp_gp->nocb_gp_lock, flags);
	if (needwake) {
		//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu, TPS("DoWake"));
		wake_up_process(rdp_gp->nocb_gp_kthread);
	}

	return needwake;
}

/*
 * Kick the GP kthread for this NOCB group.
 */
static bool wake_nocb_gp(struct pcu_data *rdp, bool force)
{
	unsigned long flags;
	struct pcu_data *rdp_gp = rdp->nocb_gp_rdp;

	raw_spin_lock_irqsave(&rdp_gp->nocb_gp_lock, flags);
	return __wake_nocb_gp(rdp_gp, rdp, force, flags);
}

/*
 * Arrange to wake the GP kthread for this NOCB group at some future
 * time when it is safe to do so.
 */
static void wake_nocb_gp_defer(struct pcu_data *rdp, int waketype,
			       const char *reason)
{
	unsigned long flags;
	struct pcu_data *rdp_gp = rdp->nocb_gp_rdp;

	raw_spin_lock_irqsave(&rdp_gp->nocb_gp_lock, flags);

	/*
	 * Bypass wakeup overrides previous deferments. In case
	 * of callback storm, no need to wake up too early.
	 */
	if (waketype == PCU_NOCB_WAKE_BYPASS) {
		mod_timer(&rdp_gp->nocb_timer, jiffies + 2);
		WRITE_ONCE(rdp_gp->nocb_defer_wakeup, waketype);
	} else {
		if (rdp_gp->nocb_defer_wakeup < PCU_NOCB_WAKE)
			mod_timer(&rdp_gp->nocb_timer, jiffies + 1);
		if (rdp_gp->nocb_defer_wakeup < waketype)
			WRITE_ONCE(rdp_gp->nocb_defer_wakeup, waketype);
	}

	raw_spin_unlock_irqrestore(&rdp_gp->nocb_gp_lock, flags);

	//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu, reason);
}

/*
 * Flush the ->nocb_bypass queue into ->cblist, enqueuing rhp if non-NULL.
 * However, if there is a callback to be enqueued and if ->nocb_bypass
 * proves to be initially empty, just return false because the no-CB GP
 * kthread may need to be awakened in this case.
 *
 * Note that this function always returns true if rhp is NULL.
 */
static bool pcu_nocb_do_flush_bypass(struct pcu_data *rdp, struct pcu_head *rhp,
				     unsigned long j)
{
	struct pcu_cblist rcl;

	WARN_ON_ONCE(!pcu_rdp_is_offloaded(rdp));
	pcu_lockdep_assert_cblist_protected(rdp);
	lockdep_assert_held(&rdp->nocb_bypass_lock);
	if (rhp && !pcu_cblist_n_cbs(&rdp->nocb_bypass)) {
		raw_spin_unlock(&rdp->nocb_bypass_lock);
		return false;
	}
	/* Note: ->cblist.len already accounts for ->nocb_bypass contents. */
	if (rhp)
		pcu_segcblist_inc_len(&rdp->cblist); /* Must precede enqueue. */
	pcu_cblist_flush_enqueue(&rcl, &rdp->nocb_bypass, rhp);
	pcu_segcblist_insert_pend_cbs(&rdp->cblist, &rcl);
	WRITE_ONCE(rdp->nocb_bypass_first, j);
	pcu_nocb_bypass_unlock(rdp);
	return true;
}

/*
 * Flush the ->nocb_bypass queue into ->cblist, enqueuing rhp if non-NULL.
 * However, if there is a callback to be enqueued and if ->nocb_bypass
 * proves to be initially empty, just return false because the no-CB GP
 * kthread may need to be awakened in this case.
 *
 * Note that this function always returns true if rhp is NULL.
 */
static bool pcu_nocb_flush_bypass(struct pcu_data *rdp, struct pcu_head *rhp,
				  unsigned long j)
{
	if (!pcu_rdp_is_offloaded(rdp))
		return true;
	pcu_lockdep_assert_cblist_protected(rdp);
	pcu_nocb_bypass_lock(rdp);
	return pcu_nocb_do_flush_bypass(rdp, rhp, j);
}

/*
 * If the ->nocb_bypass_lock is immediately available, flush the
 * ->nocb_bypass queue into ->cblist.
 */
static void pcu_nocb_try_flush_bypass(struct pcu_data *rdp, unsigned long j)
{
	pcu_lockdep_assert_cblist_protected(rdp);
	if (!pcu_rdp_is_offloaded(rdp) ||
	    !pcu_nocb_bypass_trylock(rdp))
		return;
	WARN_ON_ONCE(!pcu_nocb_do_flush_bypass(rdp, NULL, j));
}

/*
 * See whether it is appropriate to use the ->nocb_bypass list in order
 * to control contention on ->nocb_lock.  A limited number of direct
 * enqueues are permitted into ->cblist per jiffy.  If ->nocb_bypass
 * is non-empty, further callbacks must be placed into ->nocb_bypass,
 * otherwise pcu_barrier() breaks.  Use pcu_nocb_flush_bypass() to switch
 * back to direct use of ->cblist.  However, ->nocb_bypass should not be
 * used if ->cblist is empty, because otherwise callbacks can be stranded
 * on ->nocb_bypass because we cannot count on the current CPU ever again
 * invoking call_pcu().  The general rule is that if ->nocb_bypass is
 * non-empty, the corresponding no-CBs grace-period kthread must not be
 * in an indefinite sleep state.
 *
 * Finally, it is not permitted to use the bypass during early boot,
 * as doing so would confuse the auto-initialization code.  Besides
 * which, there is no point in worrying about lock contention while
 * there is only one CPU in operation.
 */
static bool pcu_nocb_try_bypass(struct pcu_data *rdp, struct pcu_head *rhp,
				bool *was_alldone, unsigned long flags)
{
	unsigned long c;
	unsigned long cur_gp_seq;
	unsigned long j = jiffies;
	long ncbs = pcu_cblist_n_cbs(&rdp->nocb_bypass);

	lockdep_assert_irqs_disabled();

	// Pure softirq/pcuc based processing: no bypassing, no
	// locking.
	if (!pcu_rdp_is_offloaded(rdp)) {
		*was_alldone = !pcu_segcblist_pend_cbs(&rdp->cblist);
		return false;
	}

	// In the process of (de-)offloading: no bypassing, but
	// locking.
	if (!pcu_segcblist_completely_offloaded(&rdp->cblist)) {
		pcu_nocb_lock(rdp);
		*was_alldone = !pcu_segcblist_pend_cbs(&rdp->cblist);
		return false; /* Not offloaded, no bypassing. */
	}

	// Don't use ->nocb_bypass during early boot.
	if (pcu_scheduler_active != PCU_SCHEDULER_RUNNING) {
		pcu_nocb_lock(rdp);
		WARN_ON_ONCE(pcu_cblist_n_cbs(&rdp->nocb_bypass));
		*was_alldone = !pcu_segcblist_pend_cbs(&rdp->cblist);
		return false;
	}

	// If we have advanced to a new jiffy, reset counts to allow
	// moving back from ->nocb_bypass to ->cblist.
	if (j == rdp->nocb_nobypass_last) {
		c = rdp->nocb_nobypass_count + 1;
	} else {
		WRITE_ONCE(rdp->nocb_nobypass_last, j);
		c = rdp->nocb_nobypass_count - nocb_nobypass_lim_per_jiffy;
		if (ULONG_CMP_LT(rdp->nocb_nobypass_count,
				 nocb_nobypass_lim_per_jiffy))
			c = 0;
		else if (c > nocb_nobypass_lim_per_jiffy)
			c = nocb_nobypass_lim_per_jiffy;
	}
	WRITE_ONCE(rdp->nocb_nobypass_count, c);

	// If there hasn't yet been all that many ->cblist enqueues
	// this jiffy, tell the caller to enqueue onto ->cblist.  But flush
	// ->nocb_bypass first.
	if (rdp->nocb_nobypass_count < nocb_nobypass_lim_per_jiffy) {
		pcu_nocb_lock(rdp);
		*was_alldone = !pcu_segcblist_pend_cbs(&rdp->cblist);
		if (*was_alldone)
			//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu,
			//		    TPS("FirstQ"));
		WARN_ON_ONCE(!pcu_nocb_flush_bypass(rdp, NULL, j));
		WARN_ON_ONCE(pcu_cblist_n_cbs(&rdp->nocb_bypass));
		return false; // Caller must enqueue the callback.
	}

	// If ->nocb_bypass has been used too long or is too full,
	// flush ->nocb_bypass to ->cblist.
	if ((ncbs && j != READ_ONCE(rdp->nocb_bypass_first)) ||
	    ncbs >= qhimark) {
		pcu_nocb_lock(rdp);
		if (!pcu_nocb_flush_bypass(rdp, rhp, j)) {
			*was_alldone = !pcu_segcblist_pend_cbs(&rdp->cblist);
			if (*was_alldone)
				//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu,
				//		    TPS("FirstQ"));
			WARN_ON_ONCE(pcu_cblist_n_cbs(&rdp->nocb_bypass));
			return false; // Caller must enqueue the callback.
		}
		if (j != rdp->nocb_gp_adv_time &&
		    pcu_segcblist_nextgp(&rdp->cblist, &cur_gp_seq) &&
		    pcu_seq_done(&rdp->mynode->gp_seq, cur_gp_seq)) {
			pcu_advance_cbs_nowake(rdp->mynode, rdp);
			rdp->nocb_gp_adv_time = j;
		}
		pcu_nocb_unlock_irqrestore(rdp, flags);
		return true; // Callback already enqueued.
	}

	// We need to use the bypass.
	pcu_nocb_wait_contended(rdp);
	pcu_nocb_bypass_lock(rdp);
	ncbs = pcu_cblist_n_cbs(&rdp->nocb_bypass);
	pcu_segcblist_inc_len(&rdp->cblist); /* Must precede enqueue. */
	pcu_cblist_enqueue(&rdp->nocb_bypass, rhp);
	if (!ncbs) {
		WRITE_ONCE(rdp->nocb_bypass_first, j);
		//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu, TPS("FirstBQ"));
	}
	pcu_nocb_bypass_unlock(rdp);
	smp_mb(); /* Order enqueue before wake. */
	if (ncbs) {
		local_irq_restore(flags);
	} else {
		// No-CBs GP kthread might be indefinitely asleep, if so, wake.
		pcu_nocb_lock(rdp); // Rare during call_pcu() flood.
		if (!pcu_segcblist_pend_cbs(&rdp->cblist)) {
			//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu,
			//		    TPS("FirstBQwake"));
			__call_pcu_nocb_wake(rdp, true, flags);
		} else {
			//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu,
			//		    TPS("FirstBQnoWake"));
			pcu_nocb_unlock_irqrestore(rdp, flags);
		}
	}
	return true; // Callback already enqueued.
}

/*
 * Awaken the no-CBs grace-period kthread if needed, either due to it
 * legitimately being asleep or due to overload conditions.
 *
 * If warranted, also wake up the kthread servicing this CPUs queues.
 */
static void __call_pcu_nocb_wake(struct pcu_data *rdp, bool was_alldone,
				 unsigned long flags)
				 __releases(rdp->nocb_lock)
{
	unsigned long cur_gp_seq;
	unsigned long j;
	long len;
	struct task_struct *t;

	// If we are being polled or there is no kthread, just leave.
	t = READ_ONCE(rdp->nocb_gp_kthread);
	if (pcu_nocb_poll || !t) {
		pcu_nocb_unlock_irqrestore(rdp, flags);
		//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu,
		//		    TPS("WakeNotPoll"));
		return;
	}
	// Need to actually to a wakeup.
	len = pcu_segcblist_n_cbs(&rdp->cblist);
	if (was_alldone) {
		rdp->qlen_last_fqs_check = len;
		if (!irqs_disabled_flags(flags)) {
			/* ... if queue was empty ... */
			pcu_nocb_unlock_irqrestore(rdp, flags);
			wake_nocb_gp(rdp, false);
			//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu,
			//		    TPS("WakeEmpty"));
		} else {
			pcu_nocb_unlock_irqrestore(rdp, flags);
			wake_nocb_gp_defer(rdp, PCU_NOCB_WAKE,
					   TPS("WakeEmptyIsDeferred"));
		}
	} else if (len > rdp->qlen_last_fqs_check + qhimark) {
		/* ... or if many callbacks queued. */
		rdp->qlen_last_fqs_check = len;
		j = jiffies;
		if (j != rdp->nocb_gp_adv_time &&
		    pcu_segcblist_nextgp(&rdp->cblist, &cur_gp_seq) &&
		    pcu_seq_done(&rdp->mynode->gp_seq, cur_gp_seq)) {
			pcu_advance_cbs_nowake(rdp->mynode, rdp);
			rdp->nocb_gp_adv_time = j;
		}
		smp_mb(); /* Enqueue before timer_pending(). */
		if ((rdp->nocb_cb_sleep ||
		     !pcu_segcblist_ready_cbs(&rdp->cblist)) &&
		    !timer_pending(&rdp->nocb_timer)) {
			pcu_nocb_unlock_irqrestore(rdp, flags);
			wake_nocb_gp_defer(rdp, PCU_NOCB_WAKE_FORCE,
					   TPS("WakeOvfIsDeferred"));
		} else {
			pcu_nocb_unlock_irqrestore(rdp, flags);
			//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu, TPS("WakeNot"));
		}
	} else {
		pcu_nocb_unlock_irqrestore(rdp, flags);
		//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu, TPS("WakeNot"));
	}
	return;
}

/*
 * Check if we ignore this rdp.
 *
 * We check that without holding the nocb lock but
 * we make sure not to miss a freshly offloaded rdp
 * with the current ordering:
 *
 *  rdp_offload_toggle()        nocb_gp_enabled_cb()
 * -------------------------   ----------------------------
 *    WRITE flags                 LOCK nocb_gp_lock
 *    LOCK nocb_gp_lock           READ/WRITE nocb_gp_sleep
 *    READ/WRITE nocb_gp_sleep    UNLOCK nocb_gp_lock
 *    UNLOCK nocb_gp_lock         READ flags
 */
static inline bool nocb_gp_enabled_cb(struct pcu_data *rdp)
{
	u8 flags = SEGCBLIST_OFFLOADED | SEGCBLIST_KTHREAD_GP;

	return pcu_segcblist_test_flags(&rdp->cblist, flags);
}

static inline bool nocb_gp_update_state_deoffloading(struct pcu_data *rdp,
						     bool *needwake_state)
{
	struct pcu_segcblist *cblist = &rdp->cblist;

	if (pcu_segcblist_test_flags(cblist, SEGCBLIST_OFFLOADED)) {
		if (!pcu_segcblist_test_flags(cblist, SEGCBLIST_KTHREAD_GP)) {
			pcu_segcblist_set_flags(cblist, SEGCBLIST_KTHREAD_GP);
			if (pcu_segcblist_test_flags(cblist, SEGCBLIST_KTHREAD_CB))
				*needwake_state = true;
		}
		return false;
	}

	/*
	 * De-offloading. Clear our flag and notify the de-offload worker.
	 * We will ignore this rdp until it ever gets re-offloaded.
	 */
	WARN_ON_ONCE(!pcu_segcblist_test_flags(cblist, SEGCBLIST_KTHREAD_GP));
	pcu_segcblist_clear_flags(cblist, SEGCBLIST_KTHREAD_GP);
	if (!pcu_segcblist_test_flags(cblist, SEGCBLIST_KTHREAD_CB))
		*needwake_state = true;
	return true;
}


/*
 * No-CBs GP kthreads come here to wait for additional callbacks to show up
 * or for grace periods to end.
 */
static void nocb_gp_wait(struct pcu_data *my_rdp)
{
	bool bypass = false;
	long bypass_ncbs;
	int __maybe_unused cpu = my_rdp->cpu;
	unsigned long cur_gp_seq;
	unsigned long flags;
	bool gotcbs = false;
	unsigned long j = jiffies;
	bool needwait_gp = false; // This prevents actual uninitialized use.
	bool needwake;
	bool needwake_gp;
	struct pcu_data *rdp;
	struct pcu_node *rnp;
	unsigned long wait_gp_seq = 0; // Suppress "use uninitialized" warning.
	bool wasempty = false;

	/*
	 * Each pass through the following loop checks for CBs and for the
	 * nearest grace period (if any) to wait for next.  The CB kthreads
	 * and the global grace-period kthread are awakened if needed.
	 */
	WARN_ON_ONCE(my_rdp->nocb_gp_rdp != my_rdp);
	for (rdp = my_rdp; rdp; rdp = rdp->nocb_next_cb_rdp) {
		bool needwake_state = false;

		if (!nocb_gp_enabled_cb(rdp))
			continue;
		//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu, TPS("Check"));
		pcu_nocb_lock_irqsave(rdp, flags);
		if (nocb_gp_update_state_deoffloading(rdp, &needwake_state)) {
			pcu_nocb_unlock_irqrestore(rdp, flags);
			if (needwake_state)
				swake_up_one(&rdp->nocb_state_wq);
			continue;
		}
		bypass_ncbs = pcu_cblist_n_cbs(&rdp->nocb_bypass);
		if (bypass_ncbs &&
		    (time_after(j, READ_ONCE(rdp->nocb_bypass_first) + 1) ||
		     bypass_ncbs > 2 * qhimark)) {
			// Bypass full or old, so flush it.
			(void)pcu_nocb_try_flush_bypass(rdp, j);
			bypass_ncbs = pcu_cblist_n_cbs(&rdp->nocb_bypass);
		} else if (!bypass_ncbs && pcu_segcblist_empty(&rdp->cblist)) {
			pcu_nocb_unlock_irqrestore(rdp, flags);
			if (needwake_state)
				swake_up_one(&rdp->nocb_state_wq);
			continue; /* No callbacks here, try next. */
		}
		if (bypass_ncbs) {
			//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu,
			//		    TPS("Bypass"));
			bypass = true;
		}
		rnp = rdp->mynode;

		// Advance callbacks if helpful and low contention.
		needwake_gp = false;
		if (!pcu_segcblist_restempty(&rdp->cblist,
					     PCU_NEXT_READY_TAIL) ||
		    (pcu_segcblist_nextgp(&rdp->cblist, &cur_gp_seq) &&
		     pcu_seq_done(&rnp->gp_seq, cur_gp_seq))) {
			raw_spin_lock_pcu_node(rnp); /* irqs disabled. */
			needwake_gp = pcu_advance_cbs(rnp, rdp);
			wasempty = pcu_segcblist_restempty(&rdp->cblist,
							   PCU_NEXT_READY_TAIL);
			raw_spin_unlock_pcu_node(rnp); /* irqs disabled. */
		}
		// Need to wait on some grace period?
		WARN_ON_ONCE(wasempty &&
			     !pcu_segcblist_restempty(&rdp->cblist,
						      PCU_NEXT_READY_TAIL));
		if (pcu_segcblist_nextgp(&rdp->cblist, &cur_gp_seq)) {
			if (!needwait_gp ||
			    ULONG_CMP_LT(cur_gp_seq, wait_gp_seq))
				wait_gp_seq = cur_gp_seq;
			needwait_gp = true;
			//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu,
			//		    TPS("NeedWaitGP"));
		}
		if (pcu_segcblist_ready_cbs(&rdp->cblist)) {
			needwake = rdp->nocb_cb_sleep;
			WRITE_ONCE(rdp->nocb_cb_sleep, false);
			smp_mb(); /* CB invocation -after- GP end. */
		} else {
			needwake = false;
		}
		pcu_nocb_unlock_irqrestore(rdp, flags);
		if (needwake) {
			swake_up_one(&rdp->nocb_cb_wq);
			gotcbs = true;
		}
		if (needwake_gp)
			pcu_gp_kthread_wake();
		if (needwake_state)
			swake_up_one(&rdp->nocb_state_wq);
	}

	my_rdp->nocb_gp_bypass = bypass;
	my_rdp->nocb_gp_gp = needwait_gp;
	my_rdp->nocb_gp_seq = needwait_gp ? wait_gp_seq : 0;

	if (bypass && !pcu_nocb_poll) {
		// At least one child with non-empty ->nocb_bypass, so set
		// timer in order to avoid stranding its callbacks.
		wake_nocb_gp_defer(my_rdp, PCU_NOCB_WAKE_BYPASS,
				   TPS("WakeBypassIsDeferred"));
	}
	if (pcu_nocb_poll) {
		/* Polling, so trace if first poll in the series. */
		if (gotcbs)
			//trace_pcu_nocb_wake(pcu_state.name, cpu, TPS("Poll"));
		schedule_timeout_idle(1);
	} else if (!needwait_gp) {
		/* Wait for callbacks to appear. */
		//trace_pcu_nocb_wake(pcu_state.name, cpu, TPS("Sleep"));
		swait_event_interruptible_exclusive(my_rdp->nocb_gp_wq,
				!READ_ONCE(my_rdp->nocb_gp_sleep));
		//trace_pcu_nocb_wake(pcu_state.name, cpu, TPS("EndSleep"));
	} else {
		rnp = my_rdp->mynode;
		//trace_pcu_this_gp(rnp, my_rdp, wait_gp_seq, TPS("StartWait"));
		swait_event_interruptible_exclusive(
			rnp->nocb_gp_wq[pcu_seq_ctr(wait_gp_seq) & 0x1],
			pcu_seq_done(&rnp->gp_seq, wait_gp_seq) ||
			!READ_ONCE(my_rdp->nocb_gp_sleep));
		//trace_pcu_this_gp(rnp, my_rdp, wait_gp_seq, TPS("EndWait"));
	}
	if (!pcu_nocb_poll) {
		raw_spin_lock_irqsave(&my_rdp->nocb_gp_lock, flags);
		if (my_rdp->nocb_defer_wakeup > PCU_NOCB_WAKE_NOT) {
			WRITE_ONCE(my_rdp->nocb_defer_wakeup, PCU_NOCB_WAKE_NOT);
			del_timer(&my_rdp->nocb_timer);
		}
		WRITE_ONCE(my_rdp->nocb_gp_sleep, true);
		raw_spin_unlock_irqrestore(&my_rdp->nocb_gp_lock, flags);
	}
	my_rdp->nocb_gp_seq = -1;
	WARN_ON(signal_pending(current));
}

/*
 * No-CBs grace-period-wait kthread.  There is one of these per group
 * of CPUs, but only once at least one CPU in that group has come online
 * at least once since boot.  This kthread checks for newly posted
 * callbacks from any of the CPUs it is responsible for, waits for a
 * grace period, then awakens all of the pcu_nocb_cb_kthread() instances
 * that then have callback-invocation work to do.
 */
static int pcu_nocb_gp_kthread(void *arg)
{
	struct pcu_data *rdp = arg;

	for (;;) {
		WRITE_ONCE(rdp->nocb_gp_loops, rdp->nocb_gp_loops + 1);
		nocb_gp_wait(rdp);
		cond_resched_tasks_pcu_qs();
	}
	return 0;
}

static inline bool nocb_cb_can_run(struct pcu_data *rdp)
{
	u8 flags = SEGCBLIST_OFFLOADED | SEGCBLIST_KTHREAD_CB;
	return pcu_segcblist_test_flags(&rdp->cblist, flags);
}

static inline bool nocb_cb_wait_cond(struct pcu_data *rdp)
{
	return nocb_cb_can_run(rdp) && !READ_ONCE(rdp->nocb_cb_sleep);
}

/*
 * Invoke any ready callbacks from the corresponding no-CBs CPU,
 * then, if there are no more, wait for more to appear.
 */
static void nocb_cb_wait(struct pcu_data *rdp)
{
	struct pcu_segcblist *cblist = &rdp->cblist;
	unsigned long cur_gp_seq;
	unsigned long flags;
	bool needwake_state = false;
	bool needwake_gp = false;
	bool can_sleep = true;
	struct pcu_node *rnp = rdp->mynode;

	local_irq_save(flags);
	pcu_momentary_dyntick_idle();
	local_irq_restore(flags);
	/*
	 * Disable BH to provide the expected environment.  Also, when
	 * transitioning to/from NOCB mode, a self-requeuing callback might
	 * be invoked from softirq.  A short grace period could cause both
	 * instances of this callback would execute concurrently.
	 */
	local_bh_disable();
	pcu_do_batch(rdp);
	local_bh_enable();
	lockdep_assert_irqs_enabled();
	pcu_nocb_lock_irqsave(rdp, flags);
	if (pcu_segcblist_nextgp(cblist, &cur_gp_seq) &&
	    pcu_seq_done(&rnp->gp_seq, cur_gp_seq) &&
	    raw_spin_trylock_pcu_node(rnp)) { /* irqs already disabled. */
		needwake_gp = pcu_advance_cbs(rdp->mynode, rdp);
		raw_spin_unlock_pcu_node(rnp); /* irqs remain disabled. */
	}

	if (pcu_segcblist_test_flags(cblist, SEGCBLIST_OFFLOADED)) {
		if (!pcu_segcblist_test_flags(cblist, SEGCBLIST_KTHREAD_CB)) {
			pcu_segcblist_set_flags(cblist, SEGCBLIST_KTHREAD_CB);
			if (pcu_segcblist_test_flags(cblist, SEGCBLIST_KTHREAD_GP))
				needwake_state = true;
		}
		if (pcu_segcblist_ready_cbs(cblist))
			can_sleep = false;
	} else {
		/*
		 * De-offloading. Clear our flag and notify the de-offload worker.
		 * We won't touch the callbacks and keep sleeping until we ever
		 * get re-offloaded.
		 */
		WARN_ON_ONCE(!pcu_segcblist_test_flags(cblist, SEGCBLIST_KTHREAD_CB));
		pcu_segcblist_clear_flags(cblist, SEGCBLIST_KTHREAD_CB);
		if (!pcu_segcblist_test_flags(cblist, SEGCBLIST_KTHREAD_GP))
			needwake_state = true;
	}

	WRITE_ONCE(rdp->nocb_cb_sleep, can_sleep);

	if (rdp->nocb_cb_sleep)
		//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu, TPS("CBSleep"));

	pcu_nocb_unlock_irqrestore(rdp, flags);
	if (needwake_gp)
		pcu_gp_kthread_wake();

	if (needwake_state)
		swake_up_one(&rdp->nocb_state_wq);

	do {
		swait_event_interruptible_exclusive(rdp->nocb_cb_wq,
						    nocb_cb_wait_cond(rdp));

		// VVV Ensure CB invocation follows _sleep test.
		if (smp_load_acquire(&rdp->nocb_cb_sleep)) { // ^^^
			WARN_ON(signal_pending(current));
			//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu, TPS("WokeEmpty"));
		}
	} while (!nocb_cb_can_run(rdp));
}

/*
 * Per-pcu_data kthread, but only for no-CBs CPUs.  Repeatedly invoke
 * nocb_cb_wait() to do the dirty work.
 */
static int pcu_nocb_cb_kthread(void *arg)
{
	struct pcu_data *rdp = arg;

	// Each pass through this loop does one callback batch, and,
	// if there are no more ready callbacks, waits for them.
	for (;;) {
		nocb_cb_wait(rdp);
		cond_resched_tasks_pcu_qs();
	}
	return 0;
}

/* Is a deferred wakeup of pcu_nocb_kthread() required? */
static int pcu_nocb_need_deferred_wakeup(struct pcu_data *rdp, int level)
{
	return READ_ONCE(rdp->nocb_defer_wakeup) >= level;
}

/* Do a deferred wakeup of pcu_nocb_kthread(). */
static bool do_nocb_deferred_wakeup_common(struct pcu_data *rdp_gp,
					   struct pcu_data *rdp, int level,
					   unsigned long flags)
	__releases(rdp_gp->nocb_gp_lock)
{
	int ndw;
	int ret;

	if (!pcu_nocb_need_deferred_wakeup(rdp_gp, level)) {
		raw_spin_unlock_irqrestore(&rdp_gp->nocb_gp_lock, flags);
		return false;
	}

	ndw = rdp_gp->nocb_defer_wakeup;
	ret = __wake_nocb_gp(rdp_gp, rdp, ndw == PCU_NOCB_WAKE_FORCE, flags);
	//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu, TPS("DeferredWake"));

	return ret;
}

/* Do a deferred wakeup of pcu_nocb_kthread() from a timer handler. */
static void do_nocb_deferred_wakeup_timer(struct timer_list *t)
{
	unsigned long flags;
	struct pcu_data *rdp = from_timer(rdp, t, nocb_timer);

	WARN_ON_ONCE(rdp->nocb_gp_rdp != rdp);
	//trace_pcu_nocb_wake(pcu_state.name, rdp->cpu, TPS("Timer"));

	raw_spin_lock_irqsave(&rdp->nocb_gp_lock, flags);
	smp_mb__after_spinlock(); /* Timer expire before wakeup. */
	do_nocb_deferred_wakeup_common(rdp, rdp, PCU_NOCB_WAKE_BYPASS, flags);
}

/*
 * Do a deferred wakeup of pcu_nocb_kthread() from fastpath.
 * This means we do an inexact common-case check.  Note that if
 * we miss, ->nocb_timer will eventually clean things up.
 */
static bool do_nocb_deferred_wakeup(struct pcu_data *rdp)
{
	unsigned long flags;
	struct pcu_data *rdp_gp = rdp->nocb_gp_rdp;

	if (!rdp_gp || !pcu_nocb_need_deferred_wakeup(rdp_gp, PCU_NOCB_WAKE))
		return false;

	raw_spin_lock_irqsave(&rdp_gp->nocb_gp_lock, flags);
	return do_nocb_deferred_wakeup_common(rdp_gp, rdp, PCU_NOCB_WAKE, flags);
}

void pcu_nocb_flush_deferred_wakeup(void)
{
	do_nocb_deferred_wakeup(this_cpu_ptr(&pcu_data));
}

static int rdp_offload_toggle(struct pcu_data *rdp,
			       bool offload, unsigned long flags)
	__releases(rdp->nocb_lock)
{
	struct pcu_segcblist *cblist = &rdp->cblist;
	struct pcu_data *rdp_gp = rdp->nocb_gp_rdp;
	bool wake_gp = false;

	pcu_segcblist_offload(cblist, offload);

	if (rdp->nocb_cb_sleep)
		rdp->nocb_cb_sleep = false;
	pcu_nocb_unlock_irqrestore(rdp, flags);

	/*
	 * Ignore former value of nocb_cb_sleep and force wake up as it could
	 * have been spuriously set to false already.
	 */
	swake_up_one(&rdp->nocb_cb_wq);

	raw_spin_lock_irqsave(&rdp_gp->nocb_gp_lock, flags);
	if (rdp_gp->nocb_gp_sleep) {
		rdp_gp->nocb_gp_sleep = false;
		wake_gp = true;
	}
	raw_spin_unlock_irqrestore(&rdp_gp->nocb_gp_lock, flags);

	if (wake_gp)
		wake_up_process(rdp_gp->nocb_gp_kthread);

	return 0;
}

static long pcu_nocb_rdp_deoffload(void *arg)
{
	struct pcu_data *rdp = arg;
	struct pcu_segcblist *cblist = &rdp->cblist;
	unsigned long flags;
	int ret;

	WARN_ON_ONCE(rdp->cpu != raw_smp_processor_id());

	pr_info("De-offloading %d\n", rdp->cpu);

	pcu_nocb_lock_irqsave(rdp, flags);
	/*
	 * Flush once and for all now. This suffices because we are
	 * running on the target CPU holding ->nocb_lock (thus having
	 * interrupts disabled), and because rdp_offload_toggle()
	 * invokes pcu_segcblist_offload(), which clears SEGCBLIST_OFFLOADED.
	 * Thus future calls to pcu_segcblist_completely_offloaded() will
	 * return false, which means that future calls to pcu_nocb_try_bypass()
	 * will refuse to put anything into the bypass.
	 */
	WARN_ON_ONCE(!pcu_nocb_flush_bypass(rdp, NULL, jiffies));
	ret = rdp_offload_toggle(rdp, false, flags);
	swait_event_exclusive(rdp->nocb_state_wq,
			      !pcu_segcblist_test_flags(cblist, SEGCBLIST_KTHREAD_CB |
							SEGCBLIST_KTHREAD_GP));
	/*
	 * Lock one last time to acquire latest callback updates from kthreads
	 * so we can later handle callbacks locally without locking.
	 */
	pcu_nocb_lock_irqsave(rdp, flags);
	/*
	 * Theoretically we could set SEGCBLIST_SOFTIRQ_ONLY after the nocb
	 * lock is released but how about being paranoid for once?
	 */
	pcu_segcblist_set_flags(cblist, SEGCBLIST_SOFTIRQ_ONLY);
	/*
	 * With SEGCBLIST_SOFTIRQ_ONLY, we can't use
	 * pcu_nocb_unlock_irqrestore() anymore.
	 */
	raw_spin_unlock_irqrestore(&rdp->nocb_lock, flags);

	/* Sanity check */
	WARN_ON_ONCE(pcu_cblist_n_cbs(&rdp->nocb_bypass));


	return ret;
}

int pcu_nocb_cpu_deoffload(int cpu)
{
	struct pcu_data *rdp = per_cpu_ptr(&pcu_data, cpu);
	int ret = 0;

	mutex_lock(&pcu_state.barrier_mutex);
	cpus_read_lock();
	if (pcu_rdp_is_offloaded(rdp)) {
		if (cpu_online(cpu)) {
			ret = work_on_cpu(cpu, pcu_nocb_rdp_deoffload, rdp);
			if (!ret)
				cpumask_clear_cpu(cpu, pcu_nocb_mask);
		} else {
			pr_info("NOCB: Can't CB-deoffload an offline CPU\n");
			ret = -EINVAL;
		}
	}
	cpus_read_unlock();
	mutex_unlock(&pcu_state.barrier_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(pcu_nocb_cpu_deoffload);

static long pcu_nocb_rdp_offload(void *arg)
{
	struct pcu_data *rdp = arg;
	struct pcu_segcblist *cblist = &rdp->cblist;
	unsigned long flags;
	int ret;

	WARN_ON_ONCE(rdp->cpu != raw_smp_processor_id());
	/*
	 * For now we only support re-offload, ie: the rdp must have been
	 * offloaded on boot first.
	 */
	if (!rdp->nocb_gp_rdp)
		return -EINVAL;

	pr_info("Offloading %d\n", rdp->cpu);
	/*
	 * Can't use pcu_nocb_lock_irqsave() while we are in
	 * SEGCBLIST_SOFTIRQ_ONLY mode.
	 */
	raw_spin_lock_irqsave(&rdp->nocb_lock, flags);

	/*
	 * We didn't take the nocb lock while working on the
	 * rdp->cblist in SEGCBLIST_SOFTIRQ_ONLY mode.
	 * Every modifications that have been done previously on
	 * rdp->cblist must be visible remotely by the nocb kthreads
	 * upon wake up after reading the cblist flags.
	 *
	 * The layout against nocb_lock enforces that ordering:
	 *
	 *  __pcu_nocb_rdp_offload()   nocb_cb_wait()/nocb_gp_wait()
	 * -------------------------   ----------------------------
	 *      WRITE callbacks           pcu_nocb_lock()
	 *      pcu_nocb_lock()           READ flags
	 *      WRITE flags               READ callbacks
	 *      pcu_nocb_unlock()         pcu_nocb_unlock()
	 */
	ret = rdp_offload_toggle(rdp, true, flags);
	swait_event_exclusive(rdp->nocb_state_wq,
			      pcu_segcblist_test_flags(cblist, SEGCBLIST_KTHREAD_CB) &&
			      pcu_segcblist_test_flags(cblist, SEGCBLIST_KTHREAD_GP));

	return ret;
}

int pcu_nocb_cpu_offload(int cpu)
{
	struct pcu_data *rdp = per_cpu_ptr(&pcu_data, cpu);
	int ret = 0;

	mutex_lock(&pcu_state.barrier_mutex);
	cpus_read_lock();
	if (!pcu_rdp_is_offloaded(rdp)) {
		if (cpu_online(cpu)) {
			ret = work_on_cpu(cpu, pcu_nocb_rdp_offload, rdp);
			if (!ret)
				cpumask_set_cpu(cpu, pcu_nocb_mask);
		} else {
			pr_info("NOCB: Can't CB-offload an offline CPU\n");
			ret = -EINVAL;
		}
	}
	cpus_read_unlock();
	mutex_unlock(&pcu_state.barrier_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(pcu_nocb_cpu_offload);

void pcu_init_nohz(void)
{
	int cpu;
	bool need_pcu_nocb_mask = false;
	struct pcu_data *rdp;

#if defined(CONFIG_NO_HZ_FULL)
	if (tick_nohz_full_running && cpumask_weight(tick_nohz_full_mask))
		need_pcu_nocb_mask = true;
#endif /* #if defined(CONFIG_NO_HZ_FULL) */

	if (!cpumask_available(pcu_nocb_mask) && need_pcu_nocb_mask) {
		if (!zalloc_cpumask_var(&pcu_nocb_mask, GFP_KERNEL)) {
			pr_info("pcu_nocb_mask allocation failed, callback offloading disabled.\n");
			return;
		}
	}
	if (!cpumask_available(pcu_nocb_mask))
		return;

#if defined(CONFIG_NO_HZ_FULL)
	if (tick_nohz_full_running)
		cpumask_or(pcu_nocb_mask, pcu_nocb_mask, tick_nohz_full_mask);
#endif /* #if defined(CONFIG_NO_HZ_FULL) */

	if (!cpumask_subset(pcu_nocb_mask, cpu_possible_mask)) {
		pr_info("\tNote: kernel parameter 'pcu_nocbs=', 'nohz_full', or 'isolcpus=' contains nonexistent CPUs.\n");
		cpumask_and(pcu_nocb_mask, cpu_possible_mask,
			    pcu_nocb_mask);
	}
	if (cpumask_empty(pcu_nocb_mask))
		pr_info("\tOffload PCU callbacks from CPUs: (none).\n");
	else
		pr_info("\tOffload PCU callbacks from CPUs: %*pbl.\n",
			cpumask_pr_args(pcu_nocb_mask));
	if (pcu_nocb_poll)
		pr_info("\tPoll for callbacks from no-CBs CPUs.\n");

	for_each_cpu(cpu, pcu_nocb_mask) {
		rdp = per_cpu_ptr(&pcu_data, cpu);
		if (pcu_segcblist_empty(&rdp->cblist))
			pcu_segcblist_init(&rdp->cblist);
		pcu_segcblist_offload(&rdp->cblist, true);
		pcu_segcblist_set_flags(&rdp->cblist, SEGCBLIST_KTHREAD_CB);
		pcu_segcblist_set_flags(&rdp->cblist, SEGCBLIST_KTHREAD_GP);
	}
	pcu_organize_nocb_kthreads();
}

/* Initialize per-pcu_data variables for no-CBs CPUs. */
static void pcu_boot_init_nocb_percpu_data(struct pcu_data *rdp)
{
	init_swait_queue_head(&rdp->nocb_cb_wq);
	init_swait_queue_head(&rdp->nocb_gp_wq);
	init_swait_queue_head(&rdp->nocb_state_wq);
	raw_spin_lock_init(&rdp->nocb_lock);
	raw_spin_lock_init(&rdp->nocb_bypass_lock);
	raw_spin_lock_init(&rdp->nocb_gp_lock);
	timer_setup(&rdp->nocb_timer, do_nocb_deferred_wakeup_timer, 0);
	pcu_cblist_init(&rdp->nocb_bypass);
}

/*
 * If the specified CPU is a no-CBs CPU that does not already have its
 * pcuo CB kthread, spawn it.  Additionally, if the pcuo GP kthread
 * for this CPU's group has not yet been created, spawn it as well.
 */
static void pcu_spawn_one_nocb_kthread(int cpu)
{
	struct pcu_data *rdp = per_cpu_ptr(&pcu_data, cpu);
	struct pcu_data *rdp_gp;
	struct task_struct *t;

	/*
	 * If this isn't a no-CBs CPU or if it already has an pcuo kthread,
	 * then nothing to do.
	 */
	if (!pcu_is_nocb_cpu(cpu) || rdp->nocb_cb_kthread)
		return;

	/* If we didn't spawn the GP kthread first, reorganize! */
	rdp_gp = rdp->nocb_gp_rdp;
	if (!rdp_gp->nocb_gp_kthread) {
		t = kthread_run(pcu_nocb_gp_kthread, rdp_gp,
				"pcuog/%d", rdp_gp->cpu);
		if (WARN_ONCE(IS_ERR(t), "%s: Could not start pcuo GP kthread, OOM is now expected behavior\n", __func__))
			return;
		WRITE_ONCE(rdp_gp->nocb_gp_kthread, t);
	}

	/* Spawn the kthread for this CPU. */
	t = kthread_run(pcu_nocb_cb_kthread, rdp,
			"pcuo%c/%d", pcu_state.abbr, cpu);
	if (WARN_ONCE(IS_ERR(t), "%s: Could not start pcuo CB kthread, OOM is now expected behavior\n", __func__))
		return;
	WRITE_ONCE(rdp->nocb_cb_kthread, t);
	WRITE_ONCE(rdp->nocb_gp_kthread, rdp_gp->nocb_gp_kthread);
}

/*
 * If the specified CPU is a no-CBs CPU that does not already have its
 * pcuo kthread, spawn it.
 */
static void pcu_spawn_cpu_nocb_kthread(int cpu)
{
	if (pcu_scheduler_fully_active)
		pcu_spawn_one_nocb_kthread(cpu);
}

/*
 * Once the scheduler is running, spawn pcuo kthreads for all online
 * no-CBs CPUs.  This assumes that the early_initcall()s happen before
 * non-boot CPUs come online -- if this changes, we will need to add
 * some mutual exclusion.
 */
static void pcu_spawn_nocb_kthreads(void)
{
	int cpu;

	for_each_online_cpu(cpu)
		pcu_spawn_cpu_nocb_kthread(cpu);
}

/* How many CB CPU IDs per GP kthread?  Default of -1 for sqrt(nr_cpu_ids). */
static int pcu_nocb_gp_stride = -1;
module_param(pcu_nocb_gp_stride, int, 0444);

/*
 * Initialize GP-CB relationships for all no-CBs CPU.
 */
static void pcu_organize_nocb_kthreads(void)
{
	int cpu;
	bool firsttime = true;
	bool gotnocbs = false;
	bool gotnocbscbs = true;
	int ls = pcu_nocb_gp_stride;
	int nl = 0;  /* Next GP kthread. */
	struct pcu_data *rdp;
	struct pcu_data *rdp_gp = NULL;  /* Suppress misguided gcc warn. */
	struct pcu_data *rdp_prev = NULL;

	if (!cpumask_available(pcu_nocb_mask))
		return;
	if (ls == -1) {
		ls = nr_cpu_ids / int_sqrt(nr_cpu_ids);
		pcu_nocb_gp_stride = ls;
	}

	/*
	 * Each pass through this loop sets up one pcu_data structure.
	 * Should the corresponding CPU come online in the future, then
	 * we will spawn the needed set of pcu_nocb_kthread() kthreads.
	 */
	for_each_cpu(cpu, pcu_nocb_mask) {
		rdp = per_cpu_ptr(&pcu_data, cpu);
		if (rdp->cpu >= nl) {
			/* New GP kthread, set up for CBs & next GP. */
			gotnocbs = true;
			nl = DIV_ROUND_UP(rdp->cpu + 1, ls) * ls;
			rdp->nocb_gp_rdp = rdp;
			rdp_gp = rdp;
			if (dump_tree) {
				if (!firsttime)
					pr_cont("%s\n", gotnocbscbs
							? "" : " (self only)");
				gotnocbscbs = false;
				firsttime = false;
				pr_alert("%s: No-CB GP kthread CPU %d:",
					 __func__, cpu);
			}
		} else {
			/* Another CB kthread, link to previous GP kthread. */
			gotnocbscbs = true;
			rdp->nocb_gp_rdp = rdp_gp;
			rdp_prev->nocb_next_cb_rdp = rdp;
			if (dump_tree)
				pr_cont(" %d", cpu);
		}
		rdp_prev = rdp;
	}
	if (gotnocbs && dump_tree)
		pr_cont("%s\n", gotnocbscbs ? "" : " (self only)");
}

/*
 * Bind the current task to the offloaded CPUs.  If there are no offloaded
 * CPUs, leave the task unbound.  Splat if the bind attempt fails.
 */
void pcu_bind_current_to_nocb(void)
{
	if (cpumask_available(pcu_nocb_mask) && cpumask_weight(pcu_nocb_mask))
		WARN_ON(sched_setaffinity(current->pid, pcu_nocb_mask));
}
EXPORT_SYMBOL_GPL(pcu_bind_current_to_nocb);

// The ->on_cpu field is available only in CONFIG_SMP=y, so...
#ifdef CONFIG_SMP
static char *show_pcu_should_be_on_cpu(struct task_struct *tsp)
{
	return tsp && task_is_running(tsp) && !tsp->on_cpu ? "!" : "";
}
#else // #ifdef CONFIG_SMP
static char *show_pcu_should_be_on_cpu(struct task_struct *tsp)
{
	return "";
}
#endif // #else #ifdef CONFIG_SMP

/*
 * Dump out nocb grace-period kthread state for the specified pcu_data
 * structure.
 */
static void show_pcu_nocb_gp_state(struct pcu_data *rdp)
{
	struct pcu_node *rnp = rdp->mynode;

	pr_info("nocb GP %d %c%c%c%c%c %c[%c%c] %c%c:%ld rnp %d:%d %lu %c CPU %d%s\n",
		rdp->cpu,
		"kK"[!!rdp->nocb_gp_kthread],
		"lL"[raw_spin_is_locked(&rdp->nocb_gp_lock)],
		"dD"[!!rdp->nocb_defer_wakeup],
		"tT"[timer_pending(&rdp->nocb_timer)],
		"sS"[!!rdp->nocb_gp_sleep],
		".W"[swait_active(&rdp->nocb_gp_wq)],
		".W"[swait_active(&rnp->nocb_gp_wq[0])],
		".W"[swait_active(&rnp->nocb_gp_wq[1])],
		".B"[!!rdp->nocb_gp_bypass],
		".G"[!!rdp->nocb_gp_gp],
		(long)rdp->nocb_gp_seq,
		rnp->grplo, rnp->grphi, READ_ONCE(rdp->nocb_gp_loops),
		rdp->nocb_gp_kthread ? task_state_to_char(rdp->nocb_gp_kthread) : '.',
		rdp->nocb_cb_kthread ? (int)task_cpu(rdp->nocb_gp_kthread) : -1,
		show_pcu_should_be_on_cpu(rdp->nocb_cb_kthread));
}

/* Dump out nocb kthread state for the specified pcu_data structure. */
static void show_pcu_nocb_state(struct pcu_data *rdp)
{
	char bufw[20];
	char bufr[20];
	struct pcu_segcblist *rsclp = &rdp->cblist;
	bool waslocked;
	bool wassleep;

	if (rdp->nocb_gp_rdp == rdp)
		show_pcu_nocb_gp_state(rdp);

	sprintf(bufw, "%ld", rsclp->gp_seq[PCU_WAIT_TAIL]);
	sprintf(bufr, "%ld", rsclp->gp_seq[PCU_NEXT_READY_TAIL]);
	pr_info("   CB %d^%d->%d %c%c%c%c%c%c F%ld L%ld C%d %c%c%s%c%s%c%c q%ld %c CPU %d%s\n",
		rdp->cpu, rdp->nocb_gp_rdp->cpu,
		rdp->nocb_next_cb_rdp ? rdp->nocb_next_cb_rdp->cpu : -1,
		"kK"[!!rdp->nocb_cb_kthread],
		"bB"[raw_spin_is_locked(&rdp->nocb_bypass_lock)],
		"cC"[!!atomic_read(&rdp->nocb_lock_contended)],
		"lL"[raw_spin_is_locked(&rdp->nocb_lock)],
		"sS"[!!rdp->nocb_cb_sleep],
		".W"[swait_active(&rdp->nocb_cb_wq)],
		jiffies - rdp->nocb_bypass_first,
		jiffies - rdp->nocb_nobypass_last,
		rdp->nocb_nobypass_count,
		".D"[pcu_segcblist_ready_cbs(rsclp)],
		".W"[!pcu_segcblist_segempty(rsclp, PCU_WAIT_TAIL)],
		pcu_segcblist_segempty(rsclp, PCU_WAIT_TAIL) ? "" : bufw,
		".R"[!pcu_segcblist_segempty(rsclp, PCU_NEXT_READY_TAIL)],
		pcu_segcblist_segempty(rsclp, PCU_NEXT_READY_TAIL) ? "" : bufr,
		".N"[!pcu_segcblist_segempty(rsclp, PCU_NEXT_TAIL)],
		".B"[!!pcu_cblist_n_cbs(&rdp->nocb_bypass)],
		pcu_segcblist_n_cbs(&rdp->cblist),
		rdp->nocb_cb_kthread ? task_state_to_char(rdp->nocb_cb_kthread) : '.',
		rdp->nocb_cb_kthread ? (int)task_cpu(rdp->nocb_gp_kthread) : -1,
		show_pcu_should_be_on_cpu(rdp->nocb_cb_kthread));

	/* It is OK for GP kthreads to have GP state. */
	if (rdp->nocb_gp_rdp == rdp)
		return;

	waslocked = raw_spin_is_locked(&rdp->nocb_gp_lock);
	wassleep = swait_active(&rdp->nocb_gp_wq);
	if (!rdp->nocb_gp_sleep && !waslocked && !wassleep)
		return;  /* Nothing untowards. */

	pr_info("   nocb GP activity on CB-only CPU!!! %c%c%c %c\n",
		"lL"[waslocked],
		"dD"[!!rdp->nocb_defer_wakeup],
		"sS"[!!rdp->nocb_gp_sleep],
		".W"[wassleep]);
}

#else /* #ifdef CONFIG_PCU_NOCB_CPU */

/* No ->nocb_lock to acquire.  */
static void pcu_nocb_lock(struct pcu_data *rdp)
{
}

/* No ->nocb_lock to release.  */
static void pcu_nocb_unlock(struct pcu_data *rdp)
{
}

/* No ->nocb_lock to release.  */
static void pcu_nocb_unlock_irqrestore(struct pcu_data *rdp,
				       unsigned long flags)
{
	local_irq_restore(flags);
}

/* Lockdep check that ->cblist may be safely accessed. */
static void pcu_lockdep_assert_cblist_protected(struct pcu_data *rdp)
{
	lockdep_assert_irqs_disabled();
}

static void pcu_nocb_gp_cleanup(struct swait_queue_head *sq)
{
}

static struct swait_queue_head *pcu_nocb_gp_get(struct pcu_node *rnp)
{
	return NULL;
}

static void pcu_init_one_nocb(struct pcu_node *rnp)
{
}

static bool pcu_nocb_flush_bypass(struct pcu_data *rdp, struct pcu_head *rhp,
				  unsigned long j)
{
	return true;
}

static bool pcu_nocb_try_bypass(struct pcu_data *rdp, struct pcu_head *rhp,
				bool *was_alldone, unsigned long flags)
{
	return false;
}

static void __call_pcu_nocb_wake(struct pcu_data *rdp, bool was_empty,
				 unsigned long flags)
{
	WARN_ON_ONCE(1);  /* Should be dead code! */
}

static void pcu_boot_init_nocb_percpu_data(struct pcu_data *rdp)
{
}

static int pcu_nocb_need_deferred_wakeup(struct pcu_data *rdp, int level)
{
	return false;
}

static bool do_nocb_deferred_wakeup(struct pcu_data *rdp)
{
	return false;
}

static void pcu_spawn_cpu_nocb_kthread(int cpu)
{
}

static void pcu_spawn_nocb_kthreads(void)
{
}

static void show_pcu_nocb_state(struct pcu_data *rdp)
{
}

#endif /* #else #ifdef CONFIG_RCU_NOCB_CPU */

/*
 * Is this CPU a NO_HZ_FULL CPU that should ignore PCU so that the
 * grace-period kthread will do force_quiescent_state() processing?
 * The idea is to avoid waking up PCU core processing on such a
 * CPU unless the grace period has extended for too long.
 *
 * This code relies on the fact that all NO_HZ_FULL CPUs are also
 * CONFIG_PCU_NOCB_CPU CPUs.
 */
static bool pcu_nohz_full_cpu(void)
{
#ifdef CONFIG_NO_HZ_FULL
	if (tick_nohz_full_cpu(smp_processor_id()) &&
	    (!pcu_gp_in_progress() ||
	     time_before(jiffies, READ_ONCE(pcu_state.gp_start) + HZ)))
		return true;
#endif /* #ifdef CONFIG_NO_HZ_FULL */
	return false;
}

/*
 * Bind the PCU grace-period kthreads to the housekeeping CPU.
 */
static void pcu_bind_gp_kthread(void)
{
	if (!tick_nohz_full_enabled())
		return;
	housekeeping_affine(current, HK_FLAG_RCU);
}

/* Record the current task on dyntick-idle entry. */
static __always_inline void pcu_dynticks_task_enter(void)
{
#if defined(CONFIG_TASKS_PCU) && defined(CONFIG_NO_HZ_FULL)
	WRITE_ONCE(current->pcu_tasks_idle_cpu, smp_processor_id());
#endif /* #if defined(CONFIG_TASKS_PCU) && defined(CONFIG_NO_HZ_FULL) */
}

/* Record no current task on dyntick-idle exit. */
static __always_inline void pcu_dynticks_task_exit(void)
{
#if defined(CONFIG_TASKS_PCU) && defined(CONFIG_NO_HZ_FULL)
	WRITE_ONCE(current->pcu_tasks_idle_cpu, -1);
#endif /* #if defined(CONFIG_TASKS_PCU) && defined(CONFIG_NO_HZ_FULL) */
}

/* Turn on heavyweight PCU tasks trace readers on idle/user entry. */
static __always_inline void pcu_dynticks_task_trace_enter(void)
{
#ifdef CONFIG_TASKS_TRACE_PCU
	if (IS_ENABLED(CONFIG_TASKS_TRACE_PCU_READ_MB))
		current->task_struct_rh->trc_reader_special.b.need_mb = true;
#endif /* #ifdef CONFIG_TASKS_TRACE_PCU */
}

/* Turn off heavyweight PCU tasks trace readers on idle/user exit. */
static __always_inline void pcu_dynticks_task_trace_exit(void)
{
#ifdef CONFIG_TASKS_TRACE_PCU
	if (IS_ENABLED(CONFIG_TASKS_TRACE_PCU_READ_MB))
		current->task_struct_rh->trc_reader_special.b.need_mb = false;
#endif /* #ifdef CONFIG_TASKS_TRACE_PCU */
}
