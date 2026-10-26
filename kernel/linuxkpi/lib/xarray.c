// SPDX-License-Identifier: GPL-2.0-only
/*
 * xarray como arbol radix de 64 ranuras por nivel. Cada nodo lleva:
 *   present  ranura no vacia (entrada, entrada reservada o hijo)
 *   full     hoja: ranura ocupada; interior: subarbol del hijo sin huecos
 *   marks[]  hoja: entrada marcada; interior: el hijo tiene alguna marcada
 * Escrituras con xa_lock (las __xa_* esperan que el llamante lo tenga).
 * Lecturas sin bloqueo como en Linux; liberar nodos con lectores
 * concurrentes necesita RCU, que llega en L2 (xa_node_free).
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/xarray.h>

#define XA_CHUNK_SHIFT 6
#define XA_CHUNK_SIZE 64
#define XA_MARKS 3
#define XA_FLAGS_ZERO_INDEX_BUSY 64U	/* ALLOC1: el indice 0 no se asigna */

struct xa_node {
	unsigned char shift;
	unsigned char offset;		/* ranura en el padre */
	unsigned char count;		/* ranuras no vacias */
	struct xa_node *parent;
	unsigned long present;
	unsigned long full;
	unsigned long marks[XA_MARKS];
	void *slots[XA_CHUNK_SIZE];
};

static inline struct xa_node *xa_head_node(const struct xarray *xa)
{
	return xa->xa_head;
}

/* El indice cabe en el arbol actual. */
static bool xa_covers(const struct xarray *xa, unsigned long index)
{
	unsigned int top = xa->xa_shift + XA_CHUNK_SHIFT;
	return xa->xa_head && (top >= BITS_PER_LONG || (index >> top) == 0);
}

/* Ultima ranura valida de un nodo (con shift 60 solo hay 16). */
static unsigned int xa_last_offset(const struct xa_node *node)
{
	if (node->shift + XA_CHUNK_SHIFT > BITS_PER_LONG)
		return (unsigned int)(ULONG_MAX >> node->shift);
	return XA_CHUNK_SIZE - 1;
}

static struct xa_node *xa_node_alloc(unsigned int shift, gfp_t gfp)
{
	struct xa_node *node = kzalloc(sizeof(*node), gfp & ~__GFP_ZERO);
	if (node)
		node->shift = shift;
	return node;
}

static void xa_node_free(struct xa_node *node)
{
	kfree(node);	/* L2: kfree_rcu */
}

/* Recalcula en los antecesores los bits que dependen del hijo. */
static void xa_update_up(struct xa_node *node)
{
	while (node->parent) {
		struct xa_node *p = node->parent;
		unsigned long bit = 1UL << node->offset;
		if (node->full == ~0UL || (node->shift + XA_CHUNK_SHIFT > BITS_PER_LONG &&
					   node->full == (1UL << (xa_last_offset(node) + 1)) - 1))
			p->full |= bit;
		else
			p->full &= ~bit;
		for (int m = 0; m < XA_MARKS; m++) {
			if (node->marks[m])
				p->marks[m] |= bit;
			else
				p->marks[m] &= ~bit;
		}
		node = p;
	}
}

/* Hace crecer el arbol hasta que cubra index. */
static int xa_expand(struct xarray *xa, unsigned long index, gfp_t gfp)
{
	struct xa_node *head = xa_head_node(xa), *node;

	if (!head) {
		unsigned int shift = 0;
		while (shift + XA_CHUNK_SHIFT < BITS_PER_LONG && (index >> (shift + XA_CHUNK_SHIFT)))
			shift += XA_CHUNK_SHIFT;
		node = xa_node_alloc(shift, gfp);
		if (!node)
			return -ENOMEM;
		xa->xa_head = node;
		xa->xa_shift = shift;
		return 0;
	}
	while (!xa_covers(xa, index)) {
		node = xa_node_alloc(xa->xa_shift + XA_CHUNK_SHIFT, gfp);
		if (!node)
			return -ENOMEM;
		head = xa_head_node(xa);
		node->slots[0] = head;
		node->present = 1;
		node->count = 1;
		head->parent = node;
		head->offset = 0;
		xa->xa_head = node;
		xa->xa_shift = node->shift;
		xa_update_up(head);
	}
	return 0;
}

/* Hoja que contiene index, creando los nodos que falten. */
static struct xa_node *xa_create_leaf(struct xarray *xa, unsigned long index, gfp_t gfp)
{
	struct xa_node *node;

