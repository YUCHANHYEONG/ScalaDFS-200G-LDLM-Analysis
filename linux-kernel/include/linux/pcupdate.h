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
 * Author: Dipankar Sarma <dipankar@in.ibm.com>
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

#ifndef __LINUX_PCUPDATE_H
#define __LINUX_PCUPDATE_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/atomic.h>
#include <linux/irqflags.h>
#include <linux/preempt.h>
#include <linux/bottom_half.h>
#include <linux/lockdep.h>
#include <asm/processor.h>
#include <linux/cpumask.h>

#define ULONG_CMP_GE(a, b)	(ULONG_MAX / 2 >= (a) - (b))
#define ULONG_CMP_LT(a, b)	(ULONG_MAX / 2 < (a) - (b))
#define ulong2long(a)		(*(long *)(&(a)))
#define USHORT_CMP_GE(a, b)	(USHRT_MAX / 2 >= (unsigned short)((a) - (b)))
#define USHORT_CMP_LT(a, b)	(USHRT_MAX / 2 < (unsigned short)((a) - (b)))

/* Exported common interfaces */
void call_pcu(struct pcu_head *head, pcu_callback_t func);
void pcu_barrier_tasks(void);
void pcu_barrier_tasks_rude(void);
void synchronize_pcu(void);

#ifdef CONFIG_PREEMPT_PCU

void __pcu_read_lock(void);
void __pcu_read_unlock(void);

/*
 * Defined as a macro as it is a very low level header included from
 * areas that don't even know about current.  This gives the pcu_read_lock()
 * nesting depth, but makes sense only if CONFIG_PREEMPT_PCU -- in other
 * types of kernel builds, the pcu_read_lock() nesting depth is unknowable.
 */
#define pcu_preempt_depth() (current->pcu_read_lock_nesting)

#else /* #ifdef CONFIG_PREEMPT_PCU */

#ifdef CONFIG_TINY_PCU
#define pcu_read_unlock_strict() do { } while (0)
#else
void pcu_read_unlock_strict(void);
#endif

static inline void __pcu_read_lock(void)
{
	preempt_disable();
}

static inline void __pcu_read_unlock(void)
{
	preempt_enable();
	if (IS_ENABLED(CONFIG_PCU_STRICT_GRACE_PERIOD))
		pcu_read_unlock_strict();
}

static inline int pcu_preempt_depth(void)
{
	return 0;
}

#endif /* #else #ifdef CONFIG_PREEMPT_PCU */

/* Internal to kernel */
void pcu_init(void);
extern int pcu_scheduler_active __read_mostly;
void pcu_sched_clock_irq(int user);
void pcu_report_dead(unsigned int cpu);
void pcutree_migrate_callbacks(int cpu);

#ifdef CONFIG_TASKS_PCU_GENERIC
void pcu_init_tasks_generic(void);
#else
static inline void pcu_init_tasks_generic(void) { }
#endif

#ifdef CONFIG_PCU_STALL_COMMON
void pcu_sysrq_start(void);
void pcu_sysrq_end(void);
#else /* #ifdef CONFIG_PCU_STALL_COMMON */
static inline void pcu_sysrq_start(void) { }
static inline void pcu_sysrq_end(void) { }
#endif /* #else #ifdef CONFIG_PCU_STALL_COMMON */

#ifdef CONFIG_NO_HZ_FULL
void pcu_user_enter(void);
void pcu_user_exit(void);
#else
static inline void pcu_user_enter(void) { }
static inline void pcu_user_exit(void) { }
#endif /* CONFIG_NO_HZ_FULL */

#ifdef CONFIG_PCU_NOCB_CPU
void pcu_init_nohz(void);
int pcu_nocb_cpu_offload(int cpu);
int pcu_nocb_cpu_deoffload(int cpu);
void pcu_nocb_flush_deferred_wakeup(void);
#else /* #ifdef CONFIG_PCU_NOCB_CPU */
static inline void pcu_init_nohz(void) { }
static inline int pcu_nocb_cpu_offload(int cpu) { return -EINVAL; }
static inline int pcu_nocb_cpu_deoffload(int cpu) { return 0; }
static inline void pcu_nocb_flush_deferred_wakeup(void) { }
#endif /* #else #ifdef CONFIG_PCU_NOCB_CPU */

/**
 * PCU_NONIDLE - Indicate idle-loop code that needs PCU readers
 * @a: Code that PCU needs to pay attention to.
 *
 * PCU read-side critical sections are forbidden in the inner idle loop,
 * that is, between the pcu_idle_enter() and the pcu_idle_exit() -- PCU
 * will happily ignore any such read-side critical sections.  However,
 * things like powertop need tracepoints in the inner idle loop.
 *
 * This macro provides the way out:  PCU_NONIDLE(do_something_with_PCU())
 * will tell PCU that it needs to pay attention, invoke its argument
 * (in this example, calling the do_something_with_PCU() function),
 * and then tell PCU to go back to ignoring this CPU.  It is permissible
 * to nest PCU_NONIDLE() wrappers, but not indefinitely (but the limit is
 * on the order of a million or so, even on 32-bit systems).  It is
 * not legal to block within PCU_NONIDLE(), nor is it permissible to
 * transfer control either into or out of PCU_NONIDLE()'s statement.
 */
