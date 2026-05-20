#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/pcupdate.h>

#define NR_THREADS 128

static struct task_struct *threads[NR_THREADS];
static struct completion done;
static atomic_t finished;
static long counter;

static int pcu_test_thread(void *arg)
{
	msleep(10);

	pcu_read_lock();
	counter++;
	pcu_read_unlock();

	if (atomic_inc_return(&finished) == NR_THREADS)
		complete(&done);

	return 0;
}

static int __init pcu_test_init(void)
{
	int i;

	printk("[PCU TEST] module loaded\n");

	counter = 0;
	atomic_set(&finished, 0);
	init_completion(&done);

	for (i = 0; i < NR_THREADS; i++) {
		threads[i] = kthread_run(pcu_test_thread, NULL,
				"pcu_test/%d", i);
		if (IS_ERR(threads[i])) {
			printk("[PCU_TEST] failed to create thread %d\n", i);
			return -ENOMEM;
		}
	}

	wait_for_completion(&done);

	printk("[PCU_TEST] all threads done\n");
	printk("[PCU_TEST] counter = %ld (expected %d)\n",
			counter, NR_THREADS);

	return 0;
}

static void __exit pcu_test_exit(void)
{
	printk("[PCU_TEST] module unloaded\n");
}

module_init(pcu_test_init);
module_exit(pcu_test_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PCU basic correctness test");
