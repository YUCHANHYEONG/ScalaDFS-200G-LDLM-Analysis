#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/lockfree_list.h>

#define mem_alloc(size) kmalloc(size, GFP_KERNEL)

static bool marked( struct LNode* node)
{
	return (unsigned long long)(node) & 0x1;
}

static struct LNode* unmark( struct LNode* node)
{
	return (struct LNode*)((unsigned long long)(node) & 0xFFFFFFFFFFFFFFFE);
}

static void rlock_node_rcu_free(struct rcu_head *head)
{
	struct LNode *node = container_of(head, struct LNode, rcu);

	kfree(node);
	
	// pr_debug("LFS: LNode[%d-%d] removed\n", start, end);
}

static void DeleteNode(struct LNode* lock)
{
	while (true) {
		 struct LNode* orig = lock->next;
		unsigned long long _marked = (unsigned long long)orig + 1;
		BUG_ON(!marked((struct LNode *)_marked));
		if (cmpxchg(&lock->next, orig, (struct LNode*)_marked) == orig) {
			// pr_info("Physically deleted \n");
			// pr_debug("LFS: LNode[%d-%d] Logically deleted!\n", 
			// 		lock->start, lock->end);
			break;
		}
	}
}

static struct list_head* DeleteNodeLH(struct LNode* lock)
{
	struct list_head* inode = NULL;
	unsigned long long _marked;
	while (true) {
		struct LNode* orig = lock->next;
		_marked = (unsigned long long)orig + 1;
		// BUG_ON(!marked((struct LNode *)_marked));
		if (!marked((struct LNode *)_marked)){
			return NULL;
		}
		if (cmpxchg(&lock->next, orig, (struct LNode*)_marked) == orig) {
			// pr_debug("LFS: LNode[%d-%d] Logically deleted!\n", 
			// 		lock->start, lock->end);
			inode = lock->inode;
			break;
		}
	}
	return inode;
}


static int InsertNodeRW( struct LNode** listrl, struct LNode* lock, bool try)
{
	 struct LNode** prev;
	 struct LNode* cur;

restart:
	rcu_read_lock();
	prev = listrl;
	cur = *prev;

	while (true) {
		// struct LNode *old = NULL;

		if (marked(cur)) {
			rcu_read_unlock();
			goto restart;
		}

		if (cur && marked(cur->next)) {
			struct LNode* next = unmark(cur->next);

			if (cmpxchg(prev, cur, next) == cur) {
				call_rcu(&cur->rcu, rlock_node_rcu_free);
			}
			cur = next;
			continue;
		}

		// switch (compareRW(cur, lock)) {
		// case -1:
		// 	prev = &cur->next;
		// 	cur = *prev;
		// 	break;
		// case 0:
		// 	if (try)
		// 		goto failed;
		// 	while (cur && !marked(cur->next)) {
		// 		cur = unmark(*prev);
		// 		if (old != cur) {
		// 			struct LNode* cur_debug = unmark(cur);
		// 			int cur_start = -1;
		// 			int cur_end = -1;
		// 			if (cur_debug) {
		// 				cur_start = cur_debug->start;
		// 				cur_end = cur_debug->end;
		// 			}
		// 			pr_debug("LFS: LNode[%d-%d] conflicted!\n",
		// 				cur_debug->start, cur_debug->end);
		// 			if (old != NULL) {
		// 				struct LNode* old_debug = unmark(old);
		// 				int old_start = -1;
		// 				int old_end = -1;
		// 				if (old_debug) {
		// 					old_start = old_debug->start;
		// 					old_end = old_debug->end;
		// 				}
		// 				pr_debug("LFS: cur has changed while we busy waiting!!! "
		// 					"old[%d-%d] cur[%d-%d]",
		// 					old_start, old_end,
		// 					cur_start, cur_end);
		// 			}
		// 			old = cur;
		// 		}
		// 		cpu_relax();
		// 	}
		// 	old = NULL;
		// 	break;
		// case 1:
			lock->next = cur;
			if (cmpxchg(prev, cur, lock) == cur) {
				int ret = 0;
				// if (lock->reader) {
				// 	ret = r_validate(lock, try);
				// } else {
				// 	ret = w_validate(listrl, lock);
				// }

				rcu_read_unlock();
				return ret;
			}
			cur = *prev;
			// break;
		// }
	}
// failed:
	rcu_read_unlock();
	return -1;
}