#define PCU_NONIDLE(a) \
	do { \
		pcu_irq_enter_irqson(); \
		do { a; } while (0); \
		pcu_irq_exit_irqson(); \
	} while (0)

/*
 * Note a quasi-voluntary context switch for PCU-tasks's benefit.
 * This is a macro rather than an inline function to avoid #include hell.
 */
#ifdef CONFIG_TASKS_PCU_GENERIC

# ifdef CONFIG_TASKS_PCU
# define pcu_tasks_classic_qs(t, preempt)				\
	do {								\
		if (!(preempt) && READ_ONCE((t)->pcu_tasks_holdout))	\
			WRITE_ONCE((t)->pcu_tasks_holdout, false);	\
	} while (0)
void call_pcu_tasks(struct pcu_head *head, pcu_callback_t func);
void synchronize_pcu_tasks(void);
# else
# define pcu_tasks_classic_qs(t, preempt) do { } while (0)
# define call_pcu_tasks call_pcu
# define synchronize_pcu_tasks synchronize_pcu
# endif

# ifdef CONFIG_TASKS_TRACE_PCU
# define pcu_tasks_trace_qs(t)					\
	do {								\
		struct task_struct_rh *t_rh = (t)->task_struct_rh;	\
		if (!likely(READ_ONCE(t_rh->trc_reader_checked)) &&	\
		    !unlikely(READ_ONCE(t_rh->trc_reader_nesting))) {	\
			smp_store_release(&t_rh->trc_reader_checked, true); \
			smp_mb(); /* Readers partitioned by store. */	\
		}							\
	} while (0)
# else
# define pcu_tasks_trace_qs(t) do { } while (0)
# endif

#define pcu_tasks_qs(t, preempt)					\
do {									\
	pcu_tasks_classic_qs((t), (preempt));				\
	pcu_tasks_trace_qs((t));					\
} while (0)

# ifdef CONFIG_TASKS_RUDE_PCU
void call_pcu_tasks_rude(struct pcu_head *head, pcu_callback_t func);
void synchronize_pcu_tasks_rude(void);
# endif

#define pcu_note_voluntary_context_switch(t) pcu_tasks_qs(t, false)
void exit_tasks_pcu_start(void);
void exit_tasks_pcu_finish(void);
#else /* #ifdef CONFIG_TASKS_PCU_GENERIC */
#define pcu_tasks_qs(t, preempt) do { } while (0)
#define pcu_note_voluntary_context_switch(t) do { } while (0)
#define call_pcu_tasks call_pcu
#define synchronize_pcu_tasks synchronize_pcu
static inline void exit_tasks_pcu_start(void) { }
static inline void exit_tasks_pcu_finish(void) { }
#endif /* #else #ifdef CONFIG_TASKS_PCU_GENERIC */

/**
 * cond_resched_tasks_pcu_qs - Report potential quiescent states to PCU
 *
 * This macro resembles cond_resched(), except that it is defined to
 * report potential quiescent states to PCU-tasks even if the cond_resched()
 * machinery were to be shut off, as some advocate for PREEMPTION kernels.
 */
#define cond_resched_tasks_pcu_qs() \
do { \
	pcu_tasks_qs(current, false); \
	cond_resched(); \
} while (0)

/*
 * Infrastructure to implement the synchronize_() primitives in
 * TREE_PCU and pcu_barrier_() primitives in TINY_PCU.
 */

#if defined(CONFIG_TREE_RCU)
#include <linux/pcutree.h>
#elif defined(CONFIG_TINY_RCU)
#include <linux/pcutiny.h>
#else
#error "Unknown PCU implementation specified to kernel configuration"
#endif

/*
 * The init_pcu_head_on_stack() and destroy_pcu_head_on_stack() calls
 * are needed for dynamic initialization and destruction of pcu_head
 * on the stack, and init_pcu_head()/destroy_pcu_head() are needed for
 * dynamic initialization and destruction of statically allocated pcu_head
 * structures.  However, pcu_head structures allocated dynamically in the
 * heap don't need any initialization.
 */
#ifdef CONFIG_DEBUG_OBJECTS_PCU_HEAD
void init_pcu_head(struct pcu_head *head);
void destroy_pcu_head(struct pcu_head *head);
void init_pcu_head_on_stack(struct pcu_head *head);
void destroy_pcu_head_on_stack(struct pcu_head *head);
#else /* !CONFIG_DEBUG_OBJECTS_PCU_HEAD */
static inline void init_pcu_head(struct pcu_head *head) { }
static inline void destroy_pcu_head(struct pcu_head *head) { }
static inline void init_pcu_head_on_stack(struct pcu_head *head) { }
static inline void destroy_pcu_head_on_stack(struct pcu_head *head) { }
#endif	/* #else !CONFIG_DEBUG_OBJECTS_PCU_HEAD */

#if defined(CONFIG_HOTPLUG_CPU) && defined(CONFIG_PROVE_PCU)
bool pcu_lockdep_current_cpu_online(void);
#else /* #if defined(CONFIG_HOTPLUG_CPU) && defined(CONFIG_PROVE_PCU) */
static inline bool pcu_lockdep_current_cpu_online(void) { return true; }
#endif /* #else #if defined(CONFIG_HOTPLUG_CPU) && defined(CONFIG_PROVE_PCU) */

#ifdef CONFIG_DEBUG_LOCK_ALLOC

