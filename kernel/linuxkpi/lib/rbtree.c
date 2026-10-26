// SPDX-License-Identifier: GPL-2.0-only
/*
 * Arbol rojo-negro (algoritmos clasicos de Cormen et al.) con el formato de
 * nodo de Linux: el bit 0 de __rb_parent_color es el color (1 = negro) y el
 * resto, el padre. Las rotaciones avisan al callback de aumentacion
 * rotate(antiguo, nuevo) cuando "nuevo" ocupa el sitio de "antiguo".
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/rbtree_augmented.h>

typedef void (*rb_rotate_fn)(struct rb_node *old, struct rb_node *new);

static inline bool node_is_red(const struct rb_node *n)
{
	return n && !(n->__rb_parent_color & 1);
}
static inline bool node_is_black(const struct rb_node *n)
{
	return !n || (n->__rb_parent_color & 1);
}
static inline void set_black(struct rb_node *n) { n->__rb_parent_color |= 1; }
static inline void set_red(struct rb_node *n) { n->__rb_parent_color &= ~1UL; }
static inline void set_parent(struct rb_node *n, struct rb_node *p)
{
	n->__rb_parent_color = (unsigned long)p | (n->__rb_parent_color & 1);
}
static inline void copy_color(struct rb_node *dst, const struct rb_node *src)
{
	dst->__rb_parent_color = (dst->__rb_parent_color & ~1UL) | (src->__rb_parent_color & 1);
}

static inline void change_child(struct rb_node *old, struct rb_node *new,
				struct rb_node *parent, struct rb_root *root)
{
	if (parent) {
		if (parent->rb_left == old)
			WRITE_ONCE(parent->rb_left, new);
		else
			WRITE_ONCE(parent->rb_right, new);
	} else {
		WRITE_ONCE(root->rb_node, new);
	}
}

/* Rotaciones: conservan el color de cada nodo. */
static void rotate_left(struct rb_node *x, struct rb_root *root, rb_rotate_fn aug)
{
	struct rb_node *y = x->rb_right, *p = rb_parent(x);

	WRITE_ONCE(x->rb_right, y->rb_left);
	if (y->rb_left)
		set_parent(y->rb_left, x);
	WRITE_ONCE(y->rb_left, x);
	set_parent(y, p);
	change_child(x, y, p, root);
	set_parent(x, y);
	if (aug)
		aug(x, y);
}

static void rotate_right(struct rb_node *x, struct rb_root *root, rb_rotate_fn aug)
{
	struct rb_node *y = x->rb_left, *p = rb_parent(x);

	WRITE_ONCE(x->rb_left, y->rb_right);
	if (y->rb_right)
		set_parent(y->rb_right, x);
	WRITE_ONCE(y->rb_right, x);
	set_parent(y, p);
	change_child(x, y, p, root);
	set_parent(x, y);
	if (aug)
		aug(x, y);
}

static void rb_insert_fixup(struct rb_node *node, struct rb_root *root, rb_rotate_fn aug)
{
	struct rb_node *parent, *gparent, *uncle;

	set_red(node);
	while ((parent = rb_parent(node)) && node_is_red(parent)) {
		gparent = rb_parent(parent);	/* el padre es rojo: no es la raiz */
		if (parent == gparent->rb_left) {
			uncle = gparent->rb_right;
			if (node_is_red(uncle)) {
				set_black(parent);
				set_black(uncle);
				set_red(gparent);
				node = gparent;
				continue;
			}
			if (node == parent->rb_right) {
				rotate_left(parent, root, aug);
				node = parent;
				parent = rb_parent(node);
			}
			set_black(parent);
			set_red(gparent);
			rotate_right(gparent, root, aug);
		} else {
			uncle = gparent->rb_left;
			if (node_is_red(uncle)) {
				set_black(parent);
				set_black(uncle);
				set_red(gparent);
				node = gparent;
				continue;
			}
			if (node == parent->rb_left) {
				rotate_right(parent, root, aug);
				node = parent;
				parent = rb_parent(node);
			}
			set_black(parent);
			set_red(gparent);
			rotate_left(gparent, root, aug);
		}
	}
	set_black(root->rb_node);
}

/*
 * Reequilibrado tras quitar un nodo negro: "node" (posiblemente NULL) lleva
 * un negro de mas y es hijo de "parent".
 */