static void
InitNode(struct LNode *node, struct list_head *inode)
{
	node->inode = inode;
	node->next = NULL;
}

struct RangeLock* __InsertInode(struct ListRL *list_rl,
	struct list_head *inode, bool try)
{
	struct RangeLock *rl;
	struct LNode *lnode;
	int ret = 0;
	// int i, ret = 0;
	// int j;

	rl = mem_alloc(sizeof(struct RangeLock));
	if (!rl)
		return NULL;

#if HASH_MODE
	lnode = mem_alloc(sizeof(struct LNode));
		if (!lnode)
			goto free_rl;
		InitNode(lnode, inode);

		i = hash_list_head(inode);
		// pr_info("Bucket hashed: %d\n", i);
		do {
			ret = InsertNodeRW(&list_rl->head[i], lnode, false);
			if (try && ret < 0) {
				kfree(rl);
				goto free_rl;
			}
		} while	(ret);
		rl->bucket = i;
		rl->node[i] = lnode;
	// }
	return rl;
release_locks:
	for (j = i - 1 ; j>= 0 ; j--) {
		// Deferred Physical deletion of already inserted node
		DeleteNode(rl->node[j]);
	}
#else
	lnode = mem_alloc(sizeof(struct LNode));
	if (!lnode){
		goto free_rl;
	}
	InitNode(lnode, inode);
	// pr_debug("Lnode inode: %p\n", lnode->inode);
	do {
		ret = InsertNodeRW(&list_rl->head, lnode, false);
		if (try && ret < 0)
			goto free_rl;
	} while(ret); 

	rl->node = lnode;
	return rl;
#endif
free_rl:
	kfree(rl);
	return NULL;
}
EXPORT_SYMBOL(__InsertInode);

void MutexRangeRelease(struct RangeLock* rl)
{
#if HASH_MODE
	int i;
	if (!rl || rl->bucket > BUCKET_CNT)
		return;

	if (rl->bucket == ALL_RANGE) {
		for (i = 0 ; i < BUCKET_CNT ; i++) {
			DeleteNode(rl->node[i]);
		}
	} else
		DeleteNode(rl->node[rl->bucket]);
#else
	DeleteNode(rl->node);
#endif
	kfree(rl);
}
EXPORT_SYMBOL(MutexRangeRelease);

struct list_head* __FetchHead(struct LNode** listrl)
{
	struct list_head* inode = NULL;
	struct LNode** prev;
	struct LNode* cur;

restart:
	rcu_read_lock();
	prev = listrl;
	cur = *prev;

	while (true) {
		// struct LNode *old = NULL;

		if (!cur) {
			break;
		}

		if (marked(cur)) {
			rcu_read_unlock();
			goto restart;
		}

		if (cur && marked(cur->next)) {
			struct LNode* next = unmark(cur->next);

			if (cmpxchg(prev, cur, next) == cur) {
				call_rcu(&cur->rcu, rlock_node_rcu_free);
			}
			cur = next;
			continue;
		}

		if (cur) {
			inode = DeleteNodeLH(cur);
			if (!inode)
				continue;
			return inode;
		}

	}
// failed:
	rcu_read_unlock();
	return NULL;
}

#if HASH_MODE

static inline u32 hash_current_pid(void)
{
    pid_t pid = current->pid;  // Get the current PID
    return hash_ptr((void *)(unsigned long)pid, ilog2(BUCKET_CNT));
}

#endif

struct list_head* FetchHead(struct ListRL *listrl)
{
#if HASH_MODE	
	int bucket = hash_current_pid();
	return __FetchHead(&listrl->head[bucket]);
#else
	return __FetchHead(&listrl->head);
#endif
}
EXPORT_SYMBOL(FetchHead);

void bucket_init(struct ListRL *list_rl)
{
#if HASH_MODE
	memset(list_rl->head, 0, sizeof(list_rl->head));
#else
	list_rl->head = NULL;
#endif
}
EXPORT_SYMBOL(bucket_init);

