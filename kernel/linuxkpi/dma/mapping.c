// SPDX-License-Identifier: GPL-2.0-only
/*
 * DMA (K1-DMA, L4): memoria coherente, mapeos de streaming con bounce,
 * mapeo a usuario y mascaras. Sin IOMMU (L9) la direccion de bus es la fisica
 * y x86 es coherente: las sincronizaciones solo copian cuando hay bounce.
 *
 * Bounce: si [fis, fis + len) no cabe en la mascara del dispositivo, se
 * reservan paginas por debajo de ella (pmm_alloc_pages_below, se puede
 * llamar con las IRQ desactivadas) y se copia segun la direccion: al mapear
 * hacia el dispositivo, al desmapear o sync_for_cpu desde el dispositivo. Sin
 * pool fijo: cada mapeo reserva lo suyo y lo devuelve al desmapear.
 *
 * DMA_ATTR_WRITE_COMBINE: la memoria se ve por un mapeo WC en la region vmap
 * de Aurora (la ventana directa sigue siendo WB: alias de tipos de memoria
 * hasta poder cambiar la ventana, ver docs/LinuxKPI_Sesion_L4.md).
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include "../aurora.h"
#include "../lkpi_internal.h"

struct lkpi_dma_stats lkpi_dma_stats;
EXPORT_SYMBOL(lkpi_dma_stats);

static inline bool lkpi_is_vmap_addr(const void *p)
{
	unsigned long a = (unsigned long)p;
	return a >= AURORA_VMAP_BASE && a < AURORA_VMAP_BASE + AURORA_VMAP_SIZE;
}

/* Paginas contiguas por debajo de `mask` (alineadas al orden). 0 si no hay. */
static u64 lkpi_dma_pages_below(u64 count, u64 mask)
{
	u64 phys = 0, ram_end = aurora_pmm_total_pages() << PAGE_SHIFT;

	if (mask >= ram_end - 1)
		phys = aurora_pmm_alloc_pages(count);
	if (phys && phys + (count << PAGE_SHIFT) - 1 > mask) {
		aurora_pmm_free_pages(phys, count);
		phys = 0;
	}
	if (!phys)
		phys = aurora_pmm_alloc_pages_below(count, mask == ~0ULL ? ~0ULL : mask + 1);
	return phys;
}

static u64 lkpi_dev_mask(struct device *dev)
{
	if (dev && dev->dma_mask && *dev->dma_mask)
		return *dev->dma_mask;
	return DMA_BIT_MASK(32);
}

/* ------------------------------------------------------- coherente */

void *lkpi_dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t gfp,
			   unsigned long attrs)
{
	u64 mask = dev && dev->coherent_dma_mask ? dev->coherent_dma_mask : DMA_BIT_MASK(32);
	unsigned int order;
	u64 count, phys, i;
	void *va;

	if (!size)
		return NULL;
	order = get_order(size);
	count = 1ULL << order;	/* alineado al orden del tamano, como Linux */
	phys = lkpi_dma_pages_below(count, mask);
	if (!phys) {
		if (!(attrs & DMA_ATTR_NO_WARN))
			dev_warn(dev, "dma_alloc_coherent: sin %zu bytes bajo la mascara %llx\n", size,
				 (unsigned long long)mask);
		return NULL;
	}
	memset((void *)(AURORA_PHYS_MAP_BASE + phys), 0, count << PAGE_SHIFT);
	*dma_handle = phys;
	if (attrs & DMA_ATTR_WRITE_COMBINE) {
		u64 vaddr = aurora_vmap_area_alloc(count << PAGE_SHIFT);

		if (!vaddr)
			goto err;
		for (i = 0; i < count; i++)
			if (aurora_vmap_area_map(vaddr + (i << PAGE_SHIFT), phys + (i << PAGE_SHIFT),
						 AURORA_PTE_WRITABLE | AURORA_PTE_NX |
						 AURORA_PTE_WRITETHRU | AURORA_PTE_NOCACHE)) {
				aurora_vmap_area_free(vaddr);
				goto err;
			}
		return (void *)vaddr;
	}
	/* NO_KERNEL_MAPPING: la ventana directa sirve de cookie. */
	va = (void *)(AURORA_PHYS_MAP_BASE + phys);
	return va;
err:
	aurora_pmm_free_pages(phys, count);
	return NULL;
}
EXPORT_SYMBOL(lkpi_dma_alloc_attrs);

