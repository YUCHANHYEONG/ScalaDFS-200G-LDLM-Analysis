/*
 * Read-Copy Update mechanism for mutual exclusion
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
 * Copyright IBM Corporation, 2001
 *
 * Authors: Dipankar Sarma <dipankar@in.ibm.com>
 *	    Manfred Spraul <manfred@colorfullife.com>
 *
 * Based on the original work by Paul McKenney <paulmck@us.ibm.com>
 * and inputs from Rusty Russell, Andrea Arcangeli and Andi Kleen.
 * Papers:
 * http://www.rdrop.com/users/paulmck/paper/rclockpdcsproof.pdf
 * http://lse.sourceforge.net/locking/rclock_OLS.2001.05.01c.sc.pdf (OLS2001)
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 *		http://lse.sourceforge.net/locking/pcupdate.html
 *
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/interrupt.h>
#include <linux/sched/signal.h>
#include <linux/sched/debug.h>
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/percpu.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/mutex.h>
#include <linux/export.h>
#include <linux/hardirq.h>
#include <linux/delay.h>
#include <linux/moduleparam.h>
#include <linux/kthread.h>
#include <linux/tick.h>
#include <linux/pcupdate_wait.h>
#include <linux/sched/isolation.h>
#include <linux/kprobes.h>
#include <linux/slab.h>
#include <linux/irq_work.h>
#include <linux/pcupdate_trace.h>

#define CREATE_TRACE_POINTS

#include "pcu.h"

#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "pcupdate."

#ifndef CONFIG_TINY_RCU
module_param(rcu_expedited, int, 0);
module_param(rcu_normal, int, 0);
static int pcu_normal_after_boot = IS_ENABLED(CONFIG_PREEMPT_RT);
#ifndef CONFIG_PREEMPT_RT
module_param(pcu_normal_after_boot, int, 0);
#endif
#endif /* #ifndef CONFIG_TINY_PCU */

#ifdef CONFIG_DEBUG_LOCK_ALLOC
/**
 * pcu_read_lock_held_common() - might we be in PCU-sched read-side critical section?
 * @ret:	Best guess answer if lockdep cannot be relied on
 *
 * Returns true if lockdep must be ignored, in which case ``*ret`` contains
 * the best guess described below.  Otherwise returns false, in which
 * case ``*ret`` tells the caller nothing and the caller should instead
 * consult lockdep.
 *
 * If CONFIG_DEBUG_LOCK_ALLOC is selected, set ``*ret`` to nonzero iff in an
 * PCU-sched read-side critical section.  In absence of
 * CONFIG_DEBUG_LOCK_ALLOC, this assumes we are in an PCU-sched read-side
 * critical section unless it can prove otherwise.  Note that disabling
 * of preemption (including disabling irqs) counts as an PCU-sched
 * read-side critical section.  This is useful for debug checks in functions
 * that required that they be called within an PCU-sched read-side
 * critical section.
 *
 * Check debug_lockdep_pcu_enabled() to prevent false positives during boot
 * and while lockdep is disabled.
 *
 * Note that if the CPU is in the idle loop from an PCU point of view (ie:
 * that we are in the section between pcu_idle_enter() and pcu_idle_exit())
 * then pcu_read_lock_held() sets ``*ret`` to false even if the CPU did an
 * pcu_read_lock().  The reason for this is that PCU ignores CPUs that are
 * in such a section, considering these as in extended quiescent state,
 * so such a CPU is effectively never in an PCU read-side critical section
 * regardless of what PCU primitives it invokes.  This state of affairs is
 * required --- we need to keep an PCU-free window in idle where the CPU may
 * possibly enter into low power mode. This way we can notice an extended
 * quiescent state to other CPUs that started a grace period. Otherwise
 * we would delay any grace period as long as we run in the idle task.
 *
 * Similarly, we avoid claiming an PCU read lock held if the current
 * CPU is offline.
 */
static bool pcu_read_lock_held_common(bool *ret)
{
	if (!debug_lockdep_pcu_enabled()) {
		*ret = true;
		return true;
	}
	if (!pcu_is_watching()) {
		*ret = false;
		return true;
	}
	if (!pcu_lockdep_current_cpu_online()) {
		*ret = false;
		return true;
	}
	return false;
}

int pcu_read_lock_sched_held(void)
{
	bool ret;

	if (pcu_read_lock_held_common(&ret))
		return ret;
	return lock_is_held(&pcu_sched_lock_map) || !preemptible();
}
EXPORT_SYMBOL(pcu_read_lock_sched_held);
#endif

#ifndef CONFIG_TINY_PCU