	if (xa_expand(xa, index, gfp))
		return NULL;
	node = xa_head_node(xa);
	while (node->shift) {
		unsigned int off = (index >> node->shift) & (XA_CHUNK_SIZE - 1);
		struct xa_node *child = node->slots[off];
		if (!child) {
			child = xa_node_alloc(node->shift - XA_CHUNK_SHIFT, gfp);
			if (!child)
				return NULL;
			child->parent = node;
			child->offset = off;
			node->slots[off] = child;
			node->present |= 1UL << off;
			node->count++;
		}
		node = child;
	}
	return node;
}

static struct xa_node *xa_find_leaf(const struct xarray *xa, unsigned long index)
{
	struct xa_node *node;

	if (!xa_covers(xa, index))
		return NULL;
	node = READ_ONCE(xa->xa_head);
	while (node && node->shift)
		node = READ_ONCE(node->slots[(index >> node->shift) & (XA_CHUNK_SIZE - 1)]);
	return node;
}

/* Quita los nodos vacios desde node hacia arriba. */
static void xa_prune(struct xarray *xa, struct xa_node *node)
{
	while (node->count == 0) {
		struct xa_node *parent = node->parent;
		unsigned long bit;
		if (!parent) {
			xa->xa_head = NULL;
			xa->xa_shift = 0;
			xa_node_free(node);
			return;
		}
		bit = 1UL << node->offset;
		parent->slots[node->offset] = NULL;
		parent->present &= ~bit;
		parent->full &= ~bit;
		for (int m = 0; m < XA_MARKS; m++)
			parent->marks[m] &= ~bit;
		parent->count--;
		xa_node_free(node);
		node = parent;
	}
	xa_update_up(node);
}

static void *xa_zero_to_null(void *entry)
{
	return xa_is_zero(entry) ? NULL : entry;
}

/* Valor crudo (XA_ZERO_ENTRY incluido). */
void *lkpi_xa_load_raw(const struct xarray *xa, unsigned long index);
void *lkpi_xa_load_raw(const struct xarray *xa, unsigned long index)
{
	struct xa_node *leaf = xa_find_leaf(xa, index);
	return leaf ? READ_ONCE(leaf->slots[index & (XA_CHUNK_SIZE - 1)]) : NULL;
}

void *lkpi_xa_load(struct xarray *xa, unsigned long index)
{
	return xa_zero_to_null(lkpi_xa_load_raw(xa, index));
}
EXPORT_SYMBOL(lkpi_xa_load);

void *lkpi___xa_erase(struct xarray *xa, unsigned long index)
{
	struct xa_node *leaf = xa_find_leaf(xa, index);
	unsigned int off = index & (XA_CHUNK_SIZE - 1);
	unsigned long bit = 1UL << off;
	void *old;

	if (!leaf || !(old = leaf->slots[off]))
		return NULL;
	WRITE_ONCE(leaf->slots[off], NULL);
	leaf->present &= ~bit;
	leaf->full &= ~bit;
	for (int m = 0; m < XA_MARKS; m++)
		leaf->marks[m] &= ~bit;
	leaf->count--;
	xa_prune(xa, leaf);
	return xa_zero_to_null(old);
}
EXPORT_SYMBOL(lkpi___xa_erase);

/* Guarda entry (no NULL) y devuelve la entrada cruda anterior. */
static void *xa_store_raw(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp)
{
	struct xa_node *leaf = xa_create_leaf(xa, index, gfp);
	unsigned int off = index & (XA_CHUNK_SIZE - 1);
	void *old;

	if (!leaf)
		return XA_ERROR(-ENOMEM);
	old = leaf->slots[off];
	WRITE_ONCE(leaf->slots[off], entry);
	if (!old) {
		leaf->present |= 1UL << off;
		leaf->full |= 1UL << off;
		leaf->count++;
		xa_update_up(leaf);
	}
	return old;
}

static bool xa_entry_invalid(void *entry)
{
	return entry && xa_is_internal(entry) && entry != XA_ZERO_ENTRY;
}

void *lkpi___xa_store(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp)
{
	void *old;

	if (WARN_ON_ONCE(xa_entry_invalid(entry)))
		return XA_ERROR(-EINVAL);
	if (!entry && (xa->xa_flags & XA_FLAGS_TRACK_FREE))
		entry = XA_ZERO_ENTRY;
	if (!entry)
		return lkpi___xa_erase(xa, index);
	old = xa_store_raw(xa, index, entry, gfp);
	return xa_is_err(old) ? old : xa_zero_to_null(old);
}
EXPORT_SYMBOL(lkpi___xa_store);

