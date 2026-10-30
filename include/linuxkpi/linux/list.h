/* SPDX-License-Identifier: GPL-2.0-only */
/* Listas circulares doblemente enlazadas (list_head) y listas hash (hlist). */
#ifndef _LKPI_LINUX_LIST_H
#define _LKPI_LINUX_LIST_H

#include <linux/bits.h>
#include <linux/container_of.h>
#include <linux/compiler.h>
#include <linux/poison.h>
#include <linux/stddef.h>
#include <linux/types.h>

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	WRITE_ONCE(list->next, list);
	WRITE_ONCE(list->prev, list);
}

static inline void __list_add(struct list_head *entry, struct list_head *prev,
			      struct list_head *next)
{
	next->prev = entry;
	entry->next = next;
	entry->prev = prev;
	WRITE_ONCE(prev->next, entry);
}
static inline void list_add(struct list_head *entry, struct list_head *head)
{
	__list_add(entry, head, head->next);
}
static inline void list_add_tail(struct list_head *entry, struct list_head *head)
{
	__list_add(entry, head->prev, head);
}
static inline void __list_del(struct list_head *prev, struct list_head *next)
{
	next->prev = prev;
	WRITE_ONCE(prev->next, next);
}
static inline void __list_del_entry(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
}
static inline void list_del(struct list_head *entry)
{
	__list_del_entry(entry);
	entry->next = LIST_POISON1;
	entry->prev = LIST_POISON2;
}
static inline void list_del_init(struct list_head *entry)
{
	__list_del_entry(entry);
	INIT_LIST_HEAD(entry);
}
static inline void list_replace(struct list_head *old, struct list_head *entry)
{
	entry->next = old->next;
	entry->next->prev = entry;
	entry->prev = old->prev;
	entry->prev->next = entry;
}
static inline void list_replace_init(struct list_head *old, struct list_head *entry)
{
	list_replace(old, entry);
	INIT_LIST_HEAD(old);
}
static inline void list_swap(struct list_head *a, struct list_head *b)
{
	struct list_head *pos = b->prev;
	list_del(b);
	list_replace(a, b);
	if (pos == a)
		pos = b;
	list_add(a, pos);
}
static inline void list_move(struct list_head *list, struct list_head *head)
{
	__list_del_entry(list);
	list_add(list, head);
}
static inline void list_move_tail(struct list_head *list, struct list_head *head)
{
	__list_del_entry(list);
	list_add_tail(list, head);
}
/* Mueve [first, last] (consecutivos) al final de head. */
static inline void list_bulk_move_tail(struct list_head *head, struct list_head *first,
				       struct list_head *last)
{
	first->prev->next = last->next;
	last->next->prev = first->prev;
	head->prev->next = first;
	first->prev = head->prev;
	last->next = head;
	head->prev = last;
}
static inline int list_is_first(const struct list_head *list, const struct list_head *head)
{
	return list->prev == head;
}
static inline int list_is_last(const struct list_head *list, const struct list_head *head)
{
	return list->next == head;
}
static inline int list_is_head(const struct list_head *list, const struct list_head *head)
{
	return list == head;
}
static inline int list_empty(const struct list_head *head)
{
	return READ_ONCE(head->next) == head;
}
static inline int list_empty_careful(const struct list_head *head)
{
	struct list_head *next = head->next;
	return list_is_head(next, head) && (next == READ_ONCE(head->prev));
}
static inline void list_rotate_left(struct list_head *head)
{
	if (!list_empty(head))
		list_move_tail(head->next, head);
}
static inline void list_rotate_to_front(struct list_head *list, struct list_head *head)
{
	list_move_tail(head, list);
}
static inline int list_is_singular(const struct list_head *head)
{
	return !list_empty(head) && (head->next == head->prev);
}
/* Pasa a list las entradas de head hasta entry inclusive. */
static inline void list_cut_position(struct list_head *list, struct list_head *head,
				     struct list_head *entry)
{
	if (list_empty(head))
		return;
	if (list_is_singular(head) && !list_is_head(entry, head) && entry != head->next)
		return;
	if (list_is_head(entry, head)) {
		INIT_LIST_HEAD(list);
		return;
	}
	list->next = head->next;
	list->next->prev = list;
	list->prev = entry;
	head->next = entry->next;
	head->next->prev = head;
	entry->next = list;
}
/* Pasa a list las entradas de head anteriores a entry (exclusive). */
static inline void list_cut_before(struct list_head *list, struct list_head *head,
				   struct list_head *entry)
{
	if (head->next == entry) {
		INIT_LIST_HEAD(list);
		return;
	}
	list->next = head->next;
	list->next->prev = list;
	list->prev = entry->prev;
	list->prev->next = list;
	head->next = entry;
	entry->prev = head;
}
static inline void __list_splice(const struct list_head *list, struct list_head *prev,
				 struct list_head *next)
{
	struct list_head *first = list->next, *last = list->prev;
	first->prev = prev;
	prev->next = first;
	last->next = next;
	next->prev = last;
}
static inline void list_splice(const struct list_head *list, struct list_head *head)
{
	if (!list_empty(list))
		__list_splice(list, head, head->next);
}
static inline void list_splice_tail(struct list_head *list, struct list_head *head)
{
	if (!list_empty(list))
		__list_splice(list, head->prev, head);
}
static inline void list_splice_init(struct list_head *list, struct list_head *head)
{
	if (!list_empty(list)) {
		__list_splice(list, head, head->next);
		INIT_LIST_HEAD(list);
	}
}
static inline void list_splice_tail_init(struct list_head *list, struct list_head *head)
{
	if (!list_empty(list)) {
		__list_splice(list, head->prev, head);
		INIT_LIST_HEAD(list);
	}
}
static inline size_t list_count_nodes(struct list_head *head)
{
	struct list_head *pos;
	size_t count = 0;
	for (pos = head->next; pos != head; pos = pos->next)
		count++;
	return count;
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(ptr, type, member) list_entry((ptr)->next, type, member)
#define list_last_entry(ptr, type, member) list_entry((ptr)->prev, type, member)
#define list_first_entry_or_null(ptr, type, member) \
	({ struct list_head *head__ = (ptr); struct list_head *pos__ = READ_ONCE(head__->next); \
	   pos__ != head__ ? list_entry(pos__, type, member) : NULL; })
#define list_last_entry_or_null(ptr, type, member) \
	(!list_empty(ptr) ? list_last_entry(ptr, type, member) : NULL)
#define list_next_entry(pos, member) list_entry((pos)->member.next, typeof(*(pos)), member)
#define list_prev_entry(pos, member) list_entry((pos)->member.prev, typeof(*(pos)), member)
#define list_next_entry_circular(pos, head, member) \
	(list_is_last(&(pos)->member, head) ? \
	 list_first_entry(head, typeof(*(pos)), member) : list_next_entry(pos, member))
#define list_prev_entry_circular(pos, head, member) \
	(list_is_first(&(pos)->member, head) ? \
	 list_last_entry(head, typeof(*(pos)), member) : list_prev_entry(pos, member))
#define list_entry_is_head(pos, head, member) list_is_head(&(pos)->member, (head))
#define list_prepare_entry(pos, head, member) \
	((pos) ? : list_entry(head, typeof(*pos), member))

#define list_for_each(pos, head) \
	for (pos = (head)->next; !list_is_head(pos, (head)); pos = pos->next)
#define list_for_each_continue(pos, head) \
	for (pos = pos->next; !list_is_head(pos, (head)); pos = pos->next)
#define list_for_each_prev(pos, head) \
	for (pos = (head)->prev; !list_is_head(pos, (head)); pos = pos->prev)
#define list_for_each_safe(pos, n, head) \
	for (pos = (head)->next, n = pos->next; !list_is_head(pos, (head)); pos = n, n = pos->next)
#define list_for_each_prev_safe(pos, n, head) \
	for (pos = (head)->prev, n = pos->prev; !list_is_head(pos, (head)); pos = n, n = pos->prev)
#define list_for_each_entry(pos, head, member) \
	for (pos = list_first_entry(head, typeof(*pos), member); \
	     !list_entry_is_head(pos, head, member); pos = list_next_entry(pos, member))
#define list_for_each_entry_reverse(pos, head, member) \
	for (pos = list_last_entry(head, typeof(*pos), member); \
	     !list_entry_is_head(pos, head, member); pos = list_prev_entry(pos, member))
#define list_for_each_entry_continue(pos, head, member) \
	for (pos = list_next_entry(pos, member); \
	     !list_entry_is_head(pos, head, member); pos = list_next_entry(pos, member))
#define list_for_each_entry_continue_reverse(pos, head, member) \
	for (pos = list_prev_entry(pos, member); \
	     !list_entry_is_head(pos, head, member); pos = list_prev_entry(pos, member))
#define list_for_each_entry_from(pos, head, member) \
	for (; !list_entry_is_head(pos, head, member); pos = list_next_entry(pos, member))
#define list_for_each_entry_from_reverse(pos, head, member) \
	for (; !list_entry_is_head(pos, head, member); pos = list_prev_entry(pos, member))
#define list_for_each_entry_safe(pos, n, head, member) \
	for (pos = list_first_entry(head, typeof(*pos), member), n = list_next_entry(pos, member); \
	     !list_entry_is_head(pos, head, member); pos = n, n = list_next_entry(n, member))
#define list_for_each_entry_safe_continue(pos, n, head, member) \
	for (pos = list_next_entry(pos, member), n = list_next_entry(pos, member); \
	     !list_entry_is_head(pos, head, member); pos = n, n = list_next_entry(n, member))
#define list_for_each_entry_safe_from(pos, n, head, member) \
	for (n = list_next_entry(pos, member); \
	     !list_entry_is_head(pos, head, member); pos = n, n = list_next_entry(n, member))
#define list_for_each_entry_safe_reverse(pos, n, head, member) \
	for (pos = list_last_entry(head, typeof(*pos), member), n = list_prev_entry(pos, member); \
	     !list_entry_is_head(pos, head, member); pos = n, n = list_prev_entry(n, member))
#define list_safe_reset_next(pos, n, member) n = list_next_entry(pos, member)

/* --- hlist: cabeza de un puntero, para tablas hash --- */
#define HLIST_HEAD_INIT { .first = NULL }
#define HLIST_HEAD(name) struct hlist_head name = { .first = NULL }
#define INIT_HLIST_HEAD(ptr) ((ptr)->first = NULL)
static inline void INIT_HLIST_NODE(struct hlist_node *h)
{
	h->next = NULL;
	h->pprev = NULL;
}
static inline int hlist_unhashed(const struct hlist_node *h) { return !h->pprev; }
static inline int hlist_unhashed_lockless(const struct hlist_node *h) { return !READ_ONCE(h->pprev); }
static inline int hlist_empty(const struct hlist_head *h) { return !READ_ONCE(h->first); }
static inline void __hlist_del(struct hlist_node *n)
{
	struct hlist_node *next = n->next;
	struct hlist_node **pprev = n->pprev;
	WRITE_ONCE(*pprev, next);
	if (next)
		WRITE_ONCE(next->pprev, pprev);
}
static inline void hlist_del(struct hlist_node *n)
{
	__hlist_del(n);
	n->next = LIST_POISON1;
	n->pprev = LIST_POISON2;
}
static inline void hlist_del_init(struct hlist_node *n)
{
	if (!hlist_unhashed(n)) {
		__hlist_del(n);
		INIT_HLIST_NODE(n);
	}
}
static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
	struct hlist_node *first = h->first;
	WRITE_ONCE(n->next, first);
	if (first)
		WRITE_ONCE(first->pprev, &n->next);
	WRITE_ONCE(h->first, n);
	WRITE_ONCE(n->pprev, &h->first);
}
static inline void hlist_add_before(struct hlist_node *n, struct hlist_node *next)
{
	WRITE_ONCE(n->pprev, next->pprev);
	WRITE_ONCE(n->next, next);
	WRITE_ONCE(next->pprev, &n->next);
	WRITE_ONCE(*(n->pprev), n);
}
static inline void hlist_add_behind(struct hlist_node *n, struct hlist_node *prev)
{
	WRITE_ONCE(n->next, prev->next);
	WRITE_ONCE(prev->next, n);
	WRITE_ONCE(n->pprev, &prev->next);
	if (n->next)
		WRITE_ONCE(n->next->pprev, &n->next);
}
static inline void hlist_add_fake(struct hlist_node *n) { n->pprev = &n->next; }
static inline bool hlist_fake(struct hlist_node *h) { return h->pprev == &h->next; }
static inline bool hlist_is_singular_node(struct hlist_node *n, struct hlist_head *h)
{
	return !n->next && n->pprev == &h->first;
}
static inline void hlist_move_list(struct hlist_head *old, struct hlist_head *entry)
{
	entry->first = old->first;
	if (entry->first)
		entry->first->pprev = &entry->first;
	old->first = NULL;
}
static inline size_t hlist_count_nodes(struct hlist_head *head)
{
	struct hlist_node *pos;
	size_t count = 0;
	for (pos = head->first; pos; pos = pos->next)
		count++;
	return count;
}

