/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_RBTREE_TYPES_H
#define _LKPI_LINUX_RBTREE_TYPES_H

struct rb_node {
	unsigned long __rb_parent_color;	/* padre | color (bit 0: 1 = negro) */
	struct rb_node *rb_right;
	struct rb_node *rb_left;
} __attribute__((aligned(sizeof(long))));

struct rb_root {
	struct rb_node *rb_node;
};

/* Raiz con el nodo mas a la izquierda cacheado (rb_first en O(1)). */
struct rb_root_cached {
	struct rb_root rb_root;
	struct rb_node *rb_leftmost;
};

#define RB_ROOT (struct rb_root) { NULL, }
#define RB_ROOT_CACHED (struct rb_root_cached) { {NULL, }, NULL }

#endif
