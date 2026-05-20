#ifndef LOCKFREE_QUEUE_H
#define LOCKFREE_QUEUE_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/list.h>

/*
 * Lock-free FIFO queue node
 * Wraps inode (i_io_list)
 */
struct QNode {
	struct list_head *inode;
	struct QNode *next;
	struct rcu_head rcu;
};

/*
 * Lock-free MPMC FIFO queue
 * (Michael-Scott style)
 */
struct QueueLF {
	struct QNode *head;
	struct QNode *tail;
	atomic_long_t qlen;
	atomic_long_t null_deq;
};

/* initialization */
void queue_init(struct QueueLF *q);

/* enqueue: producer (dirty inode path) */
void queue_enqueue(struct QueueLF *q, struct list_head *inode);

/* dequeue: consumer (flusher path) */
struct list_head *queue_dequeue(struct QueueLF *q);

#endif /* LOCKFREE_QUEUE_H */