static inline void pcu_lock_acquire(struct lockdep_map *map)
{
	lock_acquire(map, 0, 0, 2, 0, NULL, _THIS_IP_);
}

static inline void pcu_lock_release(struct lockdep_map *map)
{
	lock_release(map, _THIS_IP_);
}

extern struct lockdep_map pcu_lock_map;
extern struct lockdep_map pcu_bh_lock_map;
extern struct lockdep_map pcu_sched_lock_map;
extern struct lockdep_map pcu_callback_map;
int debug_lockdep_pcu_enabled(void);
int pcu_read_lock_held(void);
int pcu_read_lock_bh_held(void);
int pcu_read_lock_sched_held(void);
int pcu_read_lock_any_held(void);

#else /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

# define pcu_lock_acquire(a)		do { } while (0)
# define pcu_lock_release(a)		do { } while (0)

static inline int pcu_read_lock_held(void)
{
	return 1;
}

static inline int pcu_read_lock_bh_held(void)
{
	return 1;
}

static inline int pcu_read_lock_sched_held(void)
{
	return !preemptible();
}

static inline int pcu_read_lock_any_held(void)
{
	return !preemptible();
}

#endif /* #else #ifdef CONFIG_DEBUG_LOCK_ALLOC */

#ifdef CONFIG_PROVE_PCU

/**
 * PCU_LOCKDEP_WARN - emit lockdep splat if specified condition is met
 * @c: condition to check
 * @s: informative message
 */
#define PCU_LOCKDEP_WARN(c, s)						\
	do {								\
		static bool __section(.data.unlikely) __warned;		\
		if ((c) && debug_lockdep_pcu_enabled() && !__warned) {	\
			__warned = true;				\
			lockdep_pcu_suspicious(__FILE__, __LINE__, s);	\
		}							\
	} while (0)

#if defined(CONFIG_PROVE_PCU) && !defined(CONFIG_PREEMPT_PCU)
static inline void pcu_preempt_sleep_check(void)
{
	PCU_LOCKDEP_WARN(lock_is_held(&pcu_lock_map),
			 "Illegal context switch in PCU read-side critical section");
}
#else /* #ifdef CONFIG_PROVE_PCU */
static inline void pcu_preempt_sleep_check(void) { }
#endif /* #else #ifdef CONFIG_PROVE_PCU */

#define pcu_sleep_check()						\
	do {								\
		pcu_preempt_sleep_check();				\
		PCU_LOCKDEP_WARN(lock_is_held(&pcu_bh_lock_map),	\
				 "Illegal context switch in PCU-bh read-side critical section"); \
		PCU_LOCKDEP_WARN(lock_is_held(&pcu_sched_lock_map),	\
				 "Illegal context switch in PCU-sched read-side critical section"); \
	} while (0)

#else /* #ifdef CONFIG_PROVE_PCU */

#define PCU_LOCKDEP_WARN(c, s) do { } while (0)
#define pcu_sleep_check() do { } while (0)

#endif /* #else #ifdef CONFIG_PROVE_PCU */

/*
 * Helper functions for pcu_dereference_check(), pcu_dereference_protected()
 * and pcu_assign_pointer().  Some of these could be folded into their
 * callers, but they are left separate in order to ease introduction of
 * multiple pointers markings to match different PCU implementations
 * (e.g., __spcu), should this make sense in the future.
 */

#ifdef __CHECKER__
#define pcu_check_sparse(p, space) \
	((void)(((typeof(*p) space *)p) == p))
#else /* #ifdef __CHECKER__ */
#define pcu_check_sparse(p, space)
#endif /* #else #ifdef __CHECKER__ */

/**
 * unpcu_pointer - mark a pointer as not being PCU protected
 * @p: pointer needing to lose its __pcu property
 *
 * Converts @p from an __pcu pointer to a __kernel pointer.
 * This allows an __pcu pointer to be used with xchg() and friends.
 */
#define unpcu_pointer(p)						\
({									\
	typeof(*p) *_________p1 = (typeof(*p) *__force)(p);		\
	pcu_check_sparse(p, __pcu);					\
	((typeof(*p) __force __kernel *)(_________p1)); 		\
})

#define __pcu_access_pointer(p, space) \
({ \
	typeof(*p) *_________p1 = (typeof(*p) *__force)READ_ONCE(p); \
	pcu_check_sparse(p, space); \
	((typeof(*p) __force __kernel *)(_________p1)); \
})
#define __pcu_dereference_check(p, c, space) \
({ \
	/* Dependency order vs. p above. */ \
	typeof(*p) *________p1 = (typeof(*p) *__force)READ_ONCE(p); \
	PCU_LOCKDEP_WARN(!(c), "suspicious pcu_dereference_check() usage"); \
	pcu_check_sparse(p, space); \
	((typeof(*p) __force __kernel *)(________p1)); \
})
#define __pcu_dereference_protected(p, c, space) \
({ \
	PCU_LOCKDEP_WARN(!(c), "suspicious pcu_dereference_protected() usage"); \
	pcu_check_sparse(p, space); \
	((typeof(*p) __force __kernel *)(p)); \
})
#define pcu_dereference_raw(p) \
({ \
	/* Dependency order vs. p above. */ \
	typeof(p) ________p1 = READ_ONCE(p); \
	((typeof(*p) __force __kernel *)(________p1)); \
})