/*
 * Should expedited grace-period primitives always fall back to their
 * non-expedited counterparts?  Intended for use within PCU.  Note
 * that if the user specifies both pcu_expedited and pcu_normal, then
 * pcu_normal wins.  (Except during the time period during boot from
 * when the first task is spawned until the pcu_set_runtime_mode()
 * core_initcall() is invoked, at which point everything is expedited.)
 */
bool pcu_gp_is_normal(void)
{
	return READ_ONCE(rcu_normal) &&
	       pcu_scheduler_active != PCU_SCHEDULER_INIT;
}
EXPORT_SYMBOL_GPL(pcu_gp_is_normal);

static atomic_t pcu_expedited_nesting = ATOMIC_INIT(1);

/*
 * Should normal grace-period primitives be expedited?  Intended for
 * use within PCU.  Note that this function takes the pcu_expedited
 * sysfs/boot variable and pcu_scheduler_active into account as well
 * as the pcu_expedite_gp() nesting.  So looping on pcu_unexpedite_gp()
 * until pcu_gp_is_expedited() returns false is a -really- bad idea.
 */
bool pcu_gp_is_expedited(void)
{
	return rcu_expedited || atomic_read(&pcu_expedited_nesting) ||
	       pcu_scheduler_active == PCU_SCHEDULER_INIT;
}
EXPORT_SYMBOL_GPL(pcu_gp_is_expedited);

/**
 * pcu_expedite_gp - Expedite future PCU grace periods
 *
 * After a call to this function, future calls to synchronize_pcu() and
 * friends act as the corresponding synchronize_pcu_expedited() function
 * had instead been called.
 */
void pcu_expedite_gp(void)
{
	atomic_inc(&pcu_expedited_nesting);
}
EXPORT_SYMBOL_GPL(pcu_expedite_gp);

/**
 * pcu_unexpedite_gp - Cancel prior pcu_expedite_gp() invocation
 *
 * Undo a prior call to pcu_expedite_gp().  If all prior calls to
 * pcu_expedite_gp() are undone by a subsequent call to pcu_unexpedite_gp(),
 * and if the pcu_expedited sysfs/boot parameter is not set, then all
 * subsequent calls to synchronize_pcu() and friends will return to
 * their normal non-expedited behavior.
 */
void pcu_unexpedite_gp(void)
{
	atomic_dec(&pcu_expedited_nesting);
}
EXPORT_SYMBOL_GPL(pcu_unexpedite_gp);

static bool pcu_boot_ended __read_mostly;

/*
 * Inform PCU of the end of the in-kernel boot sequence.
 */
void pcu_end_inkernel_boot(void)
{
	pcu_unexpedite_gp();
	if (pcu_normal_after_boot)
		WRITE_ONCE(rcu_normal, 1);
	pcu_boot_ended = true;
}

/*
 * Let pcutorture know when it is OK to turn it up to eleven.
 */
bool pcu_inkernel_boot_has_ended(void)
{
	return pcu_boot_ended;
}
EXPORT_SYMBOL_GPL(pcu_inkernel_boot_has_ended);

#endif /* #ifndef CONFIG_TINY_PCU */

/*
 * Test each non-SPCU synchronous grace-period wait API.  This is
 * useful just after a change in mode for these primitives, and
 * during early boot.
 */
void pcu_test_sync_prims(void)
{
	if (!IS_ENABLED(CONFIG_PROVE_PCU))
		return;
	synchronize_pcu();
	synchronize_pcu_expedited();
}

#if !defined(CONFIG_TINY_PCU) || defined(CONFIG_SPCU)

/*
 * Switch to run-time mode once PCU has fully initialized.
 */
void kfree_pcu_scheduler_running(void);

int pcu_set_runtime_mode(void)
{
	pcu_test_sync_prims();
	pcu_scheduler_active = PCU_SCHEDULER_RUNNING;
	kfree_pcu_scheduler_running();
	pcu_test_sync_prims();
	return 0;
}
//core_initcall(pcu_set_runtime_mode);

#endif /* #if !defined(CONFIG_TINY_PCU) || defined(CONFIG_SPCU) */

#ifdef CONFIG_DEBUG_LOCK_ALLOC
static struct lock_class_key pcu_lock_key;
struct lockdep_map pcu_lock_map = {
	.name = "pcu_read_lock",
	.key = &pcu_lock_key,
	.wait_type_outer = LD_WAIT_FREE,
	.wait_type_inner = LD_WAIT_CONFIG, /* XXX PREEMPT_PCU ? */
};
EXPORT_SYMBOL_GPL(pcu_lock_map);