#define hlist_entry(ptr, type, member) container_of(ptr, type, member)
#define hlist_entry_safe(ptr, type, member) \
	({ typeof(ptr) ____ptr = (ptr); ____ptr ? hlist_entry(____ptr, type, member) : NULL; })
#define hlist_for_each(pos, head) for (pos = (head)->first; pos; pos = pos->next)
#define hlist_for_each_safe(pos, n, head) \
	for (pos = (head)->first; pos && ({ n = pos->next; 1; }); pos = n)
#define hlist_for_each_entry(pos, head, member) \
	for (pos = hlist_entry_safe((head)->first, typeof(*(pos)), member); pos; \
	     pos = hlist_entry_safe((pos)->member.next, typeof(*(pos)), member))
#define hlist_for_each_entry_continue(pos, member) \
	for (pos = hlist_entry_safe((pos)->member.next, typeof(*(pos)), member); pos; \
	     pos = hlist_entry_safe((pos)->member.next, typeof(*(pos)), member))
#define hlist_for_each_entry_from(pos, member) \
	for (; pos; pos = hlist_entry_safe((pos)->member.next, typeof(*(pos)), member))
#define hlist_for_each_entry_safe(pos, n, head, member) \
	for (pos = hlist_entry_safe((head)->first, typeof(*pos), member); \
	     pos && ({ n = pos->member.next; 1; }); \
	     pos = hlist_entry_safe(n, typeof(*pos), member))

#endif
