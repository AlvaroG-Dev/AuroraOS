/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xarray: array asociativo de punteros indexado por unsigned long, con tres
 * marcas por entrada y asignacion de indices libres (xa_alloc). Implementado
 * como arbol radix de 64 ranuras por nodo (kernel/linuxkpi/lib/xarray.c).
 * Entradas validas: punteros alineados a 4 y valores xa_mk_value().
 */
#ifndef _LKPI_LINUX_XARRAY_H
#define _LKPI_LINUX_XARRAY_H

#include <linux/bitops.h>
#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/err.h>
#include <linux/gfp_types.h>
#include <linux/limits.h>
#include <linux/spinlock.h>
#include <linux/types.h>

/* --- Codificacion de entradas --- */
static inline void *xa_mk_value(unsigned long v)
{
	WARN_ON((long)v < 0);
	return (void *)((v << 1) | 1);
}
static inline unsigned long xa_to_value(const void *entry) { return (unsigned long)entry >> 1; }
static inline bool xa_is_value(const void *entry) { return (unsigned long)entry & 1; }
static inline void *xa_tag_pointer(void *p, unsigned long tag) { return (void *)((unsigned long)p | tag); }
static inline void *xa_untag_pointer(void *entry) { return (void *)((unsigned long)entry & ~3UL); }
static inline unsigned int xa_pointer_tag(void *entry) { return (unsigned long)entry & 3UL; }
static inline void *xa_mk_internal(unsigned long v) { return (void *)((v << 2) | 2); }
static inline unsigned long xa_to_internal(const void *entry) { return (unsigned long)entry >> 2; }
static inline bool xa_is_internal(const void *entry) { return ((unsigned long)entry & 3) == 2; }
#define XA_ZERO_ENTRY xa_mk_internal(257)
#define XA_ERROR(errno) ((void *)(((unsigned long)(errno) << 2) | 2UL))
static inline bool xa_is_zero(const void *entry) { return unlikely(entry == XA_ZERO_ENTRY); }
static inline bool xa_is_err(const void *entry)
{
	return unlikely(xa_is_internal(entry) && entry >= xa_mk_internal(-MAX_ERRNO));
}
static inline int xa_err(void *entry)
{
	if (xa_is_err(entry))
		return (long)entry >> 2;
	return 0;
}

struct xa_limit {
	u32 max;
	u32 min;
};
#define XA_LIMIT(_min, _max) (struct xa_limit) { .min = _min, .max = _max }
#define xa_limit_32b XA_LIMIT(0, UINT_MAX)
#define xa_limit_31b XA_LIMIT(0, INT_MAX)
#define xa_limit_16b XA_LIMIT(0, USHRT_MAX)

typedef unsigned __bitwise xa_mark_t;
#define XA_MARK_0 ((xa_mark_t)0U)
#define XA_MARK_1 ((xa_mark_t)1U)
#define XA_MARK_2 ((xa_mark_t)2U)
#define XA_PRESENT ((xa_mark_t)8U)
#define XA_MARK_MAX XA_MARK_2
#define XA_FREE_MARK XA_MARK_0

#define XA_FLAGS_LOCK_IRQ ((gfp_t)1U)
#define XA_FLAGS_LOCK_BH ((gfp_t)2U)
#define XA_FLAGS_TRACK_FREE ((gfp_t)4U)
#define XA_FLAGS_ZERO_BUSY ((gfp_t)8U)
#define XA_FLAGS_ALLOC_WRAPPED ((gfp_t)16U)
#define XA_FLAGS_ACCOUNT ((gfp_t)32U)
#define XA_FLAGS_ALLOC (XA_FLAGS_TRACK_FREE | XA_FLAGS_ZERO_BUSY)
#define XA_FLAGS_ALLOC1 (XA_FLAGS_TRACK_FREE | XA_FLAGS_ZERO_BUSY | 64U)

struct xarray {
	spinlock_t xa_lock;
	gfp_t xa_flags;
	void *xa_head;		/* struct lkpi_xa_node *, o NULL */
	unsigned int xa_shift;	/* el indice maximo es (64 << xa_shift) - 1 */
};

