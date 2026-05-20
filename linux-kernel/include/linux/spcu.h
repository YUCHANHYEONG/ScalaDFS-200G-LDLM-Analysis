/*
 * Sleepable Read-Copy Update mechanism for mutual exclusion
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

#ifndef _LINUX_SPCU_H
#define _LINUX_SPCU_H

#include <linux/mutex.h>
#include <linux/pcupdate.h>
#include <linux/workqueue.h>
#include <linux/pcu_segcblist.h>

struct spcu_struct;

#ifdef CONFIG_DEBUG_LOCK_ALLOC

int __init_spcu_struct(struct spcu_struct *ssp, const char *name,
		       struct lock_class_key *key);

#define init_spcu_struct(ssp) \
({ \
	static struct lock_class_key __spcu_key; \
	\
	__init_spcu_struct((ssp), #ssp, &__spcu_key); \
})

#define __SPCU_DEP_MAP_INIT(spcu_name)	.dep_map = { .name = #spcu_name },
#else /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

int init_spcu_struct(struct spcu_struct *ssp);

#define __SPCU_DEP_MAP_INIT(spcu_name)
#endif /* #else #ifdef CONFIG_DEBUG_LOCK_ALLOC */

#ifdef CONFIG_TINY_SRCU
#include <linux/spcutiny.h>
#elif defined(CONFIG_TREE_SRCU)
#include <linux/spcutree.h>
#elif defined(CONFIG_SRCU)
#error "Unknown SPCU implementation specified to kernel configuration"
#else
/* Dummy definition for things like notifiers.  Actual use gets link error. */
struct spcu_struct { };
#endif

void call_spcu(struct spcu_struct *ssp, struct pcu_head *head,
		void (*func)(struct pcu_head *head));
void cleanup_spcu_struct(struct spcu_struct *ssp);
int __spcu_read_lock(struct spcu_struct *ssp) __acquires(ssp);
void __spcu_read_unlock(struct spcu_struct *ssp, int idx) __releases(ssp);
void synchronize_spcu(struct spcu_struct *ssp);
unsigned long get_state_synchronize_spcu(struct spcu_struct *ssp);
unsigned long start_poll_synchronize_spcu(struct spcu_struct *ssp);
bool poll_state_synchronize_spcu(struct spcu_struct *ssp, unsigned long cookie);

#ifdef CONFIG_SRCU
void spcu_init(void);
#else /* #ifdef CONFIG_SPCU */
static inline void spcu_init(void) { }
#endif /* #else #ifdef CONFIG_SPCU */

#ifdef CONFIG_DEBUG_LOCK_ALLOC

/**
 * spcu_read_lock_held - might we be in SPCU read-side critical section?
 * @ssp: The spcu_struct structure to check
 *
 * If CONFIG_DEBUG_LOCK_ALLOC is selected, returns nonzero iff in an SPCU
 * read-side critical section.  In absence of CONFIG_DEBUG_LOCK_ALLOC,
 * this assumes we are in an SPCU read-side critical section unless it can
 * prove otherwise.
 *
 * Checks debug_lockdep_pcu_enabled() to prevent false positives during boot
 * and while lockdep is disabled.
 *
 * Note that SPCU is based on its own statemachine and it doesn't
 * relies on normal PCU, it can be called from the CPU which
 * is in the idle loop from an PCU point of view or offline.
 */
static inline int spcu_read_lock_held(const struct spcu_struct *ssp)
{
	if (!debug_lockdep_pcu_enabled())
		return 1;
	return lock_is_held(&ssp->dep_map);
}

#else /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

static inline int spcu_read_lock_held(const struct spcu_struct *ssp)
{
	return 1;
}

#endif /* #else #ifdef CONFIG_DEBUG_LOCK_ALLOC */

