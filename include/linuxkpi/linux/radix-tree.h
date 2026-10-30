/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_RADIX_TREE_H
#define _LKPI_LINUX_RADIX_TREE_H
/*
 * radix_tree sobre xarray, para el subconjunto que usan los drivers (insert,
 * lookup, delete, iteracion por ranuras sin etiquetas). `slot` apunta a una
 * copia en el iterador, no a la ranura real: vale para leer con
 * radix_tree_deref_slot() y para borrar con radix_tree_iter_delete().
 */
#include <linux/xarray.h>
#include <linux/gfp.h>
#include <linux/errno.h>

struct radix_tree_root {
	struct xarray xa;
};
struct radix_tree_iter {
	unsigned long index;
	void *cur;
};
#define RADIX_TREE_INIT(name, mask)	{ .xa = XARRAY_INIT((name).xa, 0) }
#define RADIX_TREE(name, mask)		struct radix_tree_root name = RADIX_TREE_INIT(name, mask)
#define INIT_RADIX_TREE(root, mask)	xa_init(&(root)->xa)

static inline bool radix_tree_empty(const struct radix_tree_root *root)
{
	return xa_empty(&root->xa);
}
static inline int radix_tree_insert(struct radix_tree_root *root, unsigned long index, void *item)
{
	return xa_insert(&root->xa, index, item, GFP_KERNEL);
}
static inline void *radix_tree_lookup(const struct radix_tree_root *root, unsigned long index)
{
	return xa_load((struct xarray *)&root->xa, index);
}
static inline void *radix_tree_delete(struct radix_tree_root *root, unsigned long index)
{
	return xa_erase(&root->xa, index);
}
static inline void *radix_tree_deref_slot(void **slot)
{
	return *slot;
}
static inline void **lkpi_radix_next(struct radix_tree_root *root, struct radix_tree_iter *iter, unsigned long start)
{
	unsigned long idx = start;
	void *e = xa_find(&root->xa, &idx, ULONG_MAX, XA_PRESENT);

	if (!e)
		return NULL;
	iter->index = idx;
	iter->cur = e;
	return &iter->cur;
}
#define radix_tree_for_each_slot(slot, root, iter, start) \
	for (slot = lkpi_radix_next(root, iter, start); slot; \
	     slot = (iter)->index == ULONG_MAX ? NULL : lkpi_radix_next(root, iter, (iter)->index + 1))
static inline void radix_tree_iter_delete(struct radix_tree_root *root, struct radix_tree_iter *iter, void **slot)
{
	xa_erase(&root->xa, iter->index);
}
static inline int radix_tree_preload(gfp_t gfp) { return 0; }
static inline void radix_tree_preload_end(void) {}
#endif
