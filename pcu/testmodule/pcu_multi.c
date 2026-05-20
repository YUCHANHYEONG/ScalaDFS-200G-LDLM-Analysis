#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/cpumask.h>

#include "pcu.h"

#define NR_WORKERS	128

static struct pcu_head test_head;
extern void call_pcu(struct pcu_head *head, pcu_callback_t func);

static struct task_struct *workers[NR_WORKERS];

static void test_pcu_callback(struct pcu_head *head)
{
	printk("[%s] callback executed\n", __func__);
}

static int qs_worker_fn(void *arg)
{
	int cpu = (long)arg;

	set_cpus_allowed_ptr(current, cpumask_of(cpu % nr_cpu_ids));

	printk("[qs_worker] start pid=%d cpu=%d\n", current->pid, smp_processor_id());

	while(!kthread_should_stop())
		schedule_timeout_interruptible(msecs_to_jiffies(1));

	printk("[qs_worker] stop pid=%d\n", current->pid);
	return 0;
}

static int __init pcu_test_init(void)
{
	int i;

	printk("[%s] pcu test module init\n", __func__);

	for (i = 0; i < NR_WORKERS; i++) {
		workers[i] = kthread_run(qs_worker_fn, (void *)(long)i,
				"pcu_qs_worker/%d\n", i);
		if (IS_ERR(workers[i])) {
			printk("[%s] failed to create worker %d\n", __func__, i);
			workers[i] = NULL;
		}
	}

	call_pcu(&test_head, test_pcu_callback);
	printk("[%s] pcu test call_pcu issued\n", __func__);

	return 0;
}

static void __exit pcu_test_exit(void)
{
	int i;
	
	printk("[%s] pcu_test module exit\n", __func__);

	for (i = 0; i < NR_WORKERS; i++) {
		if (workers[i])
			kthread_stop(workers[i]);
	}
}

module_init(pcu_test_init);
module_exit(pcu_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ych");
