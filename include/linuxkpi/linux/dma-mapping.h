/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DMA (K1-DMA, L4). Sin IOMMU (L9): la direccion de bus es la fisica y x86
 * es coherente, asi que las sincronizaciones solo hacen algo con bounce.
 *
 * - dma_alloc_coherent()/dma_alloc_attrs(): paginas por debajo de
 *   coherent_dma_mask, alineadas al orden del tamano. DMA_ATTR_WRITE_COMBINE
 *   (dma_alloc_wc) da un mapeo WC en la region vmap de Aurora;
 *   DMA_ATTR_NO_KERNEL_MAPPING, una cookie que no se debe tocar.
 * - dma_map_*: directo si [fis, fis+len) cabe en la mascara; si no, un bounce
 *   reservado al momento por debajo de la mascara, copiado en map/unmap/sync
 *   segun la direccion. DMA_MAPPING_ERROR si no hay memoria.
 * - dma_mmap_*()/dma_get_sgtable(): de memoria coherente.
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
static inline size_t dma_opt_mapping_size(struct device *dev) { return SIZE_MAX; }
static inline bool dma_need_sync(struct device *dev, dma_addr_t dma) { return true; }
static inline unsigned long dma_get_merge_boundary(struct device *dev) { return 0; }
static inline unsigned long dma_get_seg_boundary(struct device *dev)
{
	return dev->dma_parms && dev->dma_parms->segment_boundary_mask ?
		dev->dma_parms->segment_boundary_mask : ~0UL;
}
static inline int dma_set_seg_boundary(struct device *dev, unsigned long mask)
{
	if (!dev->dma_parms)
		return -EIO;
	dev->dma_parms->segment_boundary_mask = mask;
	return 0;
}
static inline bool dma_is_coherent(struct device *dev) { return true; }
#define dev_is_dma_coherent(d) dma_is_coherent(d)

struct page;
struct scatterlist;
struct sg_table;
struct vm_area_struct;

/* Mapeos de streaming. */
dma_addr_t lkpi_dma_map_page_attrs(struct device *dev, struct page *page, size_t offset,
				   size_t size, enum dma_data_direction dir, unsigned long attrs);
void lkpi_dma_unmap_page_attrs(struct device *dev, dma_addr_t addr, size_t size,
			       enum dma_data_direction dir, unsigned long attrs);
dma_addr_t lkpi_dma_map_phys(struct device *dev, phys_addr_t phys, size_t size,
			     enum dma_data_direction dir, unsigned long attrs);
dma_addr_t lkpi_dma_map_resource(struct device *dev, phys_addr_t phys_addr, size_t size,
				 enum dma_data_direction dir, unsigned long attrs);
void lkpi_dma_sync_single_for_cpu(struct device *dev, dma_addr_t addr, size_t size,
				  enum dma_data_direction dir);
void lkpi_dma_sync_single_for_device(struct device *dev, dma_addr_t addr, size_t size,
				     enum dma_data_direction dir);
int lkpi_dma_map_sg_attrs(struct device *dev, struct scatterlist *sg, int nents,
			  enum dma_data_direction dir, unsigned long attrs);
void lkpi_dma_unmap_sg_attrs(struct device *dev, struct scatterlist *sg, int nents,
			     enum dma_data_direction dir, unsigned long attrs);
void lkpi_dma_sync_sg_for_cpu(struct device *dev, struct scatterlist *sg, int nelems,
			      enum dma_data_direction dir);
void lkpi_dma_sync_sg_for_device(struct device *dev, struct scatterlist *sg, int nelems,
				 enum dma_data_direction dir);
int lkpi_dma_map_sgtable(struct device *dev, struct sg_table *sgt, enum dma_data_direction dir,
			 unsigned long attrs);
void lkpi_dma_unmap_sgtable(struct device *dev, struct sg_table *sgt,
			    enum dma_data_direction dir, unsigned long attrs);
#define dma_map_page_attrs(d, p, o, s, r, a) lkpi_dma_map_page_attrs(d, p, o, s, r, a)
#define dma_unmap_page_attrs(d, h, s, r, a) lkpi_dma_unmap_page_attrs(d, h, s, r, a)
#define dma_map_page(d, p, o, s, r) lkpi_dma_map_page_attrs(d, p, o, s, r, 0)
#define dma_unmap_page(d, h, s, r) lkpi_dma_unmap_page_attrs(d, h, s, r, 0)
#define dma_map_resource(d, p, s, r, a) lkpi_dma_map_resource(d, p, s, r, a)
#define dma_unmap_resource(d, h, s, r, a) do { (void)(d); (void)(h); } while (0)
#define dma_sync_single_for_cpu(d, h, s, r) lkpi_dma_sync_single_for_cpu(d, h, s, r)
#define dma_sync_single_for_device(d, h, s, r) lkpi_dma_sync_single_for_device(d, h, s, r)
#define dma_sync_single_range_for_cpu(d, h, o, s, r) lkpi_dma_sync_single_for_cpu(d, (h) + (o), s, r)
#define dma_sync_single_range_for_device(d, h, o, s, r) \
	lkpi_dma_sync_single_for_device(d, (h) + (o), s, r)