static void rb_erase_fixup(struct rb_node *parent, struct rb_root *root, rb_rotate_fn aug)
{
	struct rb_node *node = NULL, *sibling;

	while (parent) {
		if (node == parent->rb_left) {
			sibling = parent->rb_right;
			if (node_is_red(sibling)) {
				set_black(sibling);
				set_red(parent);
				rotate_left(parent, root, aug);
				sibling = parent->rb_right;
			}
			if (node_is_black(sibling->rb_left) && node_is_black(sibling->rb_right)) {
				set_red(sibling);
				if (node_is_red(parent)) {
					set_black(parent);
					return;
				}
				node = parent;
				parent = rb_parent(node);
				continue;
			}
			if (node_is_black(sibling->rb_right)) {
				set_black(sibling->rb_left);
				set_red(sibling);
				rotate_right(sibling, root, aug);
				sibling = parent->rb_right;
			}
			copy_color(sibling, parent);
			set_black(parent);
			set_black(sibling->rb_right);
			rotate_left(parent, root, aug);
			return;
		} else {
			sibling = parent->rb_left;
			if (node_is_red(sibling)) {
				set_black(sibling);
				set_red(parent);
				rotate_right(parent, root, aug);
				sibling = parent->rb_left;
			}
			if (node_is_black(sibling->rb_left) && node_is_black(sibling->rb_right)) {
				set_red(sibling);
				if (node_is_red(parent)) {
					set_black(parent);
					return;
				}
				node = parent;
				parent = rb_parent(node);
				continue;
			}
			if (node_is_black(sibling->rb_left)) {
				set_black(sibling->rb_right);
				set_red(sibling);
				rotate_left(sibling, root, aug);
				sibling = parent->rb_left;
			}
			copy_color(sibling, parent);
			set_black(parent);
			set_black(sibling->rb_left);
			rotate_right(parent, root, aug);
			return;
		}
	}
	/* node es la raiz: basta con que sea negra. */
	if (node)
		set_black(node);
}

/* Quita node; devuelve el padre desde el que reequilibrar, o NULL. */
static struct rb_node *rb_erase_unlink(struct rb_node *node, struct rb_root *root,
				       const struct rb_augment_callbacks *augment)
{
	struct rb_node *child = node->rb_right, *tmp = node->rb_left;
	struct rb_node *parent, *rebalance;
	unsigned long pc;

	if (!tmp) {
		/* Sin hijo izquierdo: sube el derecho (si lo hay, es rojo). */
		pc = node->__rb_parent_color;
		parent = rb_parent(node);
		change_child(node, child, parent, root);
		if (child) {
			child->__rb_parent_color = pc;
			rebalance = NULL;
		} else {
			rebalance = (pc & 1) ? parent : NULL;
		}
		tmp = parent;
	} else if (!child) {
		/* Solo hijo izquierdo (rojo): ocupa el sitio y el color. */
		tmp->__rb_parent_color = pc = node->__rb_parent_color;
		parent = rb_parent(node);
		change_child(node, tmp, parent, root);
		rebalance = NULL;
		tmp = parent;
	} else {
		/* Dos hijos: el sucesor (minimo del subarbol derecho) lo sustituye. */
		struct rb_node *successor = child, *child2;

		tmp = child->rb_left;
		if (!tmp) {
			parent = successor;
			child2 = successor->rb_right;
			augment->copy(node, successor);
		} else {
			do {
				parent = successor;
				successor = tmp;
				tmp = tmp->rb_left;
			} while (tmp);
			child2 = successor->rb_right;
			WRITE_ONCE(parent->rb_left, child2);
			WRITE_ONCE(successor->rb_right, child);
			set_parent(child, successor);
			augment->copy(node, successor);
			augment->propagate(parent, successor);
		}
		tmp = node->rb_left;
		WRITE_ONCE(successor->rb_left, tmp);
		set_parent(tmp, successor);
		pc = node->__rb_parent_color;
		tmp = rb_parent(node);
		change_child(node, successor, tmp, root);
		if (child2) {
			child2->__rb_parent_color = (unsigned long)parent | 1;
			rebalance = NULL;
		} else {
			rebalance = node_is_black(successor) ? parent : NULL;
		}
		successor->__rb_parent_color = pc;
		tmp = successor;
	}
	augment->propagate(tmp, NULL);
	return rebalance;
}