void lkpi_dma_free_attrs(struct device *dev, size_t size, void *cpu_addr, dma_addr_t dma_handle,
			 unsigned long attrs)
{
	if (!cpu_addr)
		return;
	if (lkpi_is_vmap_addr(cpu_addr))
		aurora_vmap_area_free((u64)cpu_addr);
	else
		WARN_ON((u64)cpu_addr - AURORA_PHYS_MAP_BASE != dma_handle);
	aurora_pmm_free_pages(dma_handle, 1ULL << get_order(size));
}
EXPORT_SYMBOL(lkpi_dma_free_attrs);

struct page *lkpi_dma_alloc_pages(struct device *dev, size_t size, dma_addr_t *dma_handle,
				  enum dma_data_direction dir, gfp_t gfp)
{
	void *va = lkpi_dma_alloc_attrs(dev, size, dma_handle, gfp, 0);

	return va ? virt_to_page(va) : NULL;
}
EXPORT_SYMBOL(lkpi_dma_alloc_pages);

void lkpi_dma_free_pages(struct device *dev, size_t size, struct page *page,
			 dma_addr_t dma_handle, enum dma_data_direction dir)
{
	lkpi_dma_free_attrs(dev, size, page_address(page), dma_handle, 0);
}
EXPORT_SYMBOL(lkpi_dma_free_pages);

int lkpi_dma_mmap_attrs(struct device *dev, struct vm_area_struct *vma, void *cpu_addr,
			dma_addr_t dma_addr, size_t size, unsigned long attrs)
{
	unsigned long user_count = vma_pages(vma);
	unsigned long count = PAGE_ALIGN(size) >> PAGE_SHIFT;
	unsigned long off = vma->vm_pgoff;

	if (off >= count || user_count > count - off)
		return -ENXIO;
	if (attrs & DMA_ATTR_WRITE_COMBINE)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start, (dma_addr >> PAGE_SHIFT) + off,
			       user_count << PAGE_SHIFT, vma->vm_page_prot);
}
EXPORT_SYMBOL(lkpi_dma_mmap_attrs);

int lkpi_dma_get_sgtable_attrs(struct device *dev, struct sg_table *sgt, void *cpu_addr,
			       dma_addr_t dma_addr, size_t size, unsigned long attrs)
{
	int rc = sg_alloc_table(sgt, 1, GFP_KERNEL);

	if (!rc)
		sg_set_page(sgt->sgl, pfn_to_page(dma_addr >> PAGE_SHIFT), PAGE_ALIGN(size), 0);
	return rc;
}
EXPORT_SYMBOL(lkpi_dma_get_sgtable_attrs);

/* --------------------------------------------------------- bounce */

struct lkpi_bounce {
	struct list_head list;
	dma_addr_t dma;			/* lo que ve el dispositivo */
	phys_addr_t orig;		/* memoria del driver */
	size_t size;
	u64 bphys, count;		/* paginas del bounce */
	enum dma_data_direction dir;
};
static LIST_HEAD(lkpi_bounces);
static DEFINE_SPINLOCK(lkpi_bounce_lock);

static void lkpi_bounce_copy(phys_addr_t dst, phys_addr_t src, size_t size)
{
	memcpy((void *)(AURORA_PHYS_MAP_BASE + dst), (void *)(AURORA_PHYS_MAP_BASE + src), size);
}

static dma_addr_t lkpi_bounce_map(struct device *dev, phys_addr_t phys, size_t size,
				  enum dma_data_direction dir, unsigned long attrs, u64 mask)
{
	unsigned long flags, off = phys & ~PAGE_MASK;
	struct lkpi_bounce *b;
	u64 count = 1ULL << get_order(off + size);