#define dma_map_sg_attrs(d, s, n, r, a) lkpi_dma_map_sg_attrs(d, s, n, r, a)
#define dma_unmap_sg_attrs(d, s, n, r, a) lkpi_dma_unmap_sg_attrs(d, s, n, r, a)
#define dma_map_sg(d, s, n, r) lkpi_dma_map_sg_attrs(d, s, n, r, 0)
#define dma_unmap_sg(d, s, n, r) lkpi_dma_unmap_sg_attrs(d, s, n, r, 0)
#define dma_sync_sg_for_cpu(d, s, n, r) lkpi_dma_sync_sg_for_cpu(d, s, n, r)
#define dma_sync_sg_for_device(d, s, n, r) lkpi_dma_sync_sg_for_device(d, s, n, r)
#define dma_map_sgtable(d, t, r, a) lkpi_dma_map_sgtable(d, t, r, a)
#define dma_unmap_sgtable(d, t, r, a) lkpi_dma_unmap_sgtable(d, t, r, a)
static inline void dma_sync_sgtable_for_cpu(struct device *dev, struct sg_table *sgt,
					    enum dma_data_direction dir);
static inline void dma_sync_sgtable_for_device(struct device *dev, struct sg_table *sgt,
					       enum dma_data_direction dir);
dma_addr_t lkpi_dma_map_single_attrs(struct device *dev, void *ptr, size_t size,
				     enum dma_data_direction dir, unsigned long attrs);
#define dma_map_single_attrs(d, p, s, r, a) lkpi_dma_map_single_attrs(d, p, s, r, a)
#define dma_unmap_single_attrs(d, h, s, r, a) lkpi_dma_unmap_page_attrs(d, h, s, r, a)
#define dma_map_single(d, p, s, r) lkpi_dma_map_single_attrs(d, p, s, r, 0)
#define dma_unmap_single(d, h, s, r) lkpi_dma_unmap_page_attrs(d, h, s, r, 0)

/* Memoria coherente: mapeo a usuario, sg_table y variantes. */
int lkpi_dma_mmap_attrs(struct device *dev, struct vm_area_struct *vma, void *cpu_addr,
			dma_addr_t dma_addr, size_t size, unsigned long attrs);
int lkpi_dma_get_sgtable_attrs(struct device *dev, struct sg_table *sgt, void *cpu_addr,
			       dma_addr_t dma_addr, size_t size, unsigned long attrs);
struct page *lkpi_dma_alloc_pages(struct device *dev, size_t size, dma_addr_t *dma_handle,
				  enum dma_data_direction dir, gfp_t gfp);
void lkpi_dma_free_pages(struct device *dev, size_t size, struct page *page,
			 dma_addr_t dma_handle, enum dma_data_direction dir);
#define dma_mmap_attrs(d, v, c, h, s, a) lkpi_dma_mmap_attrs(d, v, c, h, s, a)
#define dma_mmap_coherent(d, v, c, h, s) lkpi_dma_mmap_attrs(d, v, c, h, s, 0)
#define dma_mmap_wc(d, v, c, h, s) lkpi_dma_mmap_attrs(d, v, c, h, s, DMA_ATTR_WRITE_COMBINE)
#define dma_get_sgtable_attrs(d, t, c, h, s, a) lkpi_dma_get_sgtable_attrs(d, t, c, h, s, a)
#define dma_get_sgtable(d, t, c, h, s) lkpi_dma_get_sgtable_attrs(d, t, c, h, s, 0)
#define dma_alloc_pages(d, s, h, r, g) lkpi_dma_alloc_pages(d, s, h, r, g)
#define dma_free_pages(d, s, p, h, r) lkpi_dma_free_pages(d, s, p, h, r)
static inline void *dma_alloc_wc(struct device *dev, size_t size, dma_addr_t *dma_addr,
				 gfp_t gfp)
{
	return dma_alloc_attrs(dev, size, dma_addr, gfp, DMA_ATTR_WRITE_COMBINE);
}
static inline void dma_free_wc(struct device *dev, size_t size, void *cpu_addr,
			       dma_addr_t dma_addr)
{
	dma_free_attrs(dev, size, cpu_addr, dma_addr, DMA_ATTR_WRITE_COMBINE);
}
#define dma_alloc_noncoherent(d, s, h, r, g) dma_alloc_attrs(d, s, h, g, 0)
#define dma_free_noncoherent(d, s, c, h, r) dma_free_attrs(d, s, c, h, 0)
static inline bool dma_can_mmap(struct device *dev) { return true; }

/* Contadores de bounce (pruebas y debugfs). */
struct lkpi_dma_stats {
	atomic64_t maps, bounces, bounce_bytes, bounce_live;
};
extern struct lkpi_dma_stats lkpi_dma_stats;

#include <linux/scatterlist.h>
static inline void dma_sync_sgtable_for_cpu(struct device *dev, struct sg_table *sgt,
					    enum dma_data_direction dir)
{
	lkpi_dma_sync_sg_for_cpu(dev, sgt->sgl, sgt->orig_nents, dir);
}
static inline void dma_sync_sgtable_for_device(struct device *dev, struct sg_table *sgt,
					       enum dma_data_direction dir)
{
	lkpi_dma_sync_sg_for_device(dev, sgt->sgl, sgt->orig_nents, dir);
}

#endif
