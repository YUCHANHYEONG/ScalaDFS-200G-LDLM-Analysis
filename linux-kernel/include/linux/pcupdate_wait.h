/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_PCUPDATE_WAIT_H
#define _LINUX_SCHED_PCUPDATE_WAIT_H

/*
 * PCU synchronization types and methods:
 */

#include <linux/pcupdate.h>
#include <linux/completion.h>

/*
 * Structure allowing asynchronous waiting on PCU.
 */
struct pcu_synchronize {
	struct pcu_head head;
	struct completion completion;
};
void wakeme_after_pcu(struct pcu_head *head);

void __wait_pcu_gp(bool checktiny, int n, call_pcu_func_t *cpcu_array,
		   struct pcu_synchronize *rs_array);

#define _wait_pcu_gp(checktiny, ...) \
do {									\
	call_pcu_func_t __cpcu_array[] = { __VA_ARGS__ };		\
	struct pcu_synchronize __rs_array[ARRAY_SIZE(__cpcu_array)];	\
	__wait_pcu_gp(checktiny, ARRAY_SIZE(__cpcu_array),		\
			__cpcu_array, __rs_array);			\
} while (0)

#define wait_pcu_gp(...) _wait_pcu_gp(false, __VA_ARGS__)

/**
 * synchronize_pcu_mult - Wait concurrently for multiple grace periods
 * @...: List of call_pcu() functions for different grace periods to wait on
 *
 * This macro waits concurrently for multiple types of PCU grace periods.
 * For example, synchronize_pcu_mult(call_pcu, call_pcu_tasks) would wait
 * on concurrent PCU and PCU-tasks grace periods.  Waiting on a given SPCU
 * domain requires you to write a wrapper function for that SPCU domain's
 * call_spcu() function, with this wrapper supplying the pointer to the
 * corresponding spcu_struct.
 *
 * The first argument tells Tiny PCU's _wait_pcu_gp() not to
 * bother waiting for PCU.  The reason for this is because anywhere
 * synchronize_pcu_mult() can be called is automatically already a full
 * grace period.
 */
#define synchronize_pcu_mult(...) \
	_wait_pcu_gp(IS_ENABLED(CONFIG_TINY_PCU), __VA_ARGS__)

#endif /* _LINUX_SCHED_PCUPDATE_WAIT_H */