void *lkpi___xa_cmpxchg(struct xarray *xa, unsigned long index, void *old,
			void *entry, gfp_t gfp)
{
	void *curr;

	if (WARN_ON_ONCE(xa_entry_invalid(entry)))
		return XA_ERROR(-EINVAL);
	curr = lkpi_xa_load_raw(xa, index);
	if (curr == old) {
		if (!entry) {
			lkpi___xa_erase(xa, index);
		} else {
			void *r = xa_store_raw(xa, index, entry, gfp);
			if (xa_is_err(r))
				return r;
		}
	}
	return xa_zero_to_null(curr);
}
EXPORT_SYMBOL(lkpi___xa_cmpxchg);

int lkpi___xa_insert(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp)
{
	void *r;

	if (WARN_ON_ONCE(xa_entry_invalid(entry)))
		return -EINVAL;
	if (!entry)
		entry = XA_ZERO_ENTRY;
	if (lkpi_xa_load_raw(xa, index))
		return -EBUSY;
	r = xa_store_raw(xa, index, entry, gfp);
	return xa_is_err(r) ? xa_err(r) : 0;
}
EXPORT_SYMBOL(lkpi___xa_insert);

/* Primer indice libre >= min (y <= max) en el subarbol de node (base). */
static bool xa_free_in_node(const struct xa_node *node, unsigned long base,
			    unsigned long min, unsigned long max, unsigned long *out)
{
	unsigned int start = (min - base) >> node->shift;
	unsigned int last = xa_last_offset(node);

	for (unsigned int off = start; off <= last; off++) {
		unsigned long ibase = base + ((unsigned long)off << node->shift);
		unsigned long bit = 1UL << off;
		const struct xa_node *child;

		if (ibase > max)
			return false;
		if (node->shift == 0) {
			if (!(node->full & bit)) {
				*out = ibase;
				return true;
			}
			continue;
		}
		child = node->slots[off];
		if (!child) {
			*out = off == start ? max(min, ibase) : ibase;
			return true;
		}
		if (node->full & bit)
			continue;
		if (xa_free_in_node(child, ibase, off == start ? max(min, ibase) : ibase, max, out))
			return true;
	}
	return false;
}

static bool xa_find_free(const struct xarray *xa, unsigned long min, unsigned long max,
			 unsigned long *out)
{
	unsigned int top = xa->xa_shift + XA_CHUNK_SHIFT;
	unsigned long end;

	if (min > max)
		return false;
	if (!xa_covers(xa, min)) {
		*out = min;
		return true;
	}
	if (xa_free_in_node(xa_head_node(xa), 0, min, max, out))
		return true;
	if (top >= BITS_PER_LONG)
		return false;
	end = (1UL << top) - 1;	/* ultimo indice que cubre el arbol */
	if (end < max) {
		*out = end + 1;
		return true;
	}
	return false;
}

int lkpi___xa_alloc(struct xarray *xa, u32 *id, void *entry, struct xa_limit limit, gfp_t gfp)
{
	unsigned long index, min = limit.min;
	void *r;

	if (WARN_ON_ONCE(xa_entry_invalid(entry)))
		return -EINVAL;
	if (WARN_ON_ONCE(!(xa->xa_flags & XA_FLAGS_TRACK_FREE)))
		return -EINVAL;
	if (!entry)
		entry = XA_ZERO_ENTRY;
	if ((xa->xa_flags & XA_FLAGS_ZERO_INDEX_BUSY) && min == 0)
		min = 1;
	if (!xa_find_free(xa, min, limit.max, &index))
		return -EBUSY;
	r = xa_store_raw(xa, index, entry, gfp);
	if (xa_is_err(r))
		return xa_err(r);
	*id = (u32)index;
	return 0;
}
EXPORT_SYMBOL(lkpi___xa_alloc);

int lkpi___xa_alloc_cyclic(struct xarray *xa, u32 *id, void *entry, struct xa_limit limit,
			   u32 *next, gfp_t gfp)
{
	u32 min = limit.min;
	int ret;

	limit.min = max(min, *next);
	ret = lkpi___xa_alloc(xa, id, entry, limit, gfp);
	if ((xa->xa_flags & XA_FLAGS_ALLOC_WRAPPED) && ret == 0) {
		xa->xa_flags &= ~XA_FLAGS_ALLOC_WRAPPED;
		ret = 1;
	}
	if (ret < 0 && limit.min > min) {
		limit.min = min;
		ret = lkpi___xa_alloc(xa, id, entry, limit, gfp);
		if (ret == 0)
			ret = 1;
	}
	if (ret >= 0) {
		*next = *id + 1;
		if (*next == 0)
			xa->xa_flags |= XA_FLAGS_ALLOC_WRAPPED;
	}
	return ret;
}
EXPORT_SYMBOL(lkpi___xa_alloc_cyclic);