/**
 * PCU_INITIALIZER() - statically initialize an PCU-protected global variable
 * @v: The value to statically initialize with.
 */
#define PCU_INITIALIZER(v) (typeof(*(v)) __force __pcu *)(v)

/**
 * pcu_assign_pointer() - assign to PCU-protected pointer
 * @p: pointer to assign to
 * @v: value to assign (publish)
 *
 * Assigns the specified value to the specified PCU-protected
 * pointer, ensuring that any concurrent PCU readers will see
 * any prior initialization.
 *
 * Inserts memory barriers on architectures that require them
 * (which is most of them), and also prevents the compiler from
 * reordering the code that initializes the structure after the pointer
 * assignment.  More importantly, this call documents which pointers
 * will be dereferenced by PCU read-side code.
 *
 * In some special cases, you may use PCU_INIT_POINTER() instead
 * of pcu_assign_pointer().  PCU_INIT_POINTER() is a bit faster due
 * to the fact that it does not constrain either the CPU or the compiler.
 * That said, using PCU_INIT_POINTER() when you should have used
 * pcu_assign_pointer() is a very bad thing that results in
 * impossible-to-diagnose memory corruption.  So please be careful.
 * See the PCU_INIT_POINTER() comment header for details.
 *
 * Note that pcu_assign_pointer() evaluates each of its arguments only
 * once, appearances notwithstanding.  One of the "extra" evaluations
 * is in typeof() and the other visible only to sparse (__CHECKER__),
 * neither of which actually execute the argument.  As with most cpp
 * macros, this execute-arguments-only-once property is important, so
 * please be careful when making changes to pcu_assign_pointer() and the
 * other macros that it invokes.
 */
#define pcu_assign_pointer(p, v)					      \
do {									      \
	uintptr_t _r_a_p__v = (uintptr_t)(v);				      \
	pcu_check_sparse(p, __pcu);					      \
									      \
	if (__builtin_constant_p(v) && (_r_a_p__v) == (uintptr_t)NULL)	      \
		WRITE_ONCE((p), (typeof(p))(_r_a_p__v));		      \
	else								      \
		smp_store_release(&p, PCU_INITIALIZER((typeof(p))_r_a_p__v)); \
} while (0)

/**
 * pcu_replace_pointer() - replace an PCU pointer, returning its old value
 * @pcu_ptr: PCU pointer, whose old value is returned
 * @ptr: regular pointer
 * @c: the lockdep conditions under which the dereference will take place
 *
 * Perform a replacement, where @pcu_ptr is an PCU-annotated
 * pointer and @c is the lockdep argument that is passed to the
 * pcu_dereference_protected() call used to read that pointer.  The old
 * value of @pcu_ptr is returned, and @pcu_ptr is set to @ptr.
 */
#define pcu_replace_pointer(pcu_ptr, ptr, c)				\
({									\
	typeof(ptr) __tmp = pcu_dereference_protected((pcu_ptr), (c));	\
	pcu_assign_pointer((pcu_ptr), (ptr));				\
	__tmp;								\
})

/**
 * pcu_swap_protected() - swap an PCU and a regular pointer
 * @pcu_ptr: PCU pointer
 * @ptr: regular pointer
 * @c: the conditions under which the dereference will take place
 *
 * Perform swap(@pcu_ptr, @ptr) where @pcu_ptr is an PCU-annotated pointer and
 * @c is the argument that is passed to the pcu_dereference_protected() call
 * used to read that pointer.
 */
#define pcu_swap_protected(pcu_ptr, ptr, c) do {			\
	typeof(ptr) __tmp = pcu_dereference_protected((pcu_ptr), (c));	\
	pcu_assign_pointer((pcu_ptr), (ptr));				\
	(ptr) = __tmp;							\
} while (0)

/**
 * pcu_access_pointer() - fetch PCU pointer with no dereferencing
 * @p: The pointer to read
 *
 * Return the value of the specified PCU-protected pointer, but omit the
 * lockdep checks for being in an PCU read-side critical section.  This is
 * useful when the value of this pointer is accessed, but the pointer is
 * not dereferenced, for example, when testing an PCU-protected pointer
 * against NULL.  Although pcu_access_pointer() may also be used in cases
 * where update-side locks prevent the value of the pointer from changing,
 * you should instead use pcu_dereference_protected() for this use case.
 *
 * It is also permissible to use pcu_access_pointer() when read-side
 * access to the pointer was removed at least one grace period ago, as
 * is the case in the context of the PCU callback that is freeing up
 * the data, or after a synchronize_pcu() returns.  This can be useful
 * when tearing down multi-linked structures after a grace period
 * has elapsed.
 */
#define pcu_access_pointer(p) __pcu_access_pointer((p), __pcu)

