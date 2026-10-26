/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DMA, version PROVISIONAL de L3: memoria coherente y mascaras de
 * direccion. Sin IOMMU (L9): la direccion de bus es la fisica. x86 es
 * coherente, asi que la memoria es WB normal.
 *
 * dma_alloc_coherent() respeta coherent_dma_mask (paginas por debajo del
 * limite, pmm_alloc_pages_below de Aurora) y alinea al orden del tamano, como
 * Linux. dma_map_*, sg_table, bounce buffers y dma_buf llegan con L4.
 */
#ifndef _LKPI_LINUX_DMA_MAPPING_H
#define _LKPI_LINUX_DMA_MAPPING_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/dma-direction.h>
#include <linux/err.h>

#define DMA_BIT_MASK(n) (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))
#define DMA_MAPPING_ERROR (~(dma_addr_t)0)

#define DMA_ATTR_WEAK_ORDERING (1UL << 1)
#define DMA_ATTR_WRITE_COMBINE (1UL << 2)
#define DMA_ATTR_NO_KERNEL_MAPPING (1UL << 4)
#define DMA_ATTR_SKIP_CPU_SYNC (1UL << 5)
#define DMA_ATTR_FORCE_CONTIGUOUS (1UL << 6)
#define DMA_ATTR_ALLOC_SINGLE_PAGES (1UL << 7)
#define DMA_ATTR_NO_WARN (1UL << 8)

void *lkpi_dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle,
			   gfp_t gfp, unsigned long attrs);
void lkpi_dma_free_attrs(struct device *dev, size_t size, void *cpu_addr,
			 dma_addr_t dma_handle, unsigned long attrs);
void *lkpi_dmam_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle,
			    gfp_t gfp, unsigned long attrs);
void lkpi_dmam_free_coherent(struct device *dev, size_t size, void *vaddr,
			     dma_addr_t dma_handle);
int lkpi_dma_set_mask(struct device *dev, u64 mask);
int lkpi_dma_set_coherent_mask(struct device *dev, u64 mask);
int lkpi_dma_supported(struct device *dev, u64 mask);

#define dma_alloc_attrs(d, s, h, g, a) lkpi_dma_alloc_attrs(d, s, h, g, a)
#define dma_free_attrs(d, s, c, h, a) lkpi_dma_free_attrs(d, s, c, h, a)
#define dma_set_mask(d, m) lkpi_dma_set_mask(d, m)
#define dma_set_coherent_mask(d, m) lkpi_dma_set_coherent_mask(d, m)
#define dma_supported(d, m) lkpi_dma_supported(d, m)

static inline void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle,
				       gfp_t gfp)
{
	return dma_alloc_attrs(dev, size, dma_handle, gfp, 0);
}
static inline void dma_free_coherent(struct device *dev, size_t size, void *cpu_addr,
				     dma_addr_t dma_handle)
{
	dma_free_attrs(dev, size, cpu_addr, dma_handle, 0);
}
static inline void *dmam_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle,
					gfp_t gfp)
{
	return lkpi_dmam_alloc_attrs(dev, size, dma_handle, gfp, 0);
}
#define dmam_free_coherent(d, s, v, h) lkpi_dmam_free_coherent(d, s, v, h)
static inline int dma_set_mask_and_coherent(struct device *dev, u64 mask)
{
	int rc = dma_set_mask(dev, mask);
	if (rc == 0)
		dma_set_coherent_mask(dev, mask);
	return rc;
}
static inline int dma_coerce_mask_and_coherent(struct device *dev, u64 mask)
{
	dev->dma_mask = &dev->coherent_dma_mask;
	return dma_set_mask_and_coherent(dev, mask);
}
static inline u64 dma_get_mask(struct device *dev)
{
	if (dev->dma_mask && *dev->dma_mask)
		return *dev->dma_mask;
	return DMA_BIT_MASK(32);
}
static inline int dma_mapping_error(struct device *dev, dma_addr_t dma_addr)
{
	return dma_addr == DMA_MAPPING_ERROR ? -ENOMEM : 0;
}
static inline unsigned int dma_get_max_seg_size(struct device *dev)
{
	return dev->dma_parms ? dev->dma_parms->max_segment_size : 0x10000;
}
static inline int dma_set_max_seg_size(struct device *dev, unsigned int size)
{
	if (!dev->dma_parms)
		return -EIO;
	dev->dma_parms->max_segment_size = size;
	return 0;
}
static inline bool dma_addressing_limited(struct device *dev) { return false; }
static inline size_t dma_max_mapping_size(struct device *dev) { return SIZE_MAX; }

#endif
