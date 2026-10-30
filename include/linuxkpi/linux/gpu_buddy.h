/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Interfaz del asignador buddy de GPU (drivers/gpu/buddy.c de Linux). Los
 * nombres y la disposicion de los campos los fija el codigo de Linux que se
 * compila tal cual; el texto y los comentarios son propios.
 */
#ifndef _LKPI_LINUX_GPU_BUDDY_H
#define _LKPI_LINUX_GPU_BUDDY_H

#include <linux/bitops.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/rbtree.h>
#include <linux/align.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/minmax.h>

/* Banderas de gpu_buddy_alloc_blocks() y de la lista que se libera. */
#define GPU_BUDDY_RANGE_ALLOCATION	BIT(0)
#define GPU_BUDDY_TOPDOWN_ALLOCATION	BIT(1)
#define GPU_BUDDY_CONTIGUOUS_ALLOCATION	BIT(2)
#define GPU_BUDDY_CLEAR_ALLOCATION	BIT(3)
#define GPU_BUDDY_CLEARED		BIT(4)
#define GPU_BUDDY_TRIM_DISABLE		BIT(5)

/* Hay un arbol de bloques libres limpios (a cero) y otro de sucios. */
enum gpu_buddy_free_tree {
	GPU_BUDDY_CLEAR_TREE = 0,
	GPU_BUDDY_DIRTY_TREE,
	GPU_BUDDY_MAX_FREE_TREES,
};

#define for_each_free_tree(tree) \
	for ((tree) = 0; (tree) < GPU_BUDDY_MAX_FREE_TREES; (tree)++)

/*
 * Un bloque del arbol binario. `header` empaqueta desplazamiento (bits 63:12),
 * estado (11:10), "limpio" (9) y orden (5:0).
 */
struct gpu_buddy_block {
#define GPU_BUDDY_HEADER_OFFSET GENMASK_ULL(63, 12)
#define GPU_BUDDY_HEADER_STATE  GENMASK_ULL(11, 10)
#define   GPU_BUDDY_ALLOCATED	   (1 << 10)
#define   GPU_BUDDY_FREE	   (2 << 10)
#define   GPU_BUDDY_SPLIT	   (3 << 10)
#define GPU_BUDDY_HEADER_CLEAR  GENMASK_ULL(9, 9)
#define GPU_BUDDY_HEADER_UNUSED GENMASK_ULL(8, 6)
#define GPU_BUDDY_HEADER_ORDER  GENMASK_ULL(5, 0)
	u64 header;

	struct gpu_buddy_block *left;
	struct gpu_buddy_block *right;
	struct gpu_buddy_block *parent;

	void *private;	/* del que lo creo */

	/* Mientras esta asignado, `link` es del usuario (para sus listas). */
	union {
		struct rb_node rb;
		struct list_head link;
	};

	struct list_head tmp_link;
};

/* El orden cero es, como minimo, SZ_4K. */
#define GPU_BUDDY_MAX_ORDER (63 - 12)

/* El bloqueo lo pone el usuario (un mutex alrededor de alloc/free basta). */
struct gpu_buddy {
	struct rb_root **free_trees;	/* privado: listas libres por orden */
	struct gpu_buddy_block **roots;	/* privado: raices del espacio de direcciones */

	/* Lo siguiente es publico y no cambia durante la vida del gestor. */
	unsigned int n_roots;
	unsigned int max_order;
	u64 chunk_size;			/* al menos SZ_4K */
	u64 size;
	u64 avail;
	u64 clear_avail;
};

static inline u64 gpu_buddy_block_offset(const struct gpu_buddy_block *block)
{
	return block->header & GPU_BUDDY_HEADER_OFFSET;
}

static inline unsigned int gpu_buddy_block_order(struct gpu_buddy_block *block)
{
	return block->header & GPU_BUDDY_HEADER_ORDER;
}

static inline unsigned int gpu_buddy_block_state(struct gpu_buddy_block *block)
{
	return block->header & GPU_BUDDY_HEADER_STATE;
}

static inline bool gpu_buddy_block_is_allocated(struct gpu_buddy_block *block)
{
	return gpu_buddy_block_state(block) == GPU_BUDDY_ALLOCATED;
}

static inline bool gpu_buddy_block_is_clear(struct gpu_buddy_block *block)
{
	return block->header & GPU_BUDDY_HEADER_CLEAR;
}

static inline bool gpu_buddy_block_is_free(struct gpu_buddy_block *block)
{
	return gpu_buddy_block_state(block) == GPU_BUDDY_FREE;
}

static inline bool gpu_buddy_block_is_split(struct gpu_buddy_block *block)
{
	return gpu_buddy_block_state(block) == GPU_BUDDY_SPLIT;
}

static inline u64 gpu_buddy_block_size(struct gpu_buddy *mm, struct gpu_buddy_block *block)
{
	return mm->chunk_size << gpu_buddy_block_order(block);
}

int gpu_buddy_init(struct gpu_buddy *mm, u64 size, u64 chunk_size);
void gpu_buddy_fini(struct gpu_buddy *mm);
struct gpu_buddy_block *gpu_get_buddy(struct gpu_buddy_block *block);
int gpu_buddy_alloc_blocks(struct gpu_buddy *mm, u64 start, u64 end, u64 size, u64 min_page_size,
			   struct list_head *blocks, unsigned long flags);
int gpu_buddy_block_trim(struct gpu_buddy *mm, u64 *start, u64 new_size, struct list_head *blocks);
void gpu_buddy_reset_clear(struct gpu_buddy *mm, bool is_clear);
void gpu_buddy_free_block(struct gpu_buddy *mm, struct gpu_buddy_block *block);
void gpu_buddy_free_list(struct gpu_buddy *mm, struct list_head *objects, unsigned int flags);
void gpu_buddy_print(struct gpu_buddy *mm);
void gpu_buddy_block_print(struct gpu_buddy *mm, struct gpu_buddy_block *block);

#endif