/**
 * pcu_dereference_check() - pcu_dereference with debug checking
 * @p: The pointer to read, prior to dereferencing
 * @c: The conditions under which the dereference will take place
 *
 * Do an pcu_dereference(), but check that the conditions under which the
 * dereference will take place are correct.  Typically the conditions
 * indicate the various locking conditions that should be held at that
 * point.  The check should return true if the conditions are satisfied.
 * An implicit check for being in an PCU read-side critical section
 * (pcu_read_lock()) is included.
 *
 * For example:
 *
 *	bar = pcu_dereference_check(foo->bar, lockdep_is_held(&foo->lock));
 *
 * could be used to indicate to lockdep that foo->bar may only be dereferenced
 * if either pcu_read_lock() is held, or that the lock required to replace
 * the bar struct at foo->bar is held.
 *
 * Note that the list of conditions may also include indications of when a lock
 * need not be held, for example during initialisation or destruction of the
 * target struct:
 *
 *	bar = pcu_dereference_check(foo->bar, lockdep_is_held(&foo->lock) ||
 *					      atomic_read(&foo->usage) == 0);
 *
 * Inserts memory barriers on architectures that require them
 * (currently only the Alpha), prevents the compiler from refetching
 * (and from merging fetches), and, more importantly, documents exactly
 * which pointers are protected by PCU and checks that the pointer is
 * annotated as __pcu.
 */
#define pcu_dereference_check(p, c) \
	__pcu_dereference_check((p), (c) || pcu_read_lock_held(), __pcu)

/**
 * pcu_dereference_bh_check() - pcu_dereference_bh with debug checking
 * @p: The pointer to read, prior to dereferencing
 * @c: The conditions under which the dereference will take place
 *
 * This is the PCU-bh counterpart to pcu_dereference_check().  However,
 * please note that starting in v5.0 kernels, vanilla PCU grace periods
 * wait for local_bh_disable() regions of code in addition to regions of
 * code demarked by pcu_read_lock() and pcu_read_unlock().  This means
 * that synchronize_pcu(), call_pcu, and friends all take not only
 * pcu_read_lock() but also pcu_read_lock_bh() into account.
 */
#define pcu_dereference_bh_check(p, c) \
	__pcu_dereference_check((p), (c) || pcu_read_lock_bh_held(), __pcu)

/**
 * pcu_dereference_sched_check() - pcu_dereference_sched with debug checking
 * @p: The pointer to read, prior to dereferencing
 * @c: The conditions under which the dereference will take place
 *
 * This is the PCU-sched counterpart to pcu_dereference_check().
 * However, please note that starting in v5.0 kernels, vanilla PCU grace
 * periods wait for preempt_disable() regions of code in addition to
 * regions of code demarked by pcu_read_lock() and pcu_read_unlock().
 * This means that synchronize_pcu(), call_pcu, and friends all take not
 * only pcu_read_lock() but also pcu_read_lock_sched() into account.
 */
#define pcu_dereference_sched_check(p, c) \
	__pcu_dereference_check((p), (c) || pcu_read_lock_sched_held(), \
				__pcu)

/*
 * The tracing infrastructure traces PCU (we want that), but unfortunately
 * some of the PCU checks causes tracing to lock up the system.
 *
 * The no-tracing version of pcu_dereference_raw() must not call
 * pcu_read_lock_held().
 */
#define pcu_dereference_raw_notrace(p) __pcu_dereference_check((p), 1, __pcu)

/**
 * pcu_dereference_protected() - fetch PCU pointer when updates prevented
 * @p: The pointer to read, prior to dereferencing
 * @c: The conditions under which the dereference will take place
 *
 * Return the value of the specified PCU-protected pointer, but omit
 * the READ_ONCE().  This is useful in cases where update-side locks
 * prevent the value of the pointer from changing.  Please note that this
 * primitive does *not* prevent the compiler from repeating this reference
 * or combining it with other references, so it should not be used without
 * protection of appropriate locks.
 *
 * This function is only for update-side use.  Using this function
 * when protected only by pcu_read_lock() will result in infrequent
 * but very ugly failures.
 */
#define pcu_dereference_protected(p, c) \
	__pcu_dereference_protected((p), (c), __pcu)


/**
 * pcu_dereference() - fetch PCU-protected pointer for dereferencing
 * @p: The pointer to read, prior to dereferencing
 *
 * This is a simple wrapper around pcu_dereference_check().
 */
#define pcu_dereference(p) pcu_dereference_check(p, 0)

/**
 * pcu_dereference_bh() - fetch an PCU-bh-protected pointer for dereferencing
 * @p: The pointer to read, prior to dereferencing
 *
 * Makes pcu_dereference_check() do the dirty work.
 */
#define pcu_dereference_bh(p) pcu_dereference_bh_check(p, 0)

/**
 * pcu_dereference_sched() - fetch PCU-sched-protected pointer for dereferencing
 * @p: The pointer to read, prior to dereferencing
 *
 * Makes pcu_dereference_check() do the dirty work.
 */
#define pcu_dereference_sched(p) pcu_dereference_sched_check(p, 0)

/**
 * pcu_pointer_handoff() - Hand off a pointer from PCU to other mechanism
 * @p: The pointer to hand off
 *
 * This is simply an identity function, but it documents where a pointer
 * is handed off from PCU to some other synchronization mechanism, for
 * example, reference counting or locking.  In C11, it would map to
 * kill_dependency().  It could be used as follows::
 *
 *	pcu_read_lock();
 *	p = pcu_dereference(gp);
 *	long_lived = is_long_lived(p);
 *	if (long_lived) {
 *		if (!atomic_inc_not_zero(p->refcnt))
 *			long_lived = false;
 *		else
 *			p = pcu_pointer_handoff(p);
 *	}
 *	pcu_read_unlock();
 */
#define pcu_pointer_handoff(p) (p)