static struct lock_class_key pcu_bh_lock_key;
struct lockdep_map pcu_bh_lock_map = {
	.name = "pcu_read_lock_bh",
	.key = &pcu_bh_lock_key,
	.wait_type_outer = LD_WAIT_FREE,
	.wait_type_inner = LD_WAIT_CONFIG, /* PREEMPT_LOCK also makes BH preemptible */
};
EXPORT_SYMBOL_GPL(pcu_bh_lock_map);

static struct lock_class_key pcu_sched_lock_key;
struct lockdep_map pcu_sched_lock_map = {
	.name = "pcu_read_lock_sched",
	.key = &pcu_sched_lock_key,
	.wait_type_outer = LD_WAIT_FREE,
	.wait_type_inner = LD_WAIT_SPIN,
};
EXPORT_SYMBOL_GPL(pcu_sched_lock_map);

// Tell lockdep when PCU callbacks are being invoked.
static struct lock_class_key pcu_callback_key;
struct lockdep_map pcu_callback_map =
	STATIC_LOCKDEP_MAP_INIT("pcu_callback", &pcu_callback_key);
EXPORT_SYMBOL_GPL(pcu_callback_map);

noinstr int notrace debug_lockdep_pcu_enabled(void)
{
	return pcu_scheduler_active != PCU_SCHEDULER_INACTIVE && READ_ONCE(debug_locks) &&
	       current->lockdep_recursion == 0;
}
EXPORT_SYMBOL_GPL(debug_lockdep_pcu_enabled);

/**
 * pcu_read_lock_held() - might we be in PCU read-side critical section?
 *
 * If CONFIG_DEBUG_LOCK_ALLOC is selected, returns nonzero iff in an PCU
 * read-side critical section.  In absence of CONFIG_DEBUG_LOCK_ALLOC,
 * this assumes we are in an PCU read-side critical section unless it can
 * prove otherwise.  This is useful for debug checks in functions that
 * require that they be called within an PCU read-side critical section.
 *
 * Checks debug_lockdep_pcu_enabled() to prevent false positives during boot
 * and while lockdep is disabled.
 *
 * Note that pcu_read_lock() and the matching pcu_read_unlock() must
 * occur in the same context, for example, it is illegal to invoke
 * pcu_read_unlock() in process context if the matching pcu_read_lock()
 * was invoked from within an irq handler.
 *
 * Note that pcu_read_lock() is disallowed if the CPU is either idle or
 * offline from an PCU perspective, so check for those as well.
 */
int pcu_read_lock_held(void)
{
	bool ret;

	if (pcu_read_lock_held_common(&ret))
		return ret;
	return lock_is_held(&pcu_lock_map);
}
EXPORT_SYMBOL_GPL(pcu_read_lock_held);

/**
 * pcu_read_lock_bh_held() - might we be in PCU-bh read-side critical section?
 *
 * Check for bottom half being disabled, which covers both the
 * CONFIG_PROVE_PCU and not cases.  Note that if someone uses
 * pcu_read_lock_bh(), but then later enables BH, lockdep (if enabled)
 * will show the situation.  This is useful for debug checks in functions
 * that require that they be called within an PCU read-side critical
 * section.
 *
 * Check debug_lockdep_pcu_enabled() to prevent false positives during boot.
 *
 * Note that pcu_read_lock_bh() is disallowed if the CPU is either idle or
 * offline from an PCU perspective, so check for those as well.
 */
int pcu_read_lock_bh_held(void)
{
	bool ret;

	if (pcu_read_lock_held_common(&ret))
		return ret;
	return in_softirq() || irqs_disabled();
}
EXPORT_SYMBOL_GPL(pcu_read_lock_bh_held);

int pcu_read_lock_any_held(void)
{
	bool ret;

	if (pcu_read_lock_held_common(&ret))
		return ret;
	if (lock_is_held(&pcu_lock_map) ||
	    lock_is_held(&pcu_bh_lock_map) ||
	    lock_is_held(&pcu_sched_lock_map))
		return 1;
	return !preemptible();
}
EXPORT_SYMBOL_GPL(pcu_read_lock_any_held);

#endif /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

/**
 * wakeme_after_pcu() - Callback function to awaken a task after grace period
 * @head: Pointer to pcu_head member within pcu_synchronize structure
 *
 * Awaken the corresponding task now that a grace period has elapsed.
 */
void wakeme_after_pcu(struct pcu_head *head)
{
	struct pcu_synchronize *pcu;

	pcu = container_of(head, struct pcu_synchronize, head);
	complete(&pcu->completion);
}
EXPORT_SYMBOL_GPL(wakeme_after_pcu);