static void dummy_propagate(struct rb_node *node, struct rb_node *stop) { }
static void dummy_copy(struct rb_node *old, struct rb_node *new) { }
static const struct rb_augment_callbacks dummy_callbacks = {
	.propagate = dummy_propagate, .copy = dummy_copy, .rotate = NULL,
};

void lkpi_rb_insert_color(struct rb_node *node, struct rb_root *root)
{
	rb_insert_fixup(node, root, NULL);
}
EXPORT_SYMBOL(lkpi_rb_insert_color);

void lkpi_rb_erase(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *rebalance = rb_erase_unlink(node, root, &dummy_callbacks);
	if (rebalance)
		rb_erase_fixup(rebalance, root, NULL);
}
EXPORT_SYMBOL(lkpi_rb_erase);

void lkpi_rb_insert_augmented(struct rb_node *node, struct rb_root *root,
			      const struct rb_augment_callbacks *augment)
{
	rb_insert_fixup(node, root, augment->rotate);
}
EXPORT_SYMBOL(lkpi_rb_insert_augmented);

void lkpi_rb_erase_augmented(struct rb_node *node, struct rb_root *root,
			     const struct rb_augment_callbacks *augment)
{
	struct rb_node *rebalance = rb_erase_unlink(node, root, augment);
	if (rebalance)
		rb_erase_fixup(rebalance, root, augment->rotate);
}
EXPORT_SYMBOL(lkpi_rb_erase_augmented);

struct rb_node *lkpi_rb_first(const struct rb_root *root)
{
	struct rb_node *n = root->rb_node;
	if (!n)
		return NULL;
	while (n->rb_left)
		n = n->rb_left;
	return n;
}
EXPORT_SYMBOL(lkpi_rb_first);

struct rb_node *lkpi_rb_last(const struct rb_root *root)
{
	struct rb_node *n = root->rb_node;
	if (!n)
		return NULL;
	while (n->rb_right)
		n = n->rb_right;
	return n;
}
EXPORT_SYMBOL(lkpi_rb_last);

struct rb_node *lkpi_rb_next(const struct rb_node *node)
{
	struct rb_node *parent;

	if (RB_EMPTY_NODE(node))
		return NULL;
	if (node->rb_right) {
		node = node->rb_right;
		while (node->rb_left)
			node = node->rb_left;
		return (struct rb_node *)node;
	}
	while ((parent = rb_parent(node)) && node == parent->rb_right)
		node = parent;
	return parent;
}
EXPORT_SYMBOL(lkpi_rb_next);

struct rb_node *lkpi_rb_prev(const struct rb_node *node)
{
	struct rb_node *parent;

	if (RB_EMPTY_NODE(node))
		return NULL;
	if (node->rb_left) {
		node = node->rb_left;
		while (node->rb_right)
			node = node->rb_right;
		return (struct rb_node *)node;
	}
	while ((parent = rb_parent(node)) && node == parent->rb_left)
		node = parent;
	return parent;
}
EXPORT_SYMBOL(lkpi_rb_prev);

void lkpi_rb_replace_node(struct rb_node *victim, struct rb_node *new, struct rb_root *root)
{
	struct rb_node *parent = rb_parent(victim);

	*new = *victim;
	if (victim->rb_left)
		set_parent(victim->rb_left, new);
	if (victim->rb_right)
		set_parent(victim->rb_right, new);
	change_child(victim, new, parent, root);
}
EXPORT_SYMBOL(lkpi_rb_replace_node);

static struct rb_node *rb_left_deepest(const struct rb_node *node)
{
	for (;;) {
		if (node->rb_left)
			node = node->rb_left;
		else if (node->rb_right)
			node = node->rb_right;
		else
			return (struct rb_node *)node;
	}
}

struct rb_node *lkpi_rb_next_postorder(const struct rb_node *node)
{
	const struct rb_node *parent;
	if (!node)
		return NULL;
	parent = rb_parent(node);
	if (parent && node == parent->rb_left && parent->rb_right)
		return rb_left_deepest(parent->rb_right);
	return (struct rb_node *)parent;
}
EXPORT_SYMBOL(lkpi_rb_next_postorder);

struct rb_node *lkpi_rb_first_postorder(const struct rb_root *root)
{
	if (!root->rb_node)
		return NULL;
	return rb_left_deepest(root->rb_node);
}
EXPORT_SYMBOL(lkpi_rb_first_postorder);