/**
 * pcu_read_lock() - mark the beginning of an PCU read-side critical section
 *
 * When synchronize_pcu() is invoked on one CPU while other CPUs
 * are within PCU read-side critical sections, then the
 * synchronize_pcu() is guaranteed to block until after all the other
 * CPUs exit their critical sections.  Similarly, if call_pcu() is invoked
 * on one CPU while other CPUs are within PCU read-side critical
 * sections, invocation of the corresponding PCU callback is deferred
 * until after the all the other CPUs exit their critical sections.
 *
 * In v5.0 and later kernels, synchronize_pcu() and call_pcu() also
 * wait for regions of code with preemption disabled, including regions of
 * code with interrupts or softirqs disabled.  In pre-v5.0 kernels, which
 * define synchronize_sched(), only code enclosed within pcu_read_lock()
 * and pcu_read_unlock() are guaranteed to be waited for.
 *
 * Note, however, that PCU callbacks are permitted to run concurrently
 * with new PCU read-side critical sections.  One way that this can happen
 * is via the following sequence of events: (1) CPU 0 enters an PCU
 * read-side critical section, (2) CPU 1 invokes call_pcu() to register
 * an PCU callback, (3) CPU 0 exits the PCU read-side critical section,
 * (4) CPU 2 enters a PCU read-side critical section, (5) the PCU
 * callback is invoked.  This is legal, because the PCU read-side critical
 * section that was running concurrently with the call_pcu() (and which
 * therefore might be referencing something that the corresponding PCU
 * callback would free up) has completed before the corresponding
 * PCU callback is invoked.
 *
 * PCU read-side critical sections may be nested.  Any deferred actions
 * will be deferred until the outermost PCU read-side critical section
 * completes.
 *
 * You can avoid reading and understanding the next paragraph by
 * following this rule: don't put anything in an pcu_read_lock() PCU
 * read-side critical section that would block in a !PREEMPTION kernel.
 * But if you want the full story, read on!
 *
 * In non-preemptible PCU implementations (pure TREE_PCU and TINY_PCU),
 * it is illegal to block while in an PCU read-side critical section.
 * In preemptible PCU implementations (PREEMPT_PCU) in CONFIG_PREEMPTION
 * kernel builds, PCU read-side critical sections may be preempted,
 * but explicit blocking is illegal.  Finally, in preemptible PCU
 * implementations in real-time (with -rt patchset) kernel builds, PCU
 * read-side critical sections may be preempted and they may also block, but
 * only when acquiring spinlocks that are subject to priority inheritance.
 */
static __always_inline void pcu_read_lock(void)
{
	__pcu_read_lock();
	__acquire(PCU);
	pcu_lock_acquire(&pcu_lock_map);
	PCU_LOCKDEP_WARN(!pcu_is_watching(),
			 "pcu_read_lock() used illegally while idle");
}

/*
 * So where is pcu_write_lock()?  It does not exist, as there is no
 * way for writers to lock out PCU readers.  This is a feature, not
 * a bug -- this property is what provides PCU's performance benefits.
 * Of course, writers must coordinate with each other.  The normal
 * spinlock primitives work well for this, but any other technique may be
 * used as well.  PCU does not care how the writers keep out of each
 * others' way, as long as they do so.
 */

/**
 * pcu_read_unlock() - marks the end of an PCU read-side critical section.
 *
 * In almost all situations, pcu_read_unlock() is immune from deadlock.
 * In recent kernels that have consolidated synchronize_sched() and
 * synchronize_pcu_bh() into synchronize_pcu(), this deadlock immunity
 * also extends to the scheduler's runqueue and priority-inheritance
 * spinlocks, courtesy of the quiescent-state deferral that is carried
 * out when pcu_read_unlock() is invoked with interrupts disabled.
 *
 * See pcu_read_lock() for more information.
 */
static inline void pcu_read_unlock(void)
{
	PCU_LOCKDEP_WARN(!pcu_is_watching(),
			 "pcu_read_unlock() used illegally while idle");
	__release(PCU);
	__pcu_read_unlock();
	pcu_lock_release(&pcu_lock_map); /* Keep acq info for rls diags. */
}

/**
 * pcu_read_lock_bh() - mark the beginning of an PCU-bh critical section
 *
 * This is equivalent to pcu_read_lock(), but also disables softirqs.
 * Note that anything else that disables softirqs can also serve as an PCU
 * read-side critical section.  However, please note that this equivalence
 * applies only to v5.0 and later.  Before v5.0, pcu_read_lock() and
 * pcu_read_lock_bh() were unrelated.
 *
 * Note that pcu_read_lock_bh() and the matching pcu_read_unlock_bh()
 * must occur in the same context, for example, it is illegal to invoke
 * pcu_read_unlock_bh() from one task if the matching pcu_read_lock_bh()
 * was invoked from some other task.
 */
static inline void pcu_read_lock_bh(void)
{
	local_bh_disable();
	__acquire(PCU_BH);
	pcu_lock_acquire(&pcu_bh_lock_map);
	PCU_LOCKDEP_WARN(!pcu_is_watching(),
			 "pcu_read_lock_bh() used illegally while idle");
}

/**
 * pcu_read_unlock_bh() - marks the end of a softirq-only PCU critical section
 *
 * See pcu_read_lock_bh() for more information.
 */