	b = kmalloc(sizeof(*b), GFP_ATOMIC);
	if (!b)
		return DMA_MAPPING_ERROR;
	b->bphys = lkpi_dma_pages_below(count, mask);
	if (!b->bphys) {
		kfree(b);
		dev_warn_once(dev, "DMA: sin memoria para bounce bajo %llx\n",
			      (unsigned long long)mask);
		return DMA_MAPPING_ERROR;
	}
	b->count = count;
	b->orig = phys;
	b->size = size;
	b->dir = dir;
	b->dma = b->bphys + off;
	if ((dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL) && !(attrs & DMA_ATTR_SKIP_CPU_SYNC))
		lkpi_bounce_copy(b->dma, phys, size);
	spin_lock_irqsave(&lkpi_bounce_lock, flags);
	list_add(&b->list, &lkpi_bounces);
	spin_unlock_irqrestore(&lkpi_bounce_lock, flags);
	atomic64_inc(&lkpi_dma_stats.bounces);
	atomic64_add(size, &lkpi_dma_stats.bounce_bytes);
	atomic64_inc(&lkpi_dma_stats.bounce_live);
	return b->dma;
}

/* Bounce que contiene `addr` (con el cerrojo tomado). */
static struct lkpi_bounce *lkpi_bounce_find(dma_addr_t addr)
{
	struct lkpi_bounce *b;

	list_for_each_entry(b, &lkpi_bounces, list)
		if (addr >= b->dma && addr < b->dma + b->size)
			return b;
	return NULL;
}

/* -------------------------------------------------------- streaming */

dma_addr_t lkpi_dma_map_phys(struct device *dev, phys_addr_t phys, size_t size,
			     enum dma_data_direction dir, unsigned long attrs)
{
	u64 mask = lkpi_dev_mask(dev);

	if (WARN_ON_ONCE(!valid_dma_direction(dir)) || !size)
		return DMA_MAPPING_ERROR;
	atomic64_inc(&lkpi_dma_stats.maps);
	if (phys + size - 1 <= mask)
		return phys;
	return lkpi_bounce_map(dev, phys, size, dir, attrs, mask);
}
EXPORT_SYMBOL(lkpi_dma_map_phys);

dma_addr_t lkpi_dma_map_page_attrs(struct device *dev, struct page *page, size_t offset,
				   size_t size, enum dma_data_direction dir, unsigned long attrs)
{
	return lkpi_dma_map_phys(dev, page_to_phys(page) + offset, size, dir, attrs);
}
EXPORT_SYMBOL(lkpi_dma_map_page_attrs);

dma_addr_t lkpi_dma_map_single_attrs(struct device *dev, void *ptr, size_t size,
				     enum dma_data_direction dir, unsigned long attrs)
{
	/* Como Linux: solo memoria de la ventana directa (kmalloc, paginas). */
	if (WARN_ON_ONCE(is_vmalloc_addr(ptr)))
		return DMA_MAPPING_ERROR;
	return lkpi_dma_map_phys(dev, __pa(ptr), size, dir, attrs);
}
EXPORT_SYMBOL(lkpi_dma_map_single_attrs);

dma_addr_t lkpi_dma_map_resource(struct device *dev, phys_addr_t phys_addr, size_t size,
				 enum dma_data_direction dir, unsigned long attrs)
{
	return phys_addr;	/* MMIO de otro dispositivo: sin bounce */
}
EXPORT_SYMBOL(lkpi_dma_map_resource);

void lkpi_dma_unmap_page_attrs(struct device *dev, dma_addr_t addr, size_t size,
			       enum dma_data_direction dir, unsigned long attrs)
{
	struct lkpi_bounce *b;
	unsigned long flags;

	spin_lock_irqsave(&lkpi_bounce_lock, flags);
	b = lkpi_bounce_find(addr);
	if (b)
		list_del(&b->list);
	spin_unlock_irqrestore(&lkpi_bounce_lock, flags);
	if (!b)
		return;
	WARN_ON(b->dma != addr || b->size != size);
	if ((b->dir == DMA_FROM_DEVICE || b->dir == DMA_BIDIRECTIONAL) &&
	    !(attrs & DMA_ATTR_SKIP_CPU_SYNC))
		lkpi_bounce_copy(b->orig, b->dma, b->size);
	aurora_pmm_free_pages(b->bphys, b->count);
	atomic64_dec(&lkpi_dma_stats.bounce_live);
	kfree(b);
}
EXPORT_SYMBOL(lkpi_dma_unmap_page_attrs);

