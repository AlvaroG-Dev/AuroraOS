/* SPDX-License-Identifier: GPL-2.0-only */
/* Pools de bloques pequenos de memoria coherente (K1-DMA, L4). */
#ifndef _LKPI_LINUX_DMAPOOL_H
#define _LKPI_LINUX_DMAPOOL_H

#include <linux/types.h>
#include <linux/gfp.h>

struct device;
struct dma_pool;

struct dma_pool *lkpi_dma_pool_create(const char *name, struct device *dev, size_t size,
				      size_t align, size_t boundary);
void lkpi_dma_pool_destroy(struct dma_pool *pool);
void *lkpi_dma_pool_alloc(struct dma_pool *pool, gfp_t mem_flags, dma_addr_t *handle);
void lkpi_dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t addr);
struct dma_pool *lkpi_dmam_pool_create(const char *name, struct device *dev, size_t size,
				       size_t align, size_t allocation);
#define dma_pool_create(n, d, s, a, b) lkpi_dma_pool_create(n, d, s, a, b)
#define dma_pool_destroy(p) lkpi_dma_pool_destroy(p)
#define dma_pool_alloc(p, f, h) lkpi_dma_pool_alloc(p, f, h)
#define dma_pool_free(p, v, a) lkpi_dma_pool_free(p, v, a)
#define dmam_pool_create(n, d, s, a, b) lkpi_dmam_pool_create(n, d, s, a, b)
static inline void *dma_pool_zalloc(struct dma_pool *pool, gfp_t mem_flags, dma_addr_t *handle)
{
	return lkpi_dma_pool_alloc(pool, mem_flags | __GFP_ZERO, handle);
}

#endif
