#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/kthread.h>
#include <linux/workqueue.h>

#include "pcu.h"
#include "tree_api.h"

#define PCU_DEBUG 0

#if PCU_DEBUG
static int rcu_do_batch_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	if (strcmp(current->comm, "fio") == 0) {
		printk("[PCU] rcu_do_batch caller=%ps comm=%s\n",
				ri->ret_addr, current->comm);
	}
	return 0;
}

static struct kretprobe rp = {
	.kp.symbol_name = "rcu_do_batch",
	.entry_handler = rcu_do_batch_entry,
	.maxactive = 64,
};

static struct kprobe kp = {
	.symbol_name = "rcu_do_batch",
};

static int handler_pre(struct kprobe *p, struct pt_regs *regs) 
{
	unsigned long caller_ip;
#if defined(CONFIG_X86)
	caller_ip = regs->ip;
#else
	caller_ip = instruction_pointer(regs);
#endif
	if (strcmp(current->comm, "fio") == 0) {
		printk("[PCU] rcu_do_batch entered, caller=%ps, comm=%s\n",
				(void *)instruction_pointer(regs), current->comm);
	}

	return 0;
}
#endif /* end PCU_DEBUG	*/

extern struct shrinker kfree_pcu_shrinker;
extern struct task_struct *pcu_gp_kthread_task;

int pcu_tree_init(void)
{
	printk("[%s]: stub\n", __func__);
	return 0;
}

int pcu_update_init(void)
{
	printk("[%s] stub\n", __func__);
	return 0;
}

int pcu_spcutree_init(void)
{
	printk("[%s] stub\n", __func__);
	return 0;
}

void pcu_stop_gp_kthread(void)
{
	if (pcu_gp_kthread_task) {
		printk("[pcu] stopping gp kthread\n");
		pcu_wakeup_gp_kthread();
		kthread_stop(pcu_gp_kthread_task);
		pcu_gp_kthread_task = NULL;
	}
}

static int pcu_module_init(void)
{
	printk("[%s] pcu module start!\n", __func__);
#if PCU_DEBUG

#ifdef CONFIG_RCU_NOCB_CPU
	printk("[PCU] CONFIG_RCU_NOCB_CPU is ENABLED\n");
#else
	printk("[PCU] CONFIG_RCU_NOCB_CPU is DISABLED\n");
#endif

#ifdef CONFIG_PROVE_RCU
	printk("[%s] CONFIG_PROVE_RCU is ENABLED\n", __func__);
#else
	printk("[%s] CONFIG_PROVE_RCU is DISABLED\n", __func__);
#endif

#ifdef CONFIG_RCU_BOOST
	printk("CONFIG_RCU_BOOST is ENABLED\n");
#else
	printk("CONFIG_RCU_BOOST is DISABLED\n");
#endif

	if (register_kretprobe(&rp)) {
		printk("[PCU] failed to register kretprobe\n");
		return -1;
	}

	printk("[PCU] kretprobe registered for rcu_do_batch\n");
	/* 
	kp.pre_handler = handler_pre;
	register_kprobe(&kp);
	printk("[PCU] kprobe registered for rcu_do_batch\n");
	*/
#endif
	//printk("11111\n");

	pcu_init();

	pcu_spawn_gp_kthread();

	return 0;
}

extern struct workqueue_struct *pcu_gp_wq;
extern struct workqueue_struct *pcu_par_gp_wq;

static void pcu_module_exit(void)
{
#if PCU_DEBUG
	unregister_kretprobe(&rp);
#endif
	unregister_shrinker(&kfree_pcu_shrinker);

	if (pcu_gp_wq) {
		flush_workqueue(pcu_gp_wq);
		destroy_workqueue(pcu_gp_wq);
		pcu_gp_wq = NULL;
	}

	if (pcu_par_gp_wq) {
		flush_workqueue(pcu_par_gp_wq);
		destroy_workqueue(pcu_par_gp_wq);
		pcu_par_gp_wq = NULL;
	}

	pcu_stop_gp_kthread();
	printk("[%s] pcu module exit\n", __func__);
}

module_init(pcu_module_init);
module_exit(pcu_module_exit);

MODULE_LICENSE("GPL");