static void lkpi_dma_sync(dma_addr_t addr, size_t size, enum dma_data_direction dir, bool for_cpu)
{
	struct lkpi_bounce *b;
	unsigned long flags;
	size_t off;

	spin_lock_irqsave(&lkpi_bounce_lock, flags);
	b = lkpi_bounce_find(addr);
	if (b) {
		off = addr - b->dma;
		size = min(size, b->size - off);
		if (for_cpu && (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL))
			lkpi_bounce_copy(b->orig + off, addr, size);
		else if (!for_cpu && (dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL))
			lkpi_bounce_copy(addr, b->orig + off, size);
	}
	spin_unlock_irqrestore(&lkpi_bounce_lock, flags);
}

void lkpi_dma_sync_single_for_cpu(struct device *dev, dma_addr_t addr, size_t size,
				  enum dma_data_direction dir)
{
	lkpi_dma_sync(addr, size, dir, true);
}
EXPORT_SYMBOL(lkpi_dma_sync_single_for_cpu);

void lkpi_dma_sync_single_for_device(struct device *dev, dma_addr_t addr, size_t size,
				     enum dma_data_direction dir)
{
	lkpi_dma_sync(addr, size, dir, false);
}
EXPORT_SYMBOL(lkpi_dma_sync_single_for_device);

/* Sin IOMMU no se juntan entradas: una por segmento de la lista. */
int lkpi_dma_map_sg_attrs(struct device *dev, struct scatterlist *sgl, int nents,
			  enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i) {
		sg->dma_address = lkpi_dma_map_phys(dev, sg_phys(sg), sg->length, dir, attrs);
		if (sg->dma_address == DMA_MAPPING_ERROR) {
			lkpi_dma_unmap_sg_attrs(dev, sgl, i, dir, attrs | DMA_ATTR_SKIP_CPU_SYNC);
			return 0;
		}
		sg_dma_len(sg) = sg->length;
	}
	return nents;
}
EXPORT_SYMBOL(lkpi_dma_map_sg_attrs);

void lkpi_dma_unmap_sg_attrs(struct device *dev, struct scatterlist *sgl, int nents,
			     enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i)
		lkpi_dma_unmap_page_attrs(dev, sg->dma_address, sg_dma_len(sg), dir, attrs);
}
EXPORT_SYMBOL(lkpi_dma_unmap_sg_attrs);

void lkpi_dma_sync_sg_for_cpu(struct device *dev, struct scatterlist *sgl, int nelems,
			      enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nelems, i)
		lkpi_dma_sync(sg->dma_address, sg_dma_len(sg), dir, true);
}
EXPORT_SYMBOL(lkpi_dma_sync_sg_for_cpu);

void lkpi_dma_sync_sg_for_device(struct device *dev, struct scatterlist *sgl, int nelems,
				 enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nelems, i)
		lkpi_dma_sync(sg->dma_address, sg_dma_len(sg), dir, false);
}
EXPORT_SYMBOL(lkpi_dma_sync_sg_for_device);

int lkpi_dma_map_sgtable(struct device *dev, struct sg_table *sgt, enum dma_data_direction dir,
			 unsigned long attrs)
{
	int nents = lkpi_dma_map_sg_attrs(dev, sgt->sgl, sgt->orig_nents, dir, attrs);

	if (nents <= 0)
		return -ENOMEM;
	sgt->nents = nents;
	return 0;
}
EXPORT_SYMBOL(lkpi_dma_map_sgtable);

void lkpi_dma_unmap_sgtable(struct device *dev, struct sg_table *sgt,
			    enum dma_data_direction dir, unsigned long attrs)
{
	lkpi_dma_unmap_sg_attrs(dev, sgt->sgl, sgt->orig_nents, dir, attrs);
}
EXPORT_SYMBOL(lkpi_dma_unmap_sgtable);

/* -------------------------------------------------------- dmam_* */

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

/* --------------------------------------------------------- mascaras */

int lkpi_dma_supported(struct device *dev, u64 mask)
{
	/* Lo minimo que Linux acepta (ISA: 24 bits); con bounce, cualquier otra. */
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