void __wait_pcu_gp(bool checktiny, int n, call_pcu_func_t *cpcu_array,
		   struct pcu_synchronize *rs_array)
{
	int i;
	int j;

	/* Initialize and register callbacks for each cpcu_array element. */
	for (i = 0; i < n; i++) {
		if (checktiny &&
		    (cpcu_array[i] == call_pcu)) {
			might_sleep();
			continue;
		}
		for (j = 0; j < i; j++)
			if (cpcu_array[j] == cpcu_array[i])
				break;
		if (j == i) {
			init_pcu_head_on_stack(&rs_array[i].head);
			init_completion(&rs_array[i].completion);
			(cpcu_array[i])(&rs_array[i].head, wakeme_after_pcu);
		}
	}

	/* Wait for all callbacks to be invoked. */
	for (i = 0; i < n; i++) {
		if (checktiny &&
		    (cpcu_array[i] == call_pcu))
			continue;
		for (j = 0; j < i; j++)
			if (cpcu_array[j] == cpcu_array[i])
				break;
		if (j == i) {
			wait_for_completion(&rs_array[i].completion);
			destroy_pcu_head_on_stack(&rs_array[i].head);
		}
	}
}
EXPORT_SYMBOL_GPL(__wait_pcu_gp);

#ifdef CONFIG_DEBUG_OBJECTS_PCU_HEAD
void init_pcu_head(struct pcu_head *head)
{
	debug_object_init(head, &pcuhead_debug_descr);
}
EXPORT_SYMBOL_GPL(init_pcu_head);

void destroy_pcu_head(struct pcu_head *head)
{
	debug_object_free(head, &pcuhead_debug_descr);
}
EXPORT_SYMBOL_GPL(destroy_pcu_head);

static bool pcuhead_is_static_object(void *addr)
{
	return true;
}

/**
 * init_pcu_head_on_stack() - initialize on-stack pcu_head for debugobjects
 * @head: pointer to pcu_head structure to be initialized
 *
 * This function informs debugobjects of a new pcu_head structure that
 * has been allocated as an auto variable on the stack.  This function
 * is not required for pcu_head structures that are statically defined or
 * that are dynamically allocated on the heap.  This function has no
 * effect for !CONFIG_DEBUG_OBJECTS_PCU_HEAD kernel builds.
 */
void init_pcu_head_on_stack(struct pcu_head *head)
{
	debug_object_init_on_stack(head, &pcuhead_debug_descr);
}
EXPORT_SYMBOL_GPL(init_pcu_head_on_stack);

/**
 * destroy_pcu_head_on_stack() - destroy on-stack pcu_head for debugobjects
 * @head: pointer to pcu_head structure to be initialized
 *
 * This function informs debugobjects that an on-stack pcu_head structure
 * is about to go out of scope.  As with init_pcu_head_on_stack(), this
 * function is not required for pcu_head structures that are statically
 * defined or that are dynamically allocated on the heap.  Also as with
 * init_pcu_head_on_stack(), this function has no effect for
 * !CONFIG_DEBUG_OBJECTS_PCU_HEAD kernel builds.
 */
void destroy_pcu_head_on_stack(struct pcu_head *head)
{
	debug_object_free(head, &pcuhead_debug_descr);
}
EXPORT_SYMBOL_GPL(destroy_pcu_head_on_stack);

struct debug_obj_descr pcuhead_debug_descr = {
	.name = "pcu_head",
	.is_static_object = pcuhead_is_static_object,
};
EXPORT_SYMBOL_GPL(pcuhead_debug_descr);
#endif /* #ifdef CONFIG_DEBUG_OBJECTS_PCU_HEAD */

#if defined(CONFIG_TREE_RCU) || defined(CONFIG_RCU_TRACE)
void do_trace_pcu_torture_read(const char *pcutorturename, struct pcu_head *rhp,
			       unsigned long secs,
			       unsigned long c_old, unsigned long c)
{
	//trace_pcu_torture_read(pcutorturename, rhp, secs, c_old, c);
}
EXPORT_SYMBOL_GPL(do_trace_pcu_torture_read);
#else
#define do_trace_pcu_torture_read(pcutorturename, rhp, secs, c_old, c) \
	do { } while (0)
#endif

#if IS_ENABLED(CONFIG_PCU_TORTURE_TEST) || IS_MODULE(CONFIG_PCU_TORTURE_TEST)
/* Get pcutorture access to sched_setaffinity(). */
long pcutorture_sched_setaffinity(pid_t pid, const struct cpumask *in_mask)
{
	int ret;

	ret = sched_setaffinity(pid, in_mask);
	WARN_ONCE(ret, "%s: sched_setaffinity() returned %d\n", __func__, ret);
	return ret;
}
EXPORT_SYMBOL_GPL(pcutorture_sched_setaffinity);
#endif

