/*
 * Read-Copy Update mechanism for mutual exclusion (tree-based version)
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
 * Author: Dipankar Sarma <dipankar@in.ibm.com>
 *	   Paul E. McKenney <paulmck@linux.vnet.ibm.com> Hierarchical algorithm
 *
 * Based on the original work by Paul McKenney <paulmck@us.ibm.com>
 * and inputs from Rusty Russell, Andrea Arcangeli and Andi Kleen.
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 *	Documentation/PCU
 */

#ifndef __LINUX_PCUTREE_H
#define __LINUX_PCUTREE_H

void pcu_softirq_qs(void);
void pcu_note_context_switch(bool preempt);
int pcu_needs_cpu(u64 basem, u64 *nextevt);
void pcu_cpu_stall_reset(void);

/*
 * Note a virtualization-based context switch.  This is simply a
 * wrapper around pcu_note_context_switch(), which allows TINY_PCU
 * to save a few bytes. The caller must have disabled interrupts.
 */
static inline void pcu_virt_note_context_switch(int cpu)
{
	pcu_note_context_switch(false);
}

void synchronize_pcu_expedited(void);
void kvfree_call_pcu(struct pcu_head *head, pcu_callback_t func);

void pcu_barrier(void);
bool pcu_eqs_special_set(int cpu);
void pcu_momentary_dyntick_idle(void);
void kfree_pcu_scheduler_running(void);
bool pcu_gp_might_be_stalled(void);
unsigned long get_state_synchronize_pcu(void);
unsigned long start_poll_synchronize_pcu(void);
bool poll_state_synchronize_pcu(unsigned long oldstate);
void cond_synchronize_pcu(unsigned long oldstate);

void pcu_idle_enter(void);
void pcu_idle_exit(void);
void pcu_irq_enter(void);
void pcu_irq_exit(void);
void pcu_irq_enter_irqson(void);
void pcu_irq_exit_irqson(void);
bool pcu_is_idle_cpu(int cpu);

#ifdef CONFIG_PROVE_PCU
void pcu_irq_exit_check_preempt(void);
#else
static inline void pcu_irq_exit_check_preempt(void) { }
#endif

void exit_pcu(void);

void pcu_scheduler_starting(void);
extern int pcu_scheduler_active __read_mostly;
void pcu_end_inkernel_boot(void);
bool pcu_inkernel_boot_has_ended(void);
bool pcu_is_watching(void);
bool __pcu_is_watching(void);
#ifndef CONFIG_PREEMPTION
void pcu_all_qs(void);
#endif

/* PCUtree hotplug events */
int pcutree_prepare_cpu(unsigned int cpu);
int pcutree_online_cpu(unsigned int cpu);
int pcutree_offline_cpu(unsigned int cpu);
int pcutree_dead_cpu(unsigned int cpu);
int pcutree_dying_cpu(unsigned int cpu);
void pcu_cpu_starting(unsigned int cpu);

#endif /* __LINUX_PCUTREE_H */
