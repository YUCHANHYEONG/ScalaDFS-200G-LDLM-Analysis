#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/kthread.h>
#include <linux/wait.h>

#include "lustre_flusher.h"
#include "lustre_disk.h"
#include <calclock.h>

struct lustre_sb_info;

struct lustre_flusher_ctx *ctx;

extern long lustre_wb_do_writeback(struct bdi_writeback *wb);

KTDEF(lustre_wb_do_writeback);
EXPORT_SYMBOL(lustre_wb_do_writeback_clock);

static int lustre_flusher_thread(void *data)
{
	struct lustre_flusher *f = data;
	ktime_t localclock[2];

	//printk("lustre_flusher: cpu %d started\n", f->cpu);

	while (!kthread_should_stop()) {
		//printk("2 lustre_flusher: cpu %d started\n", f->cpu);
		wait_event_interruptible(f->wq, f->need_start || kthread_should_stop());
		//printk("[%s] 33333333\n", __func__);

		if (kthread_should_stop()){
			//printk("[%s] ych_1\n", __func__);
			break;
		}

		if (f->wb) {
			struct bdi_writeback *wb = f->wb;
			//printk("[%s] ych_2\n", __func__);
			f->wb = NULL;

			ktget(&localclock[0]);
			lustre_wb_do_writeback(wb);
			ktget(&localclock[1]);
			ktput(localclock, lustre_wb_do_writeback);
			//printk("[%s] ych_3\n", __func__);
		}


		//printk("[%s] ych_4\n", __func__);
		f->need_start = 0;
	}
	//printk("[%s] ych_5\n", __func__);

	//printk("lustre_flusher: cpu %d stopped\n", f->cpu);

	return 0;
}


int lustre_flusher_init(struct lustre_sb_info *lsi)
{
	unsigned int i, nr;
	//struct lustre_flusher_ctx *ctx;
	printk("[%s] start!\n", __func__);

	nr = num_online_cpus();
	printk("nr = %d\n", nr);

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->nr_threads= nr;
	ctx->threads = kcalloc(nr, sizeof(struct lustre_flusher), GFP_KERNEL);
	if (!ctx->threads) {
		kfree(ctx);
		return -ENOMEM;
	}

	for (i = 0; i < nr; i++) {
		struct lustre_flusher *f = &ctx->threads[i];

		f->cpu = i;
		f->need_start = 0;
		init_waitqueue_head(&f->wq);

		f->task = kthread_create(
				lustre_flusher_thread,
				f,
				"lustre-flusher/%u", i
				);
		if (IS_ERR(f->task)){
			goto fail;
		}

		kthread_bind(f->task, i);
		//printk("[%s] ych_1\n", __func__);
		wake_up_process(f->task);
	}

	//printk("[%s] ych_2\n", __func__);
	lsi->flusher_ctx = ctx;
	return 0;

fail:
	//printk("[%s] ych_3\n", __func__);
	while (i--)
		kthread_stop(ctx->threads[i].task);
	kfree(ctx->threads);
	kfree(ctx);
	return -ENOMEM;
}
EXPORT_SYMBOL(lustre_flusher_init);

void lustre_flusher_exit(struct lustre_sb_info *lsi)
{
	unsigned int i;
	//printk("[%s] ych_1\n", __func__);
	ctx = lsi->flusher_ctx;

	if (!ctx)
		return;

	for (i = 0; i < ctx->nr_threads; i++) {
		struct lustre_flusher *f = &ctx->threads[i];
		if (f->task)
			kthread_stop(f->task);
	}

	kfree(ctx->threads);
	kfree(ctx);
	lsi->flusher_ctx = NULL;
}
EXPORT_SYMBOL(lustre_flusher_exit);


void lustre_flusher_wakeup(int cpu, struct bdi_writeback *wb)
{
	struct lustre_flusher *f =  &ctx->threads[cpu];
//	printk("[%s] ych_1, current->comm = %s\n", __func__, current->comm);

	f->wb = wb;
	f->need_start = 1;

	wake_up(&f->wq);
}
EXPORT_SYMBOL(lustre_flusher_wakeup);

//void lustre_flusher_wakeup(struct lustre_sb_info *lsi)
//{
//	unsigned int i;
//	struct lustre_flusher_ctx *ctx = lsi->flusher_ctx;
//
//	if (!ctx)
//		return;
//
//	for (i = 0; i < ctx->nr_threads; i++)
//		wake_up(&ctx->threads[i].wq);
//}	
