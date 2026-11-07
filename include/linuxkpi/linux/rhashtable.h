/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rhashtable/rhltable simplificados para LinuxKPI: tabla de cubos fija con un
 * spinlock (mac80211: sta_hash, link_sta_hash, malla). Misma API de uso que
 * Linux para el subconjunto que usan los drivers; sin redimensionado.
 * Las lecturas recorren las cadenas sin bloqueo (como bajo rcu_read_lock):
 * los escritores publican con barreras y los objetos se liberan tras un
 * periodo de gracia de RCU, como en Linux.
 */
#ifndef _LKPI_LINUX_RHASHTABLE_H
#define _LKPI_LINUX_RHASHTABLE_H
#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>

struct rhash_head { struct rhash_head __rcu *next; };
struct rhlist_head { struct rhash_head rhead; struct rhlist_head __rcu *next; };
struct rhashtable;
typedef u32 (*rht_hashfn_t)(const void *data, u32 len, u32 seed);
typedef u32 (*rht_obj_hashfn_t)(const void *data, u32 len, u32 seed);
typedef int (*rht_obj_cmpfn_t)(struct rhashtable_compare_arg *arg, const void *obj);
struct rhashtable_compare_arg { struct rhashtable *ht; const void *key; };
struct rhashtable_params {
	u16 nelem_hint; u16 key_len; u16 key_offset; u16 head_offset;
	unsigned int max_size; u16 min_size;
	bool automatic_shrinking;
	u32 hash_rnd;
	size_t locks_mul;
	rht_hashfn_t hashfn;
	rht_obj_hashfn_t obj_hashfn;
	rht_obj_cmpfn_t obj_cmpfn;
};
#define LKPI_RHT_BUCKETS 256
struct rhashtable {
	struct rhash_head *tbl[LKPI_RHT_BUCKETS];
	spinlock_t lock;
	atomic_t nelems;
	struct rhashtable_params p;
	bool ready;
};
struct rhltable { struct rhashtable ht; };
struct rhashtable_iter { struct rhashtable *ht; unsigned int slot; struct rhash_head *p; struct rhlist_head *list; int skip; bool end_of_table; };
struct rhashtable_walker { struct list_head list; };

static inline u32 lkpi_rht_hash(const void *key, u32 len)
{
	const u8 *p = key; u32 h = 2166136261u;
	while (len--) h = (h ^ *p++) * 16777619u;
	return h % LKPI_RHT_BUCKETS;
}
static inline const void *lkpi_rht_key(const struct rhashtable_params *p, const void *head_obj)
{ return (const char *)head_obj - p->head_offset + p->key_offset; }

static inline int rhashtable_init(struct rhashtable *ht, const struct rhashtable_params *params)
{
	memset(ht, 0, sizeof(*ht));
	spin_lock_init(&ht->lock);
	ht->p = *params;
	ht->ready = true;
	return 0;
}
static inline int rhltable_init(struct rhltable *hlt, const struct rhashtable_params *params) { return rhashtable_init(&hlt->ht, params); }
static inline void rhashtable_destroy(struct rhashtable *ht) { ht->ready = false; }
static inline void rhltable_destroy(struct rhltable *hlt) { rhashtable_destroy(&hlt->ht); }
static inline void rhashtable_free_and_destroy(struct rhashtable *ht, void (*free_fn)(void *ptr, void *arg), void *arg)
{
	unsigned int i;
	for (i = 0; i < LKPI_RHT_BUCKETS; i++) {
		struct rhash_head *p = ht->tbl[i], *n;
		for (; p; p = n) { n = p->next; if (free_fn) free_fn((char *)p - ht->p.head_offset, arg); }
		ht->tbl[i] = NULL;
	}
	ht->ready = false;
}

