#ifndef LFLIST_H
#define LFLIST_H
/*
 * Lock-less NULL terminated single linked list
 *
 * Cases where locking is not needed:
 * If there are multiple producers and multiple consumers, lflist_add can be
 * used in producers and lflist_del_all can be used in consumers simultaneously
 * without locking. Also a single consumer can use lflist_del_first while
 * multiple producers simultaneously use lflist_add, without any locking.
 *
 * Cases where locking is needed:
 * If we have multiple consumers with lflist_del_first used in one consumer, and
 * lflist_del_first or lflist_del_all used in other consumers, then a lock is
 * needed.  This is because lflist_del_first depends on list->first->next not
 * changing, but without lock protection, there's no way to be sure about that
 * if a preemption happens in the middle of the delete operation and on being
 * preempted back, the list->first is the same as before causing the cmpxchg in
 * lflist_del_first to succeed. For example, while a lflist_del_first operation
 * is in progress in one consumer, then a lflist_del_first, lflist_add,
 * lflist_add (or lflist_del_all, lflist_add, lflist_add) sequence in another
 * consumer may cause violations.
 *
 * This can be summarized as follows:
 *
 *           |   add    | del_first |  del_all
 * add       |    -     |     -     |     -
 * del_first |          |     L     |     L
 * del_all   |          |           |     -
 *
 * Where, a particular row's operation can happen concurrently with a column's
 * operation, with "-" being no lock needed, while "L" being lock is needed.
 *
 * The list entries deleted via lflist_del_all can be traversed with
 * traversing function such as lflist_for_each etc.  But the list
 * entries can not be traversed safely before deleted from the list.
 * The order of deleted entries is from the newest to the oldest added
 * one.  If you want to traverse from the oldest to the newest, you
 * must reverse the order by yourself before traversing.
 *
 * The basic atomic operation of this list is cmpxchg on long.  On
 * architectures that don't have NMI-safe cmpxchg implementation, the
 * list can NOT be used in NMI handlers.  So code that uses the list in
 * an NMI handler should depend on CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG.
 *
 * Copyright 2010,2011 Intel Corp.
 *   Author: Huang Ying <ying.huang@intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/types.h>

struct lflist_head {
	struct list_head *first;
};

#define LFLIST_HEAD_INIT(name)	{ NULL }
#define LFLIST_HEAD(name)	struct lflist_head name = LFLIST_HEAD_INIT(name)

/**
 * init_lflist_head - initialize lock-less list head
 * @head:	the head for your lock-less list
 */
static inline void init_lflist_head(struct lflist_head *list)
{
	list->first = NULL;
}

/**
 * lflist_entry - get the struct of this entry
 * @ptr:	the &struct list_head pointer.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_head within the struct.
 */
#define lflist_entry(ptr, type, member)		\
	container_of(ptr, type, member)

/**
 * member_address_is_nonnull - check whether the member address is not NULL
 * @ptr:	the object pointer (struct type * that contains the list_head)
 * @member:	the name of the list_head within the struct.
 *
 * This macro is conceptually the same as
 *	&ptr->member != NULL
 * but it works around the fact that compilers can decide that taking a member
 * address is never a NULL pointer.
 *
 * Real objects that start at a high address and have a member at NULL are
 * unlikely to exist, but such pointers may be returned e.g. by the
 * container_of() macro.
 */
#define member_address_is_nonnull(ptr, member)	\
	((uintptr_t)(ptr) + offsetof(typeof(*(ptr)), member) != 0)

/**
 * lflist_for_each - iterate over some deleted entries of a lock-less list
 * @pos:	the &struct list_head to use as a loop cursor
 * @node:	the first entry of deleted list entries
 *
 * In general, some entries of the lock-less list can be traversed
 * safely only after being deleted from list, so start with an entry
 * instead of list head.
 *
 * If being used on entries deleted from lock-less list directly, the
 * traverse order is from the newest to the oldest added entry.  If
 * you want to traverse from the oldest to the newest, you must
 * reverse the order by yourself before traversing.
 */
