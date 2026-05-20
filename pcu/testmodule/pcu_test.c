#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pcupdate.h>

#include "pcu.h"

#define NR_TEST_CBS	10000 + 1


static struct pcu_head test_head;
extern void call_pcu(struct pcu_head *head, pcu_callback_t func);

static void test_pcu_callback(struct pcu_head *head)
{
	printk("[%s] callback executed\n", __func__);
}

static int __init pcu_test_init(void)
{
//	int i;

	printk("[%s] pcu test module init\n", __func__);

	/*
	for (i = 0; i < NR_TEST_CBS; i++) {
		pcu_read_lock();
		call_pcu(&test_head, test_pcu_callback);
		pcu_read_unlock();
	}
	*/

	call_pcu(&test_head, test_pcu_callback);

	printk("[%s] pcu test call_pcu issued\n", __func__);

	return 0;
}

static void __exit pcu_test_exit(void)
{
	printk("[%s] pcu_test module exit\n", __func__);
}

module_init(pcu_test_init);
module_exit(pcu_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ych");
