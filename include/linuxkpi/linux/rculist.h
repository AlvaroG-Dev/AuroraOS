/* SPDX-License-Identifier: GPL-2.0-only */
/* Listas recorribles bajo rcu_read_lock() mientras otro las modifica. */
#ifndef _LKPI_LINUX_RCULIST_H
#define _LKPI_LINUX_RCULIST_H

#include <linux/list.h>
#include <linux/rcupdate.h>

#define list_next_rcu(list) (*((struct list_head **)(&(list)->next)))

static inline void INIT_LIST_HEAD_RCU(struct list_head *list)
{
	WRITE_ONCE(list->next, list);
	WRITE_ONCE(list->prev, list);
}

/* Publica el nodo nuevo despues de inicializarlo. */
static inline void __list_add_rcu(struct list_head *n, struct list_head *prev,
				  struct list_head *next)
{
	n->next = next;
	n->prev = prev;
	rcu_assign_pointer(list_next_rcu(prev), n);
	next->prev = n;
}
static inline void list_add_rcu(struct list_head *n, struct list_head *head)
{
	__list_add_rcu(n, head, head->next);
}
static inline void list_add_tail_rcu(struct list_head *n, struct list_head *head)
{
	__list_add_rcu(n, head->prev, head);
}
/* El nodo sigue apuntando hacia delante: un lector en el puede seguir. */
static inline void list_del_rcu(struct list_head *entry)
{
	__list_del_entry(entry);
	entry->prev = LIST_POISON2;
}
static inline void list_del_init_rcu(struct list_head *entry)
{
	__list_del_entry(entry);
	entry->prev = entry;
}
static inline void list_replace_rcu(struct list_head *old, struct list_head *n)
{
	n->next = old->next;
	n->prev = old->prev;
	rcu_assign_pointer(list_next_rcu(n->prev), n);
	n->next->prev = n;
	old->prev = LIST_POISON2;
}

#define list_entry_rcu(ptr, type, member) container_of(READ_ONCE(ptr), type, member)
#define list_first_entry_rcu(ptr, type, member) list_entry_rcu((ptr)->next, type, member)
#define list_first_or_null_rcu(ptr, type, member) \
	({ struct list_head *__head = (ptr); \
	   struct list_head *__next = READ_ONCE(__head->next); \
	   likely(__head != __next) ? list_entry_rcu(__next, type, member) : NULL; })
#define list_next_or_null_rcu(head, ptr, type, member) \
	({ struct list_head *__head = (head); struct list_head *__ptr = (ptr); \
	   struct list_head *__next = READ_ONCE(__ptr->next); \
	   likely(__next != __head) ? list_entry_rcu(__next, type, member) : NULL; })
#define list_tail_rcu(head) (*((struct list_head **)(&(head)->prev)))

#define list_for_each_rcu(pos, head) \
	for (pos = rcu_dereference((head)->next); pos != (head); \
	     pos = rcu_dereference(pos->next))
#define list_for_each_entry_rcu(pos, head, member, cond...) \
	for (pos = list_entry_rcu((head)->next, typeof(*pos), member); \
	     &pos->member != (head); \
	     pos = list_entry_rcu(pos->member.next, typeof(*pos), member))
#define list_for_each_entry_srcu(pos, head, member, cond) \
	list_for_each_entry_rcu(pos, head, member)
#define list_for_each_entry_lockless(pos, head, member) \
	list_for_each_entry_rcu(pos, head, member)
#define list_for_each_entry_continue_rcu(pos, head, member) \
	for (pos = list_entry_rcu(pos->member.next, typeof(*pos), member); \
	     &pos->member != (head); \
	     pos = list_entry_rcu(pos->member.next, typeof(*pos), member))
#define list_for_each_entry_from_rcu(pos, head, member) \
	for (; &(pos)->member != (head); \
	     pos = list_entry_rcu(pos->member.next, typeof(*(pos)), member))

/* ------------------------------------------------------------- hlist */

#define hlist_first_rcu(head) (*((struct hlist_node **)(&(head)->first)))
#define hlist_next_rcu(node) (*((struct hlist_node **)(&(node)->next)))
#define hlist_pprev_rcu(node) (*((struct hlist_node **)((node)->pprev)))

static inline void hlist_del_rcu(struct hlist_node *n)
{
	__hlist_del(n);
	WRITE_ONCE(n->pprev, LIST_POISON2);
}
static inline void hlist_del_init_rcu(struct hlist_node *n)
{
	if (!hlist_unhashed(n)) {
		__hlist_del(n);
		WRITE_ONCE(n->pprev, NULL);
	}
}
static inline void hlist_add_head_rcu(struct hlist_node *n, struct hlist_head *h)
{
	struct hlist_node *first = h->first;

	n->next = first;
	WRITE_ONCE(n->pprev, &h->first);
	rcu_assign_pointer(hlist_first_rcu(h), n);
	if (first)
		WRITE_ONCE(first->pprev, &n->next);
}
static inline void hlist_add_behind_rcu(struct hlist_node *n, struct hlist_node *prev)
{
	n->next = prev->next;
	WRITE_ONCE(n->pprev, &prev->next);
	rcu_assign_pointer(hlist_next_rcu(prev), n);
	if (n->next)
		WRITE_ONCE(n->next->pprev, &n->next);
}
static inline void hlist_replace_rcu(struct hlist_node *old, struct hlist_node *n)
{
	struct hlist_node *next = old->next;

	n->next = next;
	WRITE_ONCE(n->pprev, old->pprev);
	rcu_assign_pointer(*(struct hlist_node **)n->pprev, n);
	if (next)
		WRITE_ONCE(n->next->pprev, &n->next);
	WRITE_ONCE(old->pprev, LIST_POISON2);
}

#define hlist_for_each_entry_rcu(pos, head, member, cond...) \
	for (pos = hlist_entry_safe(rcu_dereference_raw(hlist_first_rcu(head)), \
				    typeof(*(pos)), member); \
	     pos; \
	     pos = hlist_entry_safe(rcu_dereference_raw(hlist_next_rcu(&(pos)->member)), \
				    typeof(*(pos)), member))
#define hlist_for_each_entry_srcu(pos, head, member, cond) \
	hlist_for_each_entry_rcu(pos, head, member)
#define hlist_for_each_entry_rcu_notrace(pos, head, member) \
	hlist_for_each_entry_rcu(pos, head, member)
#define hlist_for_each_entry_continue_rcu(pos, member) \
	for (pos = hlist_entry_safe(rcu_dereference_raw(hlist_next_rcu(&(pos)->member)), \
				    typeof(*(pos)), member); \
	     pos; \
	     pos = hlist_entry_safe(rcu_dereference_raw(hlist_next_rcu(&(pos)->member)), \
				    typeof(*(pos)), member))

#endif