#define XARRAY_INIT(name, flags) { \
	.xa_lock = __SPIN_LOCK_UNLOCKED(name.xa_lock), \
	.xa_flags = flags, .xa_head = NULL, .xa_shift = 0 }
#define DEFINE_XARRAY_FLAGS(name, flags) struct xarray name = XARRAY_INIT(name, flags)
#define DEFINE_XARRAY(name) DEFINE_XARRAY_FLAGS(name, 0)
#define DEFINE_XARRAY_ALLOC(name) DEFINE_XARRAY_FLAGS(name, XA_FLAGS_ALLOC)
#define DEFINE_XARRAY_ALLOC1(name) DEFINE_XARRAY_FLAGS(name, XA_FLAGS_ALLOC1)

static inline void xa_init_flags(struct xarray *xa, gfp_t flags)
{
	spin_lock_init(&xa->xa_lock);
	xa->xa_flags = flags;
	xa->xa_head = NULL;
	xa->xa_shift = 0;
}
static inline void xa_init(struct xarray *xa) { xa_init_flags(xa, 0); }
static inline bool xa_empty(const struct xarray *xa) { return xa->xa_head == NULL; }

#define xa_lock(xa) spin_lock(&(xa)->xa_lock)
#define xa_unlock(xa) spin_unlock(&(xa)->xa_lock)
#define xa_lock_bh(xa) spin_lock_bh(&(xa)->xa_lock)
#define xa_unlock_bh(xa) spin_unlock_bh(&(xa)->xa_lock)
#define xa_lock_irq(xa) spin_lock_irq(&(xa)->xa_lock)
#define xa_unlock_irq(xa) spin_unlock_irq(&(xa)->xa_lock)
#define xa_lock_irqsave(xa, flags) spin_lock_irqsave(&(xa)->xa_lock, flags)
#define xa_unlock_irqrestore(xa, flags) spin_unlock_irqrestore(&(xa)->xa_lock, flags)
#define xa_lock_nested(xa, sub) xa_lock(xa)

/* Sin bloqueo (el llamante tiene xa_lock): __xa_* */
void *lkpi___xa_store(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp);
void *lkpi___xa_erase(struct xarray *xa, unsigned long index);
void *lkpi___xa_cmpxchg(struct xarray *xa, unsigned long index, void *old, void *entry, gfp_t gfp);
int lkpi___xa_insert(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp);
int lkpi___xa_alloc(struct xarray *xa, u32 *id, void *entry, struct xa_limit limit, gfp_t gfp);
int lkpi___xa_alloc_cyclic(struct xarray *xa, u32 *id, void *entry, struct xa_limit limit,
			   u32 *next, gfp_t gfp);
void lkpi___xa_set_mark(struct xarray *xa, unsigned long index, xa_mark_t mark);
void lkpi___xa_clear_mark(struct xarray *xa, unsigned long index, xa_mark_t mark);
/* Toman xa_lock */
void *lkpi_xa_load(struct xarray *xa, unsigned long index);
bool lkpi_xa_get_mark(struct xarray *xa, unsigned long index, xa_mark_t mark);
void *lkpi_xa_find(struct xarray *xa, unsigned long *index, unsigned long max, xa_mark_t filter);
void *lkpi_xa_find_after(struct xarray *xa, unsigned long *index, unsigned long max, xa_mark_t filter);
void lkpi_xa_destroy(struct xarray *xa);
unsigned long lkpi_xa_count(struct xarray *xa);

#define __xa_store lkpi___xa_store
#define __xa_erase lkpi___xa_erase
#define __xa_cmpxchg lkpi___xa_cmpxchg
#define __xa_insert lkpi___xa_insert
#define __xa_alloc lkpi___xa_alloc
#define __xa_alloc_cyclic lkpi___xa_alloc_cyclic
#define __xa_set_mark lkpi___xa_set_mark
#define __xa_clear_mark lkpi___xa_clear_mark
#define xa_load lkpi_xa_load
#define xa_get_mark lkpi_xa_get_mark
#define xa_find lkpi_xa_find
#define xa_find_after lkpi_xa_find_after
#define xa_destroy lkpi_xa_destroy