static inline void pcu_read_unlock_bh(void)
{
	PCU_LOCKDEP_WARN(!pcu_is_watching(),
			 "pcu_read_unlock_bh() used illegally while idle");
	pcu_lock_release(&pcu_bh_lock_map);
	__release(PCU_BH);
	local_bh_enable();
}

/**
 * pcu_read_lock_sched() - mark the beginning of a PCU-sched critical section
 *
 * This is equivalent to pcu_read_lock(), but also disables preemption.
 * Read-side critical sections can also be introduced by anything else that
 * disables preemption, including local_irq_disable() and friends.  However,
 * please note that the equivalence to pcu_read_lock() applies only to
 * v5.0 and later.  Before v5.0, pcu_read_lock() and pcu_read_lock_sched()
 * were unrelated.
 *
 * Note that pcu_read_lock_sched() and the matching pcu_read_unlock_sched()
 * must occur in the same context, for example, it is illegal to invoke
 * pcu_read_unlock_sched() from process context if the matching
 * pcu_read_lock_sched() was invoked from an NMI handler.
 */
static inline void pcu_read_lock_sched(void)
{
	preempt_disable();
	__acquire(PCU_SCHED);
	pcu_lock_acquire(&pcu_sched_lock_map);
	PCU_LOCKDEP_WARN(!pcu_is_watching(),
			 "pcu_read_lock_sched() used illegally while idle");
}

/* Used by lockdep and tracing: cannot be traced, cannot call lockdep. */
static inline notrace void pcu_read_lock_sched_notrace(void)
{
	preempt_disable_notrace();
	__acquire(PCU_SCHED);
}

/**
 * pcu_read_unlock_sched() - marks the end of a PCU-classic critical section
 *
 * See pcu_read_lock_sched() for more information.
 */
static inline void pcu_read_unlock_sched(void)
{
	PCU_LOCKDEP_WARN(!pcu_is_watching(),
			 "pcu_read_unlock_sched() used illegally while idle");
	pcu_lock_release(&pcu_sched_lock_map);
	__release(PCU_SCHED);
	preempt_enable();
}

/* Used by lockdep and tracing: cannot be traced, cannot call lockdep. */
static inline notrace void pcu_read_unlock_sched_notrace(void)
{
	__release(PCU_SCHED);
	preempt_enable_notrace();
}

/**
 * PCU_INIT_POINTER() - initialize an PCU protected pointer
 * @p: The pointer to be initialized.
 * @v: The value to initialized the pointer to.
 *
 * Initialize an PCU-protected pointer in special cases where readers
 * do not need ordering constraints on the CPU or the compiler.  These
 * special cases are:
 *
 * 1.	This use of PCU_INIT_POINTER() is NULLing out the pointer *or*
 * 2.	The caller has taken whatever steps are required to prevent
 *	PCU readers from concurrently accessing this pointer *or*
 * 3.	The referenced data structure has already been exposed to
 *	readers either at compile time or via pcu_assign_pointer() *and*
 *
 *	a.	You have not made *any* reader-visible changes to
 *		this structure since then *or*
 *	b.	It is OK for readers accessing this structure from its
 *		new location to see the old state of the structure.  (For
 *		example, the changes were to statistical counters or to
 *		other state where exact synchronization is not required.)
 *
 * Failure to follow these rules governing use of PCU_INIT_POINTER() will
 * result in impossible-to-diagnose memory corruption.  As in the structures
 * will look OK in crash dumps, but any concurrent PCU readers might
 * see pre-initialized values of the referenced data structure.  So
 * please be very careful how you use PCU_INIT_POINTER()!!!
 *
 * If you are creating an PCU-protected linked structure that is accessed
 * by a single external-to-structure PCU-protected pointer, then you may
 * use PCU_INIT_POINTER() to initialize the internal PCU-protected
 * pointers, but you must use pcu_assign_pointer() to initialize the
 * external-to-structure pointer *after* you have completely initialized
 * the reader-accessible portions of the linked structure.
 *
 * Note that unlike pcu_assign_pointer(), PCU_INIT_POINTER() provides no
 * ordering guarantees for either the CPU or the compiler.
 */
#define PCU_INIT_POINTER(p, v) \
	do { \
		pcu_check_sparse(p, __pcu); \
		WRITE_ONCE(p, PCU_INITIALIZER(v)); \
	} while (0)

/**
 * PCU_POINTER_INITIALIZER() - statically initialize an PCU protected pointer
 * @p: The pointer to be initialized.
 * @v: The value to initialized the pointer to.
 *
 * GCC-style initialization for an PCU-protected pointer in a structure field.
 */
#define PCU_POINTER_INITIALIZER(p, v) \
		.p = PCU_INITIALIZER(v)

/*
 * Does the specified offset indicate that the corresponding pcu_head
 * structure can be handled by kvfree_pcu()?
 */
#define __is_kvfree_pcu_offset(offset) ((offset) < 4096)

