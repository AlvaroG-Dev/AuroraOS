// SPDX-License-Identifier: GPL-2.0-only
/*
 * DMA PROVISIONAL de L3: memoria coherente y mascaras. Sin IOMMU, la
 * direccion de bus es la fisica; x86 es coherente (memoria WB normal, la de
 * la ventana fisica de Aurora). Respeta coherent_dma_mask: si no toda la RAM
 * cabe bajo la mascara, pide las paginas por debajo del limite
 * (pmm_alloc_pages_below). dma_map_*, sg_table y bounce buffers: L4.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/string.h>
#include "../aurora.h"
#include "../lkpi_internal.h"

void *lkpi_dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t gfp,
			   unsigned long attrs)
{
	u64 mask = dev && dev->coherent_dma_mask ? dev->coherent_dma_mask : DMA_BIT_MASK(32);
	unsigned int order;
	u64 count, phys = 0, ram_end;
	void *va;

	if (!size)
		return NULL;
	order = get_order(size);
	count = 1ULL << order;	/* alineado al orden del tamano, como Linux */
	ram_end = aurora_pmm_total_pages() << PAGE_SHIFT;
	if (mask >= ram_end - 1)
		phys = aurora_pmm_alloc_pages(count);
	if (phys && phys + (count << PAGE_SHIFT) - 1 > mask) {
		aurora_pmm_free_pages(phys, count);
		phys = 0;
	}
	if (!phys)
		phys = aurora_pmm_alloc_pages_below(count, mask == ~0ULL ? ~0ULL : mask + 1);
	if (!phys) {
		if (!(attrs & DMA_ATTR_NO_WARN))
			dev_warn(dev, "dma_alloc_coherent: sin %zu bytes bajo la mascara %llx\n", size,
				 (unsigned long long)mask);
		return NULL;
	}
	va = (void *)(AURORA_PHYS_MAP_BASE + phys);
	memset(va, 0, count << PAGE_SHIFT);
	*dma_handle = phys;
	return va;
}
EXPORT_SYMBOL(lkpi_dma_alloc_attrs);

void lkpi_dma_free_attrs(struct device *dev, size_t size, void *cpu_addr, dma_addr_t dma_handle,
			 unsigned long attrs)
{
	if (!cpu_addr)
		return;
	WARN_ON((u64)cpu_addr - AURORA_PHYS_MAP_BASE != dma_handle);
	aurora_pmm_free_pages(dma_handle, 1ULL << get_order(size));
}
EXPORT_SYMBOL(lkpi_dma_free_attrs);

struct lkpi_dmam_devres {
	size_t size;
	void *vaddr;
	dma_addr_t dma_handle;
	unsigned long attrs;
};

static void lkpi_dmam_release(struct device *dev, void *res)
{
	struct lkpi_dmam_devres *r = res;

	lkpi_dma_free_attrs(dev, r->size, r->vaddr, r->dma_handle, r->attrs);
}

static int lkpi_dmam_match(struct device *dev, void *res, void *match_data)
{
	struct lkpi_dmam_devres *a = res, *b = match_data;

	if (a->vaddr != b->vaddr)
		return 0;
	WARN_ON(a->size != b->size || a->dma_handle != b->dma_handle);
	return 1;
}

void *lkpi_dmam_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t gfp,
			    unsigned long attrs)
{
	struct lkpi_dmam_devres *r;
	void *vaddr;

	r = devres_alloc(lkpi_dmam_release, sizeof(*r), gfp);
	if (!r)
		return NULL;
	vaddr = lkpi_dma_alloc_attrs(dev, size, dma_handle, gfp, attrs);
	if (!vaddr) {
		devres_free(r);
		return NULL;
	}
	r->vaddr = vaddr;
	r->dma_handle = *dma_handle;
	r->size = size;
	r->attrs = attrs;
	devres_add(dev, r);
	return vaddr;
}
EXPORT_SYMBOL(lkpi_dmam_alloc_attrs);

void lkpi_dmam_free_coherent(struct device *dev, size_t size, void *vaddr, dma_addr_t dma_handle)
{
	struct lkpi_dmam_devres match = { size, vaddr, dma_handle, 0 };

	WARN_ON(devres_release(dev, lkpi_dmam_release, lkpi_dmam_match, &match));
}
EXPORT_SYMBOL(lkpi_dmam_free_coherent);

int lkpi_dma_supported(struct device *dev, u64 mask)
{
	/* Lo minimo que Linux acepta para memoria coherente (ISA: 24 bits). */
	return mask >= DMA_BIT_MASK(24);
}
EXPORT_SYMBOL(lkpi_dma_supported);

int lkpi_dma_set_mask(struct device *dev, u64 mask)
{
	if (!dev->dma_mask || !lkpi_dma_supported(dev, mask))
		return -EIO;
	*dev->dma_mask = mask;
	return 0;
}
EXPORT_SYMBOL(lkpi_dma_set_mask);

int lkpi_dma_set_coherent_mask(struct device *dev, u64 mask)
{
	if (!lkpi_dma_supported(dev, mask))
		return -EIO;
	dev->coherent_dma_mask = mask;
	return 0;
}
EXPORT_SYMBOL(lkpi_dma_set_coherent_mask);
