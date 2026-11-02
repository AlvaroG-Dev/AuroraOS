/* SPDX-License-Identifier: GPL-2.0-only */
/* LinuxKPI (N2, NVIDIA kernel-open): un solo nodo NUMA. */
#ifndef _LKPI_DMA_MAP_OPS_H
#define _LKPI_DMA_MAP_OPS_H
#include <linux/types.h>
#include <linux/device.h>
#include <linux/dma-direction.h>
struct dma_map_ops {
	dma_addr_t (*map_resource)(struct device *dev, phys_addr_t phys, size_t size,
				   enum dma_data_direction dir, unsigned long attrs);
	void (*unmap_resource)(struct device *dev, dma_addr_t addr, size_t size,
			       enum dma_data_direction dir, unsigned long attrs);
};
/* Aurora no tiene dma_map_ops por dispositivo: siempre DMA directo (como x86 sin IOMMU). */
static inline const struct dma_map_ops *get_dma_ops(struct device *dev) { return NULL; }
#endif
