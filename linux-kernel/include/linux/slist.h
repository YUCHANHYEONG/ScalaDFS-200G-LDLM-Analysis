#ifndef SLIST_H
#define SLIST_H

#include <linux/atomic.h>
#include <linux/list.h>

struct slist_head {
    struct list_head *first;
};

#define SLIST_HEAD_INIT(name)    { NULL }
#define SLIST_HEAD(name)         struct slist_head name = SLIST_HEAD_INIT(name)

static inline void init_slist_head(struct slist_head *list)
{
    list->first = NULL;
}

#define slist_entry(ptr, type, member)  \
    container_of(ptr, type, member)

#define member_address_is_nonnull(ptr, member)    \
    ((uintptr_t)(ptr) + offsetof(typeof(*(ptr)), member) != 0)

#define slist_for_each(pos, node)           \
    list_for_each(pos, (struct list_head *)(node))

#define slist_for_each_safe(pos, n, node)       \
    list_for_each_safe(pos, n, (struct list_head *)(node))

#define slist_for_each_entry(pos, node, member)        \
    list_for_each_entry(pos, (struct list_head *)(node), member)

#define slist_for_each_entry_safe(pos, n, node, member)    \
    list_for_each_entry_safe(pos, n, (struct list_head *)(node), member)

static inline bool slist_empty(const struct slist_head *head)
{
    return READ_ONCE(head->first) == NULL;
}

static inline struct list_head *slist_next(struct list_head *node)
{
    return node->next;
}

extern bool slist_add_batch(struct list_head *new_first,
                            struct list_head *new_last,
                            struct slist_head *head);

static inline bool slist_add(struct list_head *new, struct slist_head *head)
{
    return slist_add_batch(new, new, head);
}

static inline void __slist_del(struct list_head *prev, struct list_head *next)
{
    if (prev) {
        prev->next = next;
    }
}

static inline void slist_del(struct list_head *entry, struct slist_head *head)
{
    struct list_head *prev = NULL, *pos = head->first;

    // If the entry to delete is the first node
    if (pos == entry) {
        head->first = entry->next;
        return;
    }

    // Traverse the list to find the entry
    while (pos && pos->next != entry) {
        prev = pos;
        pos = pos->next;
    }

    // If the entry is found, delete it
    if (pos) {
        __slist_del(prev, entry->next);
    }
}

/**
 * llist_del_all - delete all entries from lock-less list
 * @head:	the head of lock-less list to delete all entries
 *
 * If list is empty, return NULL, otherwise, delete all entries and
 * return the pointer to the first entry.  The order of entries
 * deleted is from the newest to the oldest added one.
 */
static inline struct list_head *slist_del_all(struct slist_head *head)
{
	return xchg(&head->first, NULL);
}

static inline void slist_splice_init(struct slist_head *src, struct slist_head *dst)
{
    struct list_head *first, *last, *next;

    first = slist_del_all(src);
    if (!first)
        return;

    last = first;
    while ((next = slist_next(last)))
        last = next;

    slist_add_batch(first, last, dst);
}

static inline void __slist_del_entry_nocheck(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
}

static inline void slist_del_init_nocheck(struct list_head *entry)
{
	__slist_del_entry_nocheck(entry);
	INIT_LIST_HEAD(entry);
}

extern struct list_head *slist_del_first(struct slist_head *head);

struct list_head *slist_reverse_order(struct list_head *head);

#endif /* SLIST_H */