#define lflist_for_each(pos, node)			\
	for ((pos) = (node); pos; (pos) = (pos)->next)

/**
 * lflist_for_each_safe - iterate over some deleted entries of a lock-less list
 *			 safe against removal of list entry
 * @pos:	the &struct list_head to use as a loop cursor
 * @n:		another &struct list_head to use as temporary storage
 * @node:	the first entry of deleted list entries
 *
 * In general, some entries of the lock-less list can be traversed
 * safely only after being deleted from list, so start with an entry
 * instead of list head.
 *
 * If being used on entries deleted from lock-less list directly, the
 * traverse order is from the newest to the oldest added entry.  If
 * you want to traverse from the oldest to the newest, you must
 * reverse the order by yourself before traversing.
 */
#define lflist_for_each_safe(pos, n, node)			\
	for ((pos) = (node); (pos) && ((n) = (pos)->next, true); (pos) = (n))

/**
 * lflist_for_each_entry - iterate over some deleted entries of lock-less list of given type
 * @pos:	the type * to use as a loop cursor.
 * @node:	the fist entry of deleted list entries.
 * @member:	the name of the list_head with the struct.
 *
 * In general, some entries of the lock-less list can be traversed
 * safely only after being removed from list, so start with an entry
 * instead of list head.
 *
 * If being used on entries deleted from lock-less list directly, the
 * traverse order is from the newest to the oldest added entry.  If
 * you want to traverse from the oldest to the newest, you must
 * reverse the order by yourself before traversing.
 */
#define lflist_for_each_entry(pos, node, member)				\
	for ((pos) = lflist_entry((node), typeof(*(pos)), member);	\
	     member_address_is_nonnull(pos, member);			\
	     (pos) = lflist_entry((pos)->member.next, typeof(*(pos)), member))

/**
 * lflist_for_each_entry_safe - iterate over some deleted entries of lock-less list of given type
 *			       safe against removal of list entry
 * @pos:	the type * to use as a loop cursor.
 * @n:		another type * to use as temporary storage
 * @node:	the first entry of deleted list entries.
 * @member:	the name of the list_head with the struct.
 *
 * In general, some entries of the lock-less list can be traversed
 * safely only after being removed from list, so start with an entry
 * instead of list head.
 *
 * If being used on entries deleted from lock-less list directly, the
 * traverse order is from the newest to the oldest added entry.  If
 * you want to traverse from the oldest to the newest, you must
 * reverse the order by yourself before traversing.
 */
#define lflist_for_each_entry_safe(pos, n, node, member)			       \
	for (pos = lflist_entry((node), typeof(*pos), member);		       \
	     member_address_is_nonnull(pos, member) &&			       \
	        (n = lflist_entry(pos->member.next, typeof(*n), member), true); \
	     pos = n)

static inline struct list_head *lflist_next(struct list_head *node)
{
	return node->next;
}

extern bool lflist_add_batch(struct list_head *new_first,
			    struct list_head *new_last,
			    struct lflist_head *head);
/**
 * lflist_add - add a new entry
 * @new:	new entry to be added
 * @head:	the head for your lock-less list
 *
 * Returns true if the list was empty prior to adding this entry.
 */
static inline bool lflist_add(struct list_head *new, struct lflist_head *head)
{
	return lflist_add_batch(new, new, head);
}

/**
 * lflist_del_all - delete all entries from lock-less list
 * @head:	the head of lock-less list to delete all entries
 *
 * If list is empty, return NULL, otherwise, delete all entries and
 * return the pointer to the first entry.  The order of entries
 * deleted is from the newest to the oldest added one.
 */
static inline struct list_head *lflist_del_all(struct lflist_head *head)
{
	return xchg(&head->first, NULL);
}


// static inline void lflist_splice_init(struct lflist_head *src, struct list_head *dst)
// {
//     struct list_head *first, *last, *next;

//     first = lflist_del_all(src);
//     if (!first)
//         return;

