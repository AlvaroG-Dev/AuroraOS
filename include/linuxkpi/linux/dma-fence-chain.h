/* SPDX-License-Identifier: GPL-2.0-only */
/* dma_fence_chain (K7, L5): una linea temporal de fences encadenados. */
#ifndef _LKPI_LINUX_DMA_FENCE_CHAIN_H
#define _LKPI_LINUX_DMA_FENCE_CHAIN_H

#include <linux/dma-fence.h>
#include <linux/irq_work.h>
#include <linux/slab.h>

#define dma_fence_chain_walk lkpi_dma_fence_chain_walk
#define dma_fence_chain_find_seqno lkpi_dma_fence_chain_find_seqno
#define dma_fence_chain_init lkpi_dma_fence_chain_init

struct dma_fence_chain {
	struct dma_fence base;
	struct dma_fence __rcu *prev;
	u64 prev_seqno;
	struct dma_fence *fence;
	union {
		struct dma_fence_cb cb;		/* para senalar al terminar */
		struct irq_work work;		/* para anadir el cb sin invertir cerrojos */
	};
	spinlock_t lock;
};

static inline struct dma_fence_chain *to_dma_fence_chain(struct dma_fence *fence)
{
	if (!fence || !dma_fence_is_chain(fence))
		return NULL;
	return container_of(fence, struct dma_fence_chain, base);
}

static inline struct dma_fence *dma_fence_chain_contained(struct dma_fence *fence)
{
	struct dma_fence_chain *chain = to_dma_fence_chain(fence);

	return chain ? chain->fence : fence;
}

#define dma_fence_chain_alloc() \
	((struct dma_fence_chain *)kmalloc(sizeof(struct dma_fence_chain), GFP_KERNEL))

static inline void dma_fence_chain_free(struct dma_fence_chain *chain)
{
	kfree(chain);
}

#define dma_fence_chain_for_each(iter, head) \
	for (iter = dma_fence_get(head); iter; iter = dma_fence_chain_walk(iter))

struct dma_fence *dma_fence_chain_walk(struct dma_fence *fence);
int dma_fence_chain_find_seqno(struct dma_fence **pfence, uint64_t seqno);
void dma_fence_chain_init(struct dma_fence_chain *chain, struct dma_fence *prev,
			  struct dma_fence *fence, uint64_t seqno);

#endif
