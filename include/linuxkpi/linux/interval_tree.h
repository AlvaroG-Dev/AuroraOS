/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_INTERVAL_TREE_H
#define _LKPI_LINUX_INTERVAL_TREE_H

#include <linux/rbtree.h>

struct interval_tree_node {
	struct rb_node rb;
	unsigned long start;
	unsigned long last;
	unsigned long __subtree_last;
};

void lkpi_interval_tree_insert(struct interval_tree_node *node, struct rb_root_cached *root);
void lkpi_interval_tree_remove(struct interval_tree_node *node, struct rb_root_cached *root);
struct interval_tree_node *lkpi_interval_tree_iter_first(struct rb_root_cached *root,
							 unsigned long start, unsigned long last);
struct interval_tree_node *lkpi_interval_tree_iter_next(struct interval_tree_node *node,
							unsigned long start, unsigned long last);
#define interval_tree_insert lkpi_interval_tree_insert
#define interval_tree_remove lkpi_interval_tree_remove
#define interval_tree_iter_first lkpi_interval_tree_iter_first
#define interval_tree_iter_next lkpi_interval_tree_iter_next

#endif
