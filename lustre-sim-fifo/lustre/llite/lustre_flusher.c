#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/kthread.h>
#include <linux/wait.h>

#include "lustre_flusher.h"
#include "lustre_disk.h"

struct lustre_sb_info;

struct lustre_flusher_ctx *ctx;

extern long lustre_wb_do_writeback(struct bdi_writeback *wb);

static int lustre_flusher_thread(void *data)
{
	struct lustre_flusher *f = data;

	printk("lustre_flusher: cpu %d started\n", f->cpu);

	while (!kthread_should_stop()) {
		wait_event_interruptible(f->wq, f->need_start || kthread_should_stop());

		if (kthread_should_stop())
			break;

		if (f->wb) {
			struct bdi_writeback *wb = f->wb;
			f->wb = NULL;

			lustre_wb_do_writeback(wb);
		}


		f->need_start = 0;
	}

	printk("lustre_flusher: cpu %d stopped\n", f->cpu);

	return 0;
}


int lustre_flusher_init(struct lustre_sb_info *lsi)
{
	unsigned int i, nr;
	struct lustre_flusher_ctx *ctx;
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

		f->task = kthread_create(
				lustre_flusher_thread,
				f,
				"lustre-flusher/%u", i
				);
		if (IS_ERR(f->task)){
			goto fail;
		}

		kthread_bind(f->task, i);
		wake_up_process(f->task);
	}

	lsi->flusher_ctx = ctx;
	return 0;

fail:
	while (i--)
		kthread_stop(ctx->threads[i].task);
	kfree(ctx->threads);
	kfree(ctx);
	return -ENOMEM;
}

void lustre_flusher_exit(struct lustre_sb_info *lsi)
{
	unsigned int i;
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


void lustre_flusher_wakeup(int cpu, struct bdi_writeback *wb)
{
	struct lustre_flusher *f =  &ctx->threads[cpu];

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