/* ---- rhashtable (un objeto por clave) ---- */
static inline void *rhashtable_lookup(struct rhashtable *ht, const void *key, const struct rhashtable_params params)
{
	struct rhash_head *p;
	for (p = READ_ONCE(ht->tbl[lkpi_rht_hash(key, params.key_len)]); p; p = READ_ONCE(p->next))
		if (!memcmp(lkpi_rht_key(&params, p), key, params.key_len))
			return (char *)p - params.head_offset;
	return NULL;
}
#define rhashtable_lookup_fast(ht, key, params) rhashtable_lookup(ht, key, params)
static inline int rhashtable_insert_fast(struct rhashtable *ht, struct rhash_head *obj, const struct rhashtable_params params)
{
	const void *key = (char *)obj - params.head_offset + params.key_offset;
	u32 b = lkpi_rht_hash(key, params.key_len);
	unsigned long fl;
	spin_lock_irqsave(&ht->lock, fl);
	if (rhashtable_lookup(ht, key, params)) { spin_unlock_irqrestore(&ht->lock, fl); return -EEXIST; }
	obj->next = ht->tbl[b];
	smp_wmb();
	WRITE_ONCE(ht->tbl[b], obj);
	atomic_inc(&ht->nelems);
	spin_unlock_irqrestore(&ht->lock, fl);
	return 0;
}
static inline int rhashtable_remove_fast(struct rhashtable *ht, struct rhash_head *obj, const struct rhashtable_params params)
{
	const void *key = (char *)obj - params.head_offset + params.key_offset;
	u32 b = lkpi_rht_hash(key, params.key_len);
	struct rhash_head **pp;
	unsigned long fl;
	int ret = -ENOENT;
	spin_lock_irqsave(&ht->lock, fl);
	for (pp = &ht->tbl[b]; *pp; pp = &(*pp)->next)
		if (*pp == obj) { WRITE_ONCE(*pp, obj->next); atomic_dec(&ht->nelems); ret = 0; break; }
	spin_unlock_irqrestore(&ht->lock, fl);
	return ret;
}
static inline void *rhashtable_lookup_get_insert_fast(struct rhashtable *ht, struct rhash_head *obj, const struct rhashtable_params params)
{
	const void *key = (char *)obj - params.head_offset + params.key_offset;
	void *old = rhashtable_lookup(ht, key, params);
	if (old) return old;
	return ERR_PTR(rhashtable_insert_fast(ht, obj, params)) ?: NULL;
}
static inline int rhashtable_lookup_insert_fast(struct rhashtable *ht, struct rhash_head *obj, const struct rhashtable_params params)
{ return rhashtable_insert_fast(ht, obj, params); }

/* ---- rhltable (varios objetos por clave) ---- */
static inline struct rhlist_head *rhltable_lookup(struct rhltable *hlt, const void *key, const struct rhashtable_params params)
{
	struct rhash_head *p;
	for (p = READ_ONCE(hlt->ht.tbl[lkpi_rht_hash(key, params.key_len)]); p; p = READ_ONCE(p->next))
		if (!memcmp((const char *)p - params.head_offset + params.key_offset, key, params.key_len))
			return container_of(p, struct rhlist_head, rhead);
	return NULL;
}
static inline int rhltable_insert_key(struct rhltable *hlt, const void *key, struct rhlist_head *list, const struct rhashtable_params params)
{
	struct rhashtable *ht = &hlt->ht;
	u32 b = lkpi_rht_hash(key, params.key_len);
	struct rhlist_head *head;
	unsigned long fl;
	spin_lock_irqsave(&ht->lock, fl);
	head = rhltable_lookup(hlt, key, params);
	if (head) {
		list->next = head->next;
		smp_wmb();
		WRITE_ONCE(head->next, list);
	} else {
		list->next = NULL;
		list->rhead.next = ht->tbl[b];
		smp_wmb();
		WRITE_ONCE(ht->tbl[b], &list->rhead);
	}
	atomic_inc(&ht->nelems);
	spin_unlock_irqrestore(&ht->lock, fl);
	return 0;
}
static inline int rhltable_insert(struct rhltable *hlt, struct rhlist_head *list, const struct rhashtable_params params)
{ return rhltable_insert_key(hlt, (const char *)list - params.head_offset + params.key_offset, list, params); }
static inline int rhltable_remove(struct rhltable *hlt, struct rhlist_head *list, const struct rhashtable_params params)
{
	struct rhashtable *ht = &hlt->ht;
	const void *key = (const char *)list - params.head_offset + params.key_offset;
	u32 b = lkpi_rht_hash(key, params.key_len);
	struct rhlist_head *head;
	struct rhash_head **pp;
	unsigned long fl;
	int ret = -ENOENT;
	spin_lock_irqsave(&ht->lock, fl);
	head = rhltable_lookup(hlt, key, params);
	if (head == list) {
		struct rhlist_head *dup = list->next;
		for (pp = &ht->tbl[b]; *pp; pp = &(*pp)->next)
			if (*pp == &list->rhead) {
				if (dup) { dup->rhead.next = list->rhead.next; WRITE_ONCE(*pp, &dup->rhead); }
				else WRITE_ONCE(*pp, list->rhead.next);
				ret = 0;
				break;
			}
	} else if (head) {
		struct rhlist_head **lp;
		for (lp = &head->next; *lp; lp = &(*lp)->next)
			if (*lp == list) { WRITE_ONCE(*lp, list->next); ret = 0; break; }
	}
	if (!ret) atomic_dec(&ht->nelems);
	spin_unlock_irqrestore(&ht->lock, fl);
	return ret;
}
#define rhl_for_each_rcu(pos, list) for (pos = list; pos; pos = rcu_dereference_raw(pos->next))
#define rhl_for_each_entry_rcu(tpos, pos, list, member) \
	for (pos = list; pos && ({ tpos = container_of(pos, typeof(*tpos), member); 1; }); pos = rcu_dereference_raw(pos->next))
#define rht_for_each_entry_rcu(tpos, pos, head, member) \
	for (pos = head; pos && ({ tpos = container_of(pos, typeof(*tpos), member); 1; }); pos = rcu_dereference_raw(pos->next))
#define rht_ptr_rcu(p) (p)
#endif