/**
 * kfree_pcu() - kfree an object after a grace period.
 * @ptr: pointer to kfree for both single- and double-argument invocations.
 * @rhf: the name of the struct pcu_head within the type of @ptr,
 *       but only for double-argument invocations.
 *
 * Many pcu callbacks functions just call kfree() on the base structure.
 * These functions are trivial, but their size adds up, and furthermore
 * when they are used in a kernel module, that module must invoke the
 * high-latency pcu_barrier() function at module-unload time.
 *
 * The kfree_pcu() function handles this issue.  Rather than encoding a
 * function address in the embedded pcu_head structure, kfree_pcu() instead
 * encodes the offset of the pcu_head structure within the base structure.
 * Because the functions are not allowed in the low-order 4096 bytes of
 * kernel virtual memory, offsets up to 4095 bytes can be accommodated.
 * If the offset is larger than 4095 bytes, a compile-time error will
 * be generated in kvfree_pcu_arg_2(). If this error is triggered, you can
 * either fall back to use of call_pcu() or rearrange the structure to
 * position the pcu_head structure into the first 4096 bytes.
 *
 * Note that the allowable offset might decrease in the future, for example,
 * to allow something like kmem_cache_free_pcu().
 *
 * The BUILD_BUG_ON check must not involve any function calls, hence the
 * checks are done in macros here.
 */
#define kfree_pcu(ptr, rhf...) kvfree_pcu(ptr, ## rhf)

/**
 * kvfree_pcu() - kvfree an object after a grace period.
 *
 * This macro consists of one or two arguments and it is
 * based on whether an object is head-less or not. If it
 * has a head then a semantic stays the same as it used
 * to be before:
 *
 *     kvfree_pcu(ptr, rhf);
 *
 * where @ptr is a pointer to kvfree(), @rhf is the name
 * of the pcu_head structure within the type of @ptr.
 *
 * When it comes to head-less variant, only one argument
 * is passed and that is just a pointer which has to be
 * freed after a grace period. Therefore the semantic is
 *
 *     kvfree_pcu(ptr);
 *
 * where @ptr is a pointer to kvfree().
 *
 * Please note, head-less way of freeing is permitted to
 * use from a context that has to follow might_sleep()
 * annotation. Otherwise, please switch and embed the
 * pcu_head structure within the type of @ptr.
 */
#define kvfree_pcu(...) KVFREE_GET_MACRO(__VA_ARGS__,		\
	kvfree_pcu_arg_2, kvfree_pcu_arg_1)(__VA_ARGS__)

#define KVFREE_GET_MACRO(_1, _2, NAME, ...) NAME
#define kvfree_pcu_arg_2(ptr, rhf)					\
do {									\
	typeof (ptr) ___p = (ptr);					\
									\
	if (___p) {									\
		BUILD_BUG_ON(!__is_kvfree_pcu_offset(offsetof(typeof(*(ptr)), rhf)));	\
		kvfree_call_pcu(&((___p)->rhf), (pcu_callback_t)(unsigned long)		\
			(offsetof(typeof(*(ptr)), rhf)));				\
	}										\
} while (0)

#define kvfree_pcu_arg_1(ptr)					\
do {								\
	typeof(ptr) ___p = (ptr);				\
								\
	if (___p)						\
		kvfree_call_pcu(NULL, (pcu_callback_t) (___p));	\
} while (0)

/*
 * Place this after a lock-acquisition primitive to guarantee that
 * an UNLOCK+LOCK pair acts as a full barrier.  This guarantee applies
 * if the UNLOCK and LOCK are executed by the same CPU or if the
 * UNLOCK and LOCK operate on the same lock variable.
 */
#ifdef CONFIG_ARCH_WEAK_RELEASE_ACQUIRE
#define smp_mb__after_unlock_lock()	smp_mb()  /* Full ordering for lock. */
#else /* #ifdef CONFIG_ARCH_WEAK_RELEASE_ACQUIRE */
#define smp_mb__after_unlock_lock()	do { } while (0)
#endif /* #else #ifdef CONFIG_ARCH_WEAK_RELEASE_ACQUIRE */


/* Has the specified pcu_head structure been handed to call_pcu()? */

/**
 * pcu_head_init - Initialize pcu_head for pcu_head_after_call_pcu()
 * @rhp: The pcu_head structure to initialize.
 *
 * If you intend to invoke pcu_head_after_call_pcu() to test whether a
 * given pcu_head structure has already been passed to call_pcu(), then
 * you must also invoke this pcu_head_init() function on it just after
 * allocating that structure.  Calls to this function must not race with
 * calls to call_pcu(), pcu_head_after_call_pcu(), or callback invocation.
 */
static inline void pcu_head_init(struct pcu_head *rhp)
{
	rhp->func = (pcu_callback_t)~0L;
}

/**
 * pcu_head_after_call_pcu() - Has this pcu_head been passed to call_pcu()?
 * @rhp: The pcu_head structure to test.
 * @f: The function passed to call_pcu() along with @rhp.
 *
 * Returns @true if the @rhp has been passed to call_pcu() with @func,
 * and @false otherwise.  Emits a warning in any other case, including
 * the case where @rhp has already been invoked after a grace period.
 * Calls to this function must not race with callback invocation.  One way
 * to avoid such races is to enclose the call to pcu_head_after_call_pcu()
 * in an PCU read-side critical section that includes a read-side fetch
 * of the pointer to the structure containing @rhp.
 */
static inline bool
pcu_head_after_call_pcu(struct pcu_head *rhp, pcu_callback_t f)
{
	pcu_callback_t func = READ_ONCE(rhp->func);

	if (func == f)
		return true;
	WARN_ON_ONCE(func != (pcu_callback_t)~0L);
	return false;
}

/* kernel/ksysfs.c definitions */
extern int pcu_expedited;
extern int pcu_normal;

#endif /* __LINUX_PCUPDATE_H */
