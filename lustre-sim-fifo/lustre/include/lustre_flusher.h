#ifndef _LUSTRE_FLUSHER_H_
#define _LUSTRE_FLUSHER_H_

#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

struct lustre_sb_info;

struct lustre_flusher {
	int cpu;
	struct task_struct *task;

	wait_queue_head_t wq;

	unsigned int need_start;

	struct bdi_writeback *wb;
};

struct lustre_flusher_ctx {
	unsigned int nr_threads;
	struct lustre_flusher *threads;
};


/* interface */
int lustre_flusher_init(struct lustre_sb_info *lsi);
void lustre_flusher_exit(struct lustre_sb_info *lsi);

#endif
