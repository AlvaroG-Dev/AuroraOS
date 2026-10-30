/* SPDX-License-Identifier: GPL-2.0-only */
/* Recorrer y fusionar fences dentro de chains y arrays (K7, L5). */
#ifndef _LKPI_LINUX_DMA_FENCE_UNWRAP_H
#define _LKPI_LINUX_DMA_FENCE_UNWRAP_H

#include <linux/kernel.h>

#define dma_fence_unwrap_first lkpi_dma_fence_unwrap_first
#define dma_fence_unwrap_next lkpi_dma_fence_unwrap_next
#define __dma_fence_unwrap_merge lkpi___dma_fence_unwrap_merge
#define dma_fence_dedup_array lkpi_dma_fence_dedup_array

struct dma_fence;

struct dma_fence_unwrap {
	struct dma_fence *chain;	/* chain actual (o NULL) */
	struct dma_fence *array;	/* array actual (o NULL) */
	unsigned int index;		/* indice dentro del array */
};

struct dma_fence *dma_fence_unwrap_first(struct dma_fence *head,
					 struct dma_fence_unwrap *cursor);
struct dma_fence *dma_fence_unwrap_next(struct dma_fence_unwrap *cursor);

#define dma_fence_unwrap_for_each(fence, cursor, head) \
	for (fence = dma_fence_unwrap_first(head, cursor); fence; \
	     fence = dma_fence_unwrap_next(cursor))

struct dma_fence *__dma_fence_unwrap_merge(unsigned int num_fences, struct dma_fence **fences,
					   struct dma_fence_unwrap *cursors);
int dma_fence_dedup_array(struct dma_fence **array, int num_fences);

#define dma_fence_unwrap_merge(...) \
	({ \
		struct dma_fence *__f[] = { __VA_ARGS__ }; \
		struct dma_fence_unwrap __c[ARRAY_SIZE(__f)]; \
		\
		__dma_fence_unwrap_merge(ARRAY_SIZE(__f), __f, __c); \
	})

#endif