#define __LKPI_XA_LOCKED(xa, expr) \
	({ typeof(expr) __r; xa_lock(xa); __r = (expr); xa_unlock(xa); __r; })

static inline void *xa_store(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp)
{
	return __LKPI_XA_LOCKED(xa, __xa_store(xa, index, entry, gfp));
}
#define xa_store_bh xa_store
#define xa_store_irq xa_store
static inline void *xa_erase(struct xarray *xa, unsigned long index)
{
	return __LKPI_XA_LOCKED(xa, __xa_erase(xa, index));
}
#define xa_erase_bh xa_erase
#define xa_erase_irq xa_erase
static inline void *xa_cmpxchg(struct xarray *xa, unsigned long index, void *old,
			       void *entry, gfp_t gfp)
{
	return __LKPI_XA_LOCKED(xa, __xa_cmpxchg(xa, index, old, entry, gfp));
}
#define xa_cmpxchg_bh xa_cmpxchg
#define xa_cmpxchg_irq xa_cmpxchg
static inline int xa_insert(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp)
{
	return __LKPI_XA_LOCKED(xa, __xa_insert(xa, index, entry, gfp));
}
#define xa_insert_bh xa_insert
#define xa_insert_irq xa_insert
static inline __must_check int xa_alloc(struct xarray *xa, u32 *id, void *entry,
					struct xa_limit limit, gfp_t gfp)
{
	return __LKPI_XA_LOCKED(xa, __xa_alloc(xa, id, entry, limit, gfp));
}
#define xa_alloc_bh xa_alloc
#define xa_alloc_irq xa_alloc
static inline int xa_alloc_cyclic(struct xarray *xa, u32 *id, void *entry,
				  struct xa_limit limit, u32 *next, gfp_t gfp)
{
	return __LKPI_XA_LOCKED(xa, __xa_alloc_cyclic(xa, id, entry, limit, next, gfp));
}
#define xa_alloc_cyclic_bh xa_alloc_cyclic
#define xa_alloc_cyclic_irq xa_alloc_cyclic
static inline int xa_reserve(struct xarray *xa, unsigned long index, gfp_t gfp)
{
	return xa_err(xa_cmpxchg(xa, index, NULL, XA_ZERO_ENTRY, gfp));
}
#define xa_reserve_bh xa_reserve
#define xa_reserve_irq xa_reserve
static inline void xa_release(struct xarray *xa, unsigned long index)
{
	xa_cmpxchg(xa, index, XA_ZERO_ENTRY, NULL, 0);
}
static inline void xa_set_mark(struct xarray *xa, unsigned long index, xa_mark_t mark)
{
	xa_lock(xa);
	__xa_set_mark(xa, index, mark);
	xa_unlock(xa);
}
static inline void xa_clear_mark(struct xarray *xa, unsigned long index, xa_mark_t mark)
{
	xa_lock(xa);
	__xa_clear_mark(xa, index, mark);
	xa_unlock(xa);
}
static inline bool xa_marked(const struct xarray *xa, xa_mark_t mark)
{
	unsigned long index = 0;
	return lkpi_xa_find((struct xarray *)xa, &index, ULONG_MAX, mark) != NULL;
}

#define xa_for_each_range(xa, index, entry, start, last) \
	for (index = start, entry = xa_find(xa, &index, last, XA_PRESENT); entry; \
	     entry = xa_find_after(xa, &index, last, XA_PRESENT))
#define xa_for_each_start(xa, index, entry, start) \
	xa_for_each_range(xa, index, entry, start, ULONG_MAX)
#define xa_for_each(xa, index, entry) xa_for_each_start(xa, index, entry, 0)
#define xa_for_each_marked(xa, index, entry, filter) \
	for (index = 0, entry = xa_find(xa, &index, ULONG_MAX, filter); entry; \
	     entry = xa_find_after(xa, &index, ULONG_MAX, filter))

#endif