//     last = first;
//     while ((next = last->next))
//         last = next;

//     __list_splice(first, dst, dst->next);
// }

static inline void lflist_splice_init(struct lflist_head *src, struct list_head *dst)
{
    struct list_head *first, *last, *next;

    first = lflist_del_all(src);
    if (!first)
        return;

    last = first;
    while ((next = last->next)) {
        next->prev = last;
        last = next;
    }

    // Splice into dst
    first->prev = dst;
    last->next = dst->next;
    dst->next->prev = last;
    dst->next = first;
}

extern struct list_head *lflist_del_first(struct lflist_head *head);

struct list_head *lflist_reverse_order(struct list_head *head);

/**
 * list_add_tail - add a new entry
 * @new: new entry to be added
 * @head: list head to add it before
 *
 * Insert a new entry before the specified head.
 * This is useful for implementing queues.
 */
// static inline void 
// lf_list_add_tail(struct list_head *entry, struct list_head *head)
// {
// 	entry->prev = __sync_lock_test_and_set(&head->prev, entry);
// 	if (entry->prev == NULL)
// 		head = entry;
// 	else
// 		entry->prev->next = entry;
// }

static inline void lf_list_add_tail(struct list_head *entry, struct list_head *head)
{
    struct list_head *prev_tail;

    // Atomically get the current tail (previous element) and set head->prev to entry
    prev_tail = __sync_lock_test_and_set(&head->prev, entry);

    // entry's next is head
    entry->next = head;

    // entry's prev is the previous tail
    entry->prev = prev_tail;

    // Update the previous tail's next to the new entry
    prev_tail->next = entry;
}

/*
 * Delete a list entry by making the prev/next entries
 * point to each other.
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
static inline void 
__gc_list_del(struct list_head * prev, struct list_head * next, struct list_head *head)
{
	if (next)
		next->prev = prev;
	else
		head->prev = prev;
	WRITE_ONCE(prev->next, next);
}

static inline void 
gc_list_del(struct list_head *entry, struct list_head *head)
{
	//lf_list_add_tail(entry, head, tail);
	__gc_list_del(entry->prev, entry->next, head);
}

static inline void 
__lf_list_del(struct list_head * prev, struct list_head * next, struct list_head *head)
{
    if (next)
        next->prev = prev;
    else
        head->prev = prev;
    WRITE_ONCE(prev->next, next);
}

static inline void 
lf_list_del(struct list_head *entry, struct list_head *head)
{
    struct list_head *prev, *next;

    do {
        prev = entry->prev;
        next = entry->next;
    } while (cmpxchg(&entry->prev->next, entry, next) != entry);

    if (next)
        next->prev = prev;
    else
        head->prev = prev;

    entry->prev = NULL;
    entry->next = NULL;
}

static inline void lf_list_move_tail(struct list_head *entry, struct list_head *head)
{
	struct list_head *prev_tail;
    // Remove entry from its current position
    entry->next->prev = entry->prev;
    WRITE_ONCE(entry->prev->next, entry->next);

    // Add entry to the tail of the list pointed to by head
    prev_tail = __sync_lock_test_and_set(&head->prev, entry);
    entry->next = head;
    entry->prev = prev_tail;
    prev_tail->next = entry;
}

// static inline void lf_list_move_tail(struct list_head *entry, struct list_head *head)
// {
//     struct list_head *prev_tail;
//     // Remove entry from its current position
//     entry->next->prev = entry->prev;
//     WRITE_ONCE(entry->prev->next, entry->next);

//     // Add entry to the tail of the list pointed to by head

//     do {
//         prev_tail = READ_ONCE(head->prev);
//         entry->prev = prev_tail;
//         entry->next = head;
//     } while (__sync_val_compare_and_swap(&head->prev, prev_tail, entry) != prev_tail);

//     prev_tail->next = entry;
// }


static inline bool lflist_empty(struct list_head *head)
{
    return READ_ONCE(head->next) == head;
}
#endif /* LFLIST_H */
