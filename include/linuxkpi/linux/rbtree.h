/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Arbol rojo-negro intrusivo. Como en Linux, el llamante busca el hueco,
 * enlaza con rb_link_node() y reequilibra con rb_insert_color().
 */
#ifndef _LKPI_LINUX_RBTREE_H
#define _LKPI_LINUX_RBTREE_H

#include <linux/container_of.h>
#include <linux/rbtree_types.h>
#include <linux/stddef.h>
#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/seqlock.h>

#define rb_parent(r) ((struct rb_node *)((r)->__rb_parent_color & ~3))
#define rb_entry(ptr, type, member) container_of(ptr, type, member)
#define RB_EMPTY_ROOT(root) (READ_ONCE((root)->rb_node) == NULL)
#define RB_EMPTY_NODE(node) ((node)->__rb_parent_color == (unsigned long)(node))
#define RB_CLEAR_NODE(node) ((node)->__rb_parent_color = (unsigned long)(node))
#define rb_entry_safe(ptr, type, member) \
	({ typeof(ptr) ____ptr = (ptr); ____ptr ? rb_entry(____ptr, type, member) : NULL; })

void lkpi_rb_insert_color(struct rb_node *node, struct rb_root *root);
void lkpi_rb_erase(struct rb_node *node, struct rb_root *root);
struct rb_node *lkpi_rb_next(const struct rb_node *node);
struct rb_node *lkpi_rb_prev(const struct rb_node *node);
struct rb_node *lkpi_rb_first(const struct rb_root *root);
struct rb_node *lkpi_rb_last(const struct rb_root *root);
struct rb_node *lkpi_rb_first_postorder(const struct rb_root *root);
struct rb_node *lkpi_rb_next_postorder(const struct rb_node *node);
void lkpi_rb_replace_node(struct rb_node *victim, struct rb_node *entry, struct rb_root *root);

#define rb_insert_color lkpi_rb_insert_color
#define rb_erase lkpi_rb_erase
#define rb_next lkpi_rb_next
#define rb_prev lkpi_rb_prev
#define rb_first lkpi_rb_first
#define rb_last lkpi_rb_last
#define rb_first_postorder lkpi_rb_first_postorder
#define rb_next_postorder lkpi_rb_next_postorder
#define rb_replace_node lkpi_rb_replace_node
#define rb_replace_node_rcu lkpi_rb_replace_node

static inline void rb_link_node(struct rb_node *node, struct rb_node *parent,
				struct rb_node **rb_link)
{
	node->__rb_parent_color = (unsigned long)parent;
	node->rb_left = node->rb_right = NULL;
	*rb_link = node;
}
#define rb_link_node_rcu rb_link_node

#define rbtree_postorder_for_each_entry_safe(pos, n, root, field) \
	for (pos = rb_entry_safe(rb_first_postorder(root), typeof(*pos), field); \
	     pos && ({ n = rb_entry_safe(rb_next_postorder(&pos->field), typeof(*pos), field); 1; }); \
	     pos = n)

/* --- Version cacheada (rb_root_cached) --- */
#define rb_first_cached(root) (root)->rb_leftmost
static inline void rb_insert_color_cached(struct rb_node *node, struct rb_root_cached *root,
					  bool leftmost)
{
	if (leftmost)
		root->rb_leftmost = node;
	rb_insert_color(node, &root->rb_root);
}
static inline struct rb_node *rb_erase_cached(struct rb_node *node, struct rb_root_cached *root)
{
	struct rb_node *leftmost = NULL;
	if (root->rb_leftmost == node)
		leftmost = root->rb_leftmost = rb_next(node);
	rb_erase(node, &root->rb_root);
	return leftmost;
}
static inline void rb_replace_node_cached(struct rb_node *victim, struct rb_node *entry,
					  struct rb_root_cached *root)
{
	if (root->rb_leftmost == victim)
		root->rb_leftmost = entry;
	rb_replace_node(victim, entry, &root->rb_root);
}

/* --- Ayudas genericas con comparador (rb_add/rb_find) --- */
static __always_inline struct rb_node *
rb_add_cached(struct rb_node *node, struct rb_root_cached *tree,
	      bool (*less)(struct rb_node *, const struct rb_node *))
{
	struct rb_node **link = &tree->rb_root.rb_node;
	struct rb_node *parent = NULL;
	bool leftmost = true;
	while (*link) {
		parent = *link;
		if (less(node, parent)) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = false;
		}
	}
	rb_link_node(node, parent, link);
	rb_insert_color_cached(node, tree, leftmost);
	return leftmost ? node : NULL;
}
static __always_inline void rb_add(struct rb_node *node, struct rb_root *tree,
				   bool (*less)(struct rb_node *, const struct rb_node *))
{
	struct rb_node **link = &tree->rb_node;
	struct rb_node *parent = NULL;
	while (*link) {
		parent = *link;
		if (less(node, parent))
			link = &parent->rb_left;
		else
			link = &parent->rb_right;
	}
	rb_link_node(node, parent, link);
	rb_insert_color(node, tree);
}
static __always_inline struct rb_node *
rb_find_add(struct rb_node *node, struct rb_root *tree,
	    int (*cmp)(struct rb_node *, const struct rb_node *))
{
	struct rb_node **link = &tree->rb_node;
	struct rb_node *parent = NULL;
	int c;
	while (*link) {
		parent = *link;
		c = cmp(node, parent);
		if (c < 0)
			link = &parent->rb_left;
		else if (c > 0)
			link = &parent->rb_right;
		else
			return parent;
	}
	rb_link_node(node, parent, link);
	rb_insert_color(node, tree);
	return NULL;
}
static __always_inline struct rb_node *
rb_find(const void *key, const struct rb_root *tree,
	int (*cmp)(const void *key, const struct rb_node *))
{
	struct rb_node *node = tree->rb_node;
	while (node) {
		int c = cmp(key, node);
		if (c < 0)
			node = node->rb_left;
		else if (c > 0)
			node = node->rb_right;
		else
			return node;
	}
	return NULL;
}
static __always_inline struct rb_node *
rb_find_first(const void *key, const struct rb_root *tree,
	      int (*cmp)(const void *key, const struct rb_node *))
{
	struct rb_node *node = tree->rb_node, *match = NULL;
	while (node) {
		int c = cmp(key, node);
		if (c <= 0) {
			if (!c)
				match = node;
			node = node->rb_left;
		} else {
			node = node->rb_right;
		}
	}
	return match;
}
static __always_inline struct rb_node *
rb_next_match(const void *key, struct rb_node *node,
	      int (*cmp)(const void *key, const struct rb_node *))
{
	node = rb_next(node);
	if (node && cmp(key, node))
		node = NULL;
	return node;
}
#define rb_for_each(node, key, tree, cmp) \
	for ((node) = rb_find_first((key), (tree), (cmp)); (node); \
	     (node) = rb_next_match((key), (node), (cmp)))

#endif