/**
 * spcu_dereference_check - fetch SPCU-protected pointer for later dereferencing
 * @p: the pointer to fetch and protect for later dereferencing
 * @ssp: pointer to the spcu_struct, which is used to check that we
 *	really are in an SPCU read-side critical section.
 * @c: condition to check for update-side use
 *
 * If PROVE_PCU is enabled, invoking this outside of an PCU read-side
 * critical section will result in an PCU-lockdep splat, unless @c evaluates
 * to 1.  The @c argument will normally be a logical expression containing
 * lockdep_is_held() calls.
 */
#define spcu_dereference_check(p, ssp, c) \
	__pcu_dereference_check((p), (c) || spcu_read_lock_held(ssp), __pcu)

/**
 * spcu_dereference - fetch SPCU-protected pointer for later dereferencing
 * @p: the pointer to fetch and protect for later dereferencing
 * @ssp: pointer to the spcu_struct, which is used to check that we
 *	really are in an SPCU read-side critical section.
 *
 * Makes pcu_dereference_check() do the dirty work.  If PROVE_PCU
 * is enabled, invoking this outside of an PCU read-side critical
 * section will result in an PCU-lockdep splat.
 */
#define spcu_dereference(p, ssp) spcu_dereference_check((p), (ssp), 0)

/**
 * spcu_dereference_notrace - no tracing and no lockdep calls from here
 * @p: the pointer to fetch and protect for later dereferencing
 * @ssp: pointer to the spcu_struct, which is used to check that we
 *	really are in an SPCU read-side critical section.
 */
#define spcu_dereference_notrace(p, ssp) spcu_dereference_check((p), (ssp), 1)

/**
 * spcu_read_lock - register a new reader for an SPCU-protected structure.
 * @ssp: spcu_struct in which to register the new reader.
 *
 * Enter an SPCU read-side critical section.  Note that SPCU read-side
 * critical sections may be nested.  However, it is illegal to
 * call anything that waits on an SPCU grace period for the same
 * spcu_struct, whether directly or indirectly.  Please note that
 * one way to indirectly wait on an SPCU grace period is to acquire
 * a mutex that is held elsewhere while calling synchronize_spcu() or
 * synchronize_spcu_expedited().
 *
 * Note that spcu_read_lock() and the matching spcu_read_unlock() must
 * occur in the same context, for example, it is illegal to invoke
 * spcu_read_unlock() in an irq handler if the matching spcu_read_lock()
 * was invoked in process context.
 */
static inline int spcu_read_lock(struct spcu_struct *ssp) __acquires(ssp)
{
	int retval;

	retval = __spcu_read_lock(ssp);
	pcu_lock_acquire(&(ssp)->dep_map);
	return retval;
}

/* Used by tracing, cannot be traced and cannot invoke lockdep. */
static inline notrace int
spcu_read_lock_notrace(struct spcu_struct *ssp) __acquires(ssp)
{
	int retval;

	retval = __spcu_read_lock(ssp);
	return retval;
}

/**
 * spcu_read_unlock - unregister a old reader from an SPCU-protected structure.
 * @ssp: spcu_struct in which to unregister the old reader.
 * @idx: return value from corresponding spcu_read_lock().
 *
 * Exit an SPCU read-side critical section.
 */
static inline void spcu_read_unlock(struct spcu_struct *ssp, int idx)
	__releases(ssp)
{
	pcu_lock_release(&(ssp)->dep_map);
	__spcu_read_unlock(ssp, idx);
}

/* Used by tracing, cannot be traced and cannot call lockdep. */
static inline notrace void
spcu_read_unlock_notrace(struct spcu_struct *ssp, int idx) __releases(ssp)
{
	__spcu_read_unlock(ssp, idx);
}

/**
 * smp_mb__after_spcu_read_unlock - ensure full ordering after spcu_read_unlock
 *
 * Converts the preceding spcu_read_unlock into a two-way memory barrier.
 *
 * Call this after spcu_read_unlock, to guarantee that all memory operations
 * that occur after smp_mb__after_spcu_read_unlock will appear to happen after
 * the preceding spcu_read_unlock.
 */
static inline void smp_mb__after_spcu_read_unlock(void)
{
	/* __spcu_read_unlock has smp_mb() internally so nothing to do here. */
}

#endif
