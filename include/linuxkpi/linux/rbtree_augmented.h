/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rbtree aumentado: cada nodo guarda un dato derivado de su subarbol (p.
 * ej. el maximo de un intervalo) que se mantiene con tres callbacks.
 */
#ifndef _LKPI_LINUX_RBTREE_AUGMENTED_H
#define _LKPI_LINUX_RBTREE_AUGMENTED_H

#include <linux/compiler.h>
#include <linux/rbtree.h>

struct rb_augment_callbacks {
	void (*propagate)(struct rb_node *node, struct rb_node *stop);
	void (*copy)(struct rb_node *old, struct rb_node *new);
	void (*rotate)(struct rb_node *old, struct rb_node *new);
};

void lkpi_rb_insert_augmented(struct rb_node *node, struct rb_root *root,
			      const struct rb_augment_callbacks *augment);
void lkpi_rb_erase_augmented(struct rb_node *node, struct rb_root *root,
			     const struct rb_augment_callbacks *augment);

static inline void rb_insert_augmented(struct rb_node *node, struct rb_root *root,
				       const struct rb_augment_callbacks *augment)
{
	lkpi_rb_insert_augmented(node, root, augment);
}
static inline void rb_insert_augmented_cached(struct rb_node *node,
					      struct rb_root_cached *root, bool newleft,
					      const struct rb_augment_callbacks *augment)
{
	if (newleft)
		root->rb_leftmost = node;
	rb_insert_augmented(node, &root->rb_root, augment);
}
static inline void rb_erase_augmented(struct rb_node *node, struct rb_root *root,
				      const struct rb_augment_callbacks *augment)
{
	lkpi_rb_erase_augmented(node, root, augment);
}
static inline void rb_erase_augmented_cached(struct rb_node *node, struct rb_root_cached *root,
					     const struct rb_augment_callbacks *augment)
{
	if (root->rb_leftmost == node)
		root->rb_leftmost = rb_next(node);
	rb_erase_augmented(node, &root->rb_root, augment);
}

/*
 * RB_DECLARE_CALLBACKS(static, nombre, struct T, campo_rb, campo_aug, calc)
 * calc(node, exit) recalcula node->campo_aug y devuelve true si no cambio
 * (y exit es true), para parar la propagacion.
 */
#define RB_DECLARE_CALLBACKS(RBSTATIC, RBNAME, RBSTRUCT, RBFIELD, RBAUGMENTED, RBCOMPUTE) \
static inline void RBNAME##_propagate(struct rb_node *rb, struct rb_node *stop) \
{ \
	while (rb != stop) { \
		RBSTRUCT *node = rb_entry(rb, RBSTRUCT, RBFIELD); \
		if (RBCOMPUTE(node, true)) \
			break; \
		rb = rb_parent(&node->RBFIELD); \
	} \
} \
static inline void RBNAME##_copy(struct rb_node *rb_old, struct rb_node *rb_new) \
{ \
	RBSTRUCT *old = rb_entry(rb_old, RBSTRUCT, RBFIELD); \
	RBSTRUCT *new = rb_entry(rb_new, RBSTRUCT, RBFIELD); \
	new->RBAUGMENTED = old->RBAUGMENTED; \
} \
static void RBNAME##_rotate(struct rb_node *rb_old, struct rb_node *rb_new) \
{ \
	RBSTRUCT *old = rb_entry(rb_old, RBSTRUCT, RBFIELD); \
	RBSTRUCT *new = rb_entry(rb_new, RBSTRUCT, RBFIELD); \
	new->RBAUGMENTED = old->RBAUGMENTED; \
	RBCOMPUTE(old, false); \
} \
RBSTATIC const struct rb_augment_callbacks RBNAME = { \
	.propagate = RBNAME##_propagate, \
	.copy = RBNAME##_copy, \
	.rotate = RBNAME##_rotate \
};

/* Variante para "maximo de un campo escalar en el subarbol". */
#define RB_DECLARE_CALLBACKS_MAX(RBSTATIC, RBNAME, RBSTRUCT, RBFIELD, RBTYPE, RBAUGMENTED, RBCOMPUTE) \
static inline bool RBNAME##_compute_max(RBSTRUCT *node, bool exit) \
{ \
	RBSTRUCT *child; \
	RBTYPE max = RBCOMPUTE(node); \
	if (node->RBFIELD.rb_left) { \
		child = rb_entry(node->RBFIELD.rb_left, RBSTRUCT, RBFIELD); \
		if (child->RBAUGMENTED > max) \
			max = child->RBAUGMENTED; \
	} \
	if (node->RBFIELD.rb_right) { \
		child = rb_entry(node->RBFIELD.rb_right, RBSTRUCT, RBFIELD); \
		if (child->RBAUGMENTED > max) \
			max = child->RBAUGMENTED; \
	} \
	if (exit && node->RBAUGMENTED == max) \
		return true; \
	node->RBAUGMENTED = max; \
	return false; \
} \
RB_DECLARE_CALLBACKS(RBSTATIC, RBNAME, RBSTRUCT, RBFIELD, RBAUGMENTED, RBNAME##_compute_max)

#define RB_RED 0
#define RB_BLACK 1
#define __rb_parent(pc) ((struct rb_node *)((pc) & ~3))
#define __rb_color(pc) ((pc) & 1)
#define __rb_is_black(pc) __rb_color(pc)
#define __rb_is_red(pc) (!__rb_color(pc))
#define rb_color(rb) __rb_color((rb)->__rb_parent_color)
#define rb_is_red(rb) __rb_is_red((rb)->__rb_parent_color)
#define rb_is_black(rb) __rb_is_black((rb)->__rb_parent_color)

#endif