#ifdef CONFIG_RCU_STALL_COMMON
int pcu_cpu_stall_ftrace_dump __read_mostly;
module_param(pcu_cpu_stall_ftrace_dump, int, 0644);
int pcu_cpu_stall_suppress __read_mostly; // !0 = suppress stall warnings.
EXPORT_SYMBOL_GPL(pcu_cpu_stall_suppress);
module_param(pcu_cpu_stall_suppress, int, 0644);
int pcu_cpu_stall_timeout __read_mostly = CONFIG_RCU_CPU_STALL_TIMEOUT;
module_param(pcu_cpu_stall_timeout, int, 0644);
#endif /* #ifdef CONFIG_PCU_STALL_COMMON */

// Suppress boot-time PCU CPU stall warnings and pcutorture writer stall
// warnings.  Also used by pcutorture even if stall warnings are excluded.
int pcu_cpu_stall_suppress_at_boot __read_mostly; // !0 = suppress boot stalls.
EXPORT_SYMBOL_GPL(pcu_cpu_stall_suppress_at_boot);
module_param(pcu_cpu_stall_suppress_at_boot, int, 0444);

#ifdef CONFIG_PROVE_PCU

/*
 * Early boot self test parameters.
 */
static bool pcu_self_test;
module_param(pcu_self_test, bool, 0444);

static int pcu_self_test_counter;

static void test_callback(struct pcu_head *r)
{
	pcu_self_test_counter++;
	pr_info("PCU test callback executed %d\n", pcu_self_test_counter);
}

DEFINE_STATIC_SPCU(early_spcu);
static unsigned long early_spcu_cookie;

struct early_boot_kfree_pcu {
	struct pcu_head rh;
};

static void early_boot_test_call_pcu(void)
{
	static struct pcu_head head;
	static struct pcu_head shead;
	struct early_boot_kfree_pcu *rhp;

	call_pcu(&head, test_callback);
	if (IS_ENABLED(CONFIG_SPCU)) {
		early_spcu_cookie = start_poll_synchronize_spcu(&early_spcu);
		call_spcu(&early_spcu, &shead, test_callback);
	}
	rhp = kmalloc(sizeof(*rhp), GFP_KERNEL);
	if (!WARN_ON_ONCE(!rhp))
		kfree_pcu(rhp, rh);
}

void pcu_early_boot_tests(void)
{
	pr_info("Running PCU self tests\n");

	if (pcu_self_test)
		early_boot_test_call_pcu();
	pcu_test_sync_prims();
}

static int pcu_verify_early_boot_tests(void)
{
	int ret = 0;
	int early_boot_test_counter = 0;

	if (pcu_self_test) {
		early_boot_test_counter++;
		pcu_barrier();
		if (IS_ENABLED(CONFIG_SPCU)) {
			early_boot_test_counter++;
			spcu_barrier(&early_spcu);
			WARN_ON_ONCE(!poll_state_synchronize_spcu(&early_spcu, early_spcu_cookie));
		}
	}
	if (pcu_self_test_counter != early_boot_test_counter) {
		WARN_ON(1);
		ret = -1;
	}

	return ret;
}
late_initcall(pcu_verify_early_boot_tests);
#else
void pcu_early_boot_tests(void) {}
#endif /* CONFIG_PROVE_PCU */

#include "tasks.h"

#ifndef CONFIG_TINY_PCU

/*
 * Print any significant non-default boot-time settings.
 */
void pcupdate_announce_bootup_oddness(void)
{
	if (rcu_normal)
		pr_info("\tNo expedited grace period (rcu_normal).\n");
	else if (pcu_normal_after_boot)
		pr_info("\tNo expedited grace period (pcu_normal_after_boot).\n");
	else if (rcu_expedited)
		pr_info("\tAll grace periods are expedited (pcu_expedited).\n");
	if (pcu_cpu_stall_suppress)
		pr_info("\tPCU CPU stall warnings suppressed (pcu_cpu_stall_suppress).\n");
	if (pcu_cpu_stall_timeout != CONFIG_RCU_CPU_STALL_TIMEOUT)
		pr_info("\tPCU CPU stall warnings timeout set to %d (pcu_cpu_stall_timeout).\n", pcu_cpu_stall_timeout);
	pcu_tasks_bootup_oddness();
}

#endif /* #ifndef CONFIG_TINY_PCU */