void lkpi___xa_set_mark(struct xarray *xa, unsigned long index, xa_mark_t mark)
{
	struct xa_node *leaf = xa_find_leaf(xa, index);
	unsigned int off = index & (XA_CHUNK_SIZE - 1);

	if (WARN_ON_ONCE((unsigned int)mark >= XA_MARKS))
		return;
	if (!leaf || !leaf->slots[off])
		return;
	leaf->marks[mark] |= 1UL << off;
	xa_update_up(leaf);
}
EXPORT_SYMBOL(lkpi___xa_set_mark);

void lkpi___xa_clear_mark(struct xarray *xa, unsigned long index, xa_mark_t mark)
{
	struct xa_node *leaf = xa_find_leaf(xa, index);
	unsigned int off = index & (XA_CHUNK_SIZE - 1);

	if (WARN_ON_ONCE((unsigned int)mark >= XA_MARKS))
		return;
	if (!leaf)
		return;
	leaf->marks[mark] &= ~(1UL << off);
	xa_update_up(leaf);
}
EXPORT_SYMBOL(lkpi___xa_clear_mark);

bool lkpi_xa_get_mark(struct xarray *xa, unsigned long index, xa_mark_t mark)
{
	struct xa_node *leaf = xa_find_leaf(xa, index);
	if ((unsigned int)mark >= XA_MARKS || !leaf)
		return false;
	return leaf->marks[mark] & (1UL << (index & (XA_CHUNK_SIZE - 1)));
}
EXPORT_SYMBOL(lkpi_xa_get_mark);

/* Primera entrada visible (no reservada) en [min, max] que pase el filtro. */
static void *xa_find_in_node(const struct xa_node *node, unsigned long base, unsigned long min,
			     unsigned long max, xa_mark_t filter, unsigned long *out)
{
	unsigned int start = (min - base) >> node->shift;
	unsigned int last = xa_last_offset(node);
	unsigned long bits = filter == XA_PRESENT ? node->present : node->marks[filter];

	for (unsigned int off = start; off <= last; off++) {
		unsigned long ibase = base + ((unsigned long)off << node->shift);
		void *e;

		if (ibase > max)
			return NULL;
		if (!(bits & (1UL << off)))
			continue;
		e = READ_ONCE(node->slots[off]);
		if (node->shift == 0) {
			if (!e || xa_is_zero(e))
				continue;
			*out = ibase;
			return e;
		}
		e = xa_find_in_node(e, ibase, off == start ? max(min, ibase) : ibase, max,
				    filter, out);
		if (e)
			return e;
	}
	return NULL;
}

void *lkpi_xa_find(struct xarray *xa, unsigned long *index, unsigned long max, xa_mark_t filter)
{
	unsigned long idx;
	void *e;

	if (filter != XA_PRESENT && (unsigned int)filter >= XA_MARKS)
		return NULL;
	if (*index > max || !xa_covers(xa, *index))
		return NULL;
	e = xa_find_in_node(xa_head_node(xa), 0, *index, max, filter, &idx);
	if (e)
		*index = idx;
	return e;
}
EXPORT_SYMBOL(lkpi_xa_find);

void *lkpi_xa_find_after(struct xarray *xa, unsigned long *index, unsigned long max,
			 xa_mark_t filter)
{
	unsigned long idx;
	void *e;

	if (*index == ULONG_MAX)
		return NULL;
	idx = *index + 1;
	e = lkpi_xa_find(xa, &idx, max, filter);
	if (e)
		*index = idx;
	return e;
}
EXPORT_SYMBOL(lkpi_xa_find_after);

static void xa_free_tree(struct xa_node *node)
{
	if (node->shift)
		for (int off = 0; off < XA_CHUNK_SIZE; off++)
			if (node->slots[off])
				xa_free_tree(node->slots[off]);
	xa_node_free(node);
}

void lkpi_xa_destroy(struct xarray *xa)
{
	unsigned long flags;
	struct xa_node *head;

	xa_lock_irqsave(xa, flags);
	head = xa_head_node(xa);
	xa->xa_head = NULL;
	xa->xa_shift = 0;
	xa_unlock_irqrestore(xa, flags);
	if (head)
		xa_free_tree(head);
}
EXPORT_SYMBOL(lkpi_xa_destroy);

static unsigned long xa_count_node(const struct xa_node *node)
{
	unsigned long n = 0;
	for (int off = 0; off < XA_CHUNK_SIZE; off++) {
		void *e = node->slots[off];
		if (!e)
			continue;
		if (node->shift)
			n += xa_count_node(e);
		else if (!xa_is_zero(e))
			n++;
	}
	return n;
}

/* Entradas visibles (para pruebas y depuracion). */
unsigned long lkpi_xa_count(struct xarray *xa)
{
	struct xa_node *head = xa_head_node(xa);
	return head ? xa_count_node(head) : 0;
}
EXPORT_SYMBOL(lkpi_xa_count);
