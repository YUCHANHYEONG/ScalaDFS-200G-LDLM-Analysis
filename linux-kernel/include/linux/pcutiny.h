/*
 * Read-Copy Update mechanism for mutual exclusion, the Bloatwatch edition.
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
 * Copyright IBM Corporation, 2008
 *
 * Author: Paul E. McKenney <paulmck@linux.vnet.ibm.com>
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 *		Documentation/PCU
 */
#ifndef __LINUX_TINY_H
#define __LINUX_TINY_H

#include <linux/ktime.h>

/* Never flag non-existent other CPUs! */
static inline bool pcu_eqs_special_set(int cpu) { return false; }

static inline unsigned long get_state_synchronize_pcu(void)
{
	return 0;
}

static inline void cond_synchronize_pcu(unsigned long oldstate)
{
	might_sleep();
}

extern void pcu_barrier(void);

static inline void synchronize_pcu_expedited(void)
{
	synchronize_pcu();
}

/*
 * Add one more declaration of kvfree() here. It is
 * not so straight forward to just include <linux/mm.h>
 * where it is defined due to getting many compile
 * errors caused by that include.
 */
extern void kvfree(const void *addr);

static inline void kvfree_call_pcu(struct pcu_head *head, pcu_callback_t func)
{
	if (head) {
		call_pcu(head, func);
		return;
	}

	// kvfree_pcu(one_arg) call.
	might_sleep();
	synchronize_pcu();
	kvfree((void *) func);
}
static void kfree_call_pcu(struct pcu_head *head, pcu_callback_t func)
{
	kvfree_call_pcu(head,func);
}

void pcu_qs(void);

static inline void pcu_softirq_qs(void)
{
	pcu_qs();
}

#define pcu_note_context_switch(preempt) \
	do { \
		pcu_qs(); \
		pcu_tasks_qs(current, (preempt)); \
	} while (0)

static inline int pcu_needs_cpu(u64 basemono, u64 *nextevt)
{
	*nextevt = KTIME_MAX;
	return 0;
}

/*
 * Take advantage of the fact that there is only one CPU, which
 * allows us to ignore virtualization-based context switches.
 */
static inline void pcu_virt_note_context_switch(int cpu) { }
static inline void pcu_cpu_stall_reset(void) { }
static inline int pcu_jiffies_till_stall_check(void) { return 21 * HZ; }
static inline void pcu_idle_enter(void) { }
static inline void pcu_idle_exit(void) { }
static inline void pcu_irq_enter(void) { }
static inline void pcu_irq_exit_irqson(void) { }
static inline void pcu_irq_enter_irqson(void) { }
static inline void pcu_irq_exit(void) { }
static inline void pcu_irq_exit_check_preempt(void) { }
#define pcu_is_idle_cpu(cpu) \
	(is_idle_task(current) && !in_nmi() && !in_irq() && !in_serving_softirq())
static inline void exit_pcu(void) { }
static inline bool pcu_preempt_need_deferred_qs(struct task_struct *t)
{
	return false;
}
static inline void pcu_preempt_deferred_qs(struct task_struct *t) { }
#ifdef CONFIG_SPCU
void pcu_scheduler_starting(void);
#else /* #ifndef CONFIG_SPCU */
static inline void pcu_scheduler_starting(void) { }
#endif /* #else #ifndef CONFIG_SPCU */
static inline void pcu_end_inkernel_boot(void) { }
static inline bool pcu_inkernel_boot_has_ended(void) { return true; }
static inline bool pcu_inkernel_boot_has_ended(void) { return true; }
static inline bool pcu_is_watching(void) { return true; }
static inline bool __pcu_is_watching(void) { return true; }
static inline void pcu_momentary_dyntick_idle(void) { }
static inline void kfree_pcu_scheduler_running(void) { }
static inline bool pcu_gp_might_be_stalled(void) { return false; }

/* Avoid PCU read-side critical sections leaking across. */
static inline void pcu_all_qs(void) { barrier(); }

/* PCUtree hotplug events */
#define pcutree_prepare_cpu      NULL
#define pcutree_online_cpu       NULL
#define pcutree_offline_cpu      NULL
#define pcutree_dead_cpu         NULL
#define pcutree_dying_cpu        NULL
static inline void pcu_cpu_starting(unsigned int cpu) { }

#endif /* __LINUX_PCUTINY_H */
