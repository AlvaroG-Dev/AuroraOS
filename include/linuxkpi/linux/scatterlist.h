/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Listas scatter/gather (K1-DMA, L4). Mismo modelo que Linux: page_link
 * guarda el struct page y dos bits (encadenado / ultima entrada), y una tabla
 * puede encadenar arrays con sg_chain(). sg_alloc_table() de la capa reserva
 * un solo array (sin cadenas) de cualquier tamano.
 */
#ifndef _LKPI_LINUX_SCATTERLIST_H
#define _LKPI_LINUX_SCATTERLIST_H

#include <linux/types.h>
#include <linux/bug.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <asm/io.h>

struct scatterlist {
	unsigned long page_link;
	unsigned int offset;
	unsigned int length;
	dma_addr_t dma_address;
	unsigned int dma_length;
	unsigned int dma_flags;
};

struct sg_table {
	struct scatterlist *sgl;
	unsigned int nents;		/* entradas mapeadas para DMA */
	unsigned int orig_nents;	/* entradas de la lista */
};

struct sg_append_table {
	struct sg_table sgt;
	struct scatterlist *prv;
	unsigned int total_nents;
};

#define SG_CHAIN 0x01UL
#define SG_END 0x02UL
#define SG_PAGE_LINK_MASK (SG_CHAIN | SG_END)
#define SG_MAX_SINGLE_ALLOC (PAGE_SIZE / sizeof(struct scatterlist))

#define sg_dma_address(sg) ((sg)->dma_address)
#define sg_dma_len(sg) ((sg)->dma_length)
#define sg_is_chain(sg) ((sg)->page_link & SG_CHAIN)
#define sg_is_last(sg) ((sg)->page_link & SG_END)
#define sg_chain_ptr(sg) ((struct scatterlist *)((sg)->page_link & ~SG_PAGE_LINK_MASK))

static inline struct page *sg_page(struct scatterlist *sg)
{
	return (struct page *)(sg->page_link & ~SG_PAGE_LINK_MASK);
}
static inline void sg_assign_page(struct scatterlist *sg, struct page *page)
{
	unsigned long flags = sg->page_link & SG_PAGE_LINK_MASK;
	sg->page_link = flags | (unsigned long)page;
}
#define sg_set_folio(sg, folio, len, off) sg_set_page(sg, &(folio)->page, len, off)
static inline void sg_set_page(struct scatterlist *sg, struct page *page, unsigned int len,
			       unsigned int offset)
{
	sg_assign_page(sg, page);
	sg->offset = offset;
	sg->length = len;
}
static inline void sg_set_buf(struct scatterlist *sg, const void *buf, unsigned int buflen)
{
	sg_set_page(sg, virt_to_page(buf), buflen, offset_in_page(buf));
}
static inline dma_addr_t sg_phys(struct scatterlist *sg)
{
	return page_to_phys(sg_page(sg)) + sg->offset;
}
static inline void *sg_virt(struct scatterlist *sg)
{
	return page_address(sg_page(sg)) + sg->offset;
}
static inline void sg_mark_end(struct scatterlist *sg)
{
	sg->page_link |= SG_END;
	sg->page_link &= ~SG_CHAIN;
}
static inline void sg_unmark_end(struct scatterlist *sg) { sg->page_link &= ~SG_END; }
static inline void sg_chain(struct scatterlist *prv, unsigned int prv_nents,
			    struct scatterlist *sgl)
{
	prv[prv_nents - 1].offset = 0;
	prv[prv_nents - 1].length = 0;
	prv[prv_nents - 1].page_link = ((unsigned long)sgl | SG_CHAIN) & ~SG_END;
}
static inline void sg_init_marker(struct scatterlist *sgl, unsigned int nents)
{
	sg_mark_end(&sgl[nents - 1]);
}
static inline struct scatterlist *sg_next(struct scatterlist *sg)
{
	if (sg_is_last(sg))
		return NULL;
	sg++;
	if (unlikely(sg_is_chain(sg)))
		sg = sg_chain_ptr(sg);
	return sg;
}
#define sg_dma_is_bus_address(sg) 0
#define sg_dma_mark_bus_address(sg) do { } while (0)

#define for_each_sg(sglist, sg, nr, __i) \
	for (__i = 0, sg = (sglist); __i < (nr); __i++, sg = sg_next(sg))
#define for_each_sgtable_sg(sgt, sg, i) for_each_sg((sgt)->sgl, sg, (sgt)->orig_nents, i)
#define for_each_sgtable_dma_sg(sgt, sg, i) for_each_sg((sgt)->sgl, sg, (sgt)->nents, i)

void lkpi_sg_init_table(struct scatterlist *sgl, unsigned int nents);
void lkpi_sg_init_one(struct scatterlist *sg, const void *buf, unsigned int buflen);
int lkpi_sg_nents(struct scatterlist *sg);
int lkpi_sg_nents_for_len(struct scatterlist *sg, u64 len);
struct scatterlist *lkpi_sg_last(struct scatterlist *s, unsigned int nents);
int lkpi_sg_alloc_table(struct sg_table *table, unsigned int nents, gfp_t gfp);
void lkpi_sg_free_table(struct sg_table *table);
int lkpi_sg_alloc_table_from_pages_segment(struct sg_table *sgt, struct page **pages,
					   unsigned int n_pages, unsigned int offset,
					   unsigned long size, unsigned int max_segment,
					   gfp_t gfp);
size_t lkpi_sg_copy_buffer(struct scatterlist *sgl, unsigned int nents, void *buf,
			   size_t buflen, off_t skip, bool to_buffer);
#define sg_init_table(s, n) lkpi_sg_init_table(s, n)
#define sg_init_one(s, b, l) lkpi_sg_init_one(s, b, l)
#define sg_nents(s) lkpi_sg_nents(s)
#define sg_nents_for_len(s, l) lkpi_sg_nents_for_len(s, l)
#define sg_last(s, n) lkpi_sg_last(s, n)
#define sg_alloc_table(t, n, g) lkpi_sg_alloc_table(t, n, g)
#define sg_free_table(t) lkpi_sg_free_table(t)
#define sg_alloc_table_from_pages_segment(t, p, n, o, s, m, g) \
	lkpi_sg_alloc_table_from_pages_segment(t, p, n, o, s, m, g)
static inline int sg_alloc_table_from_pages(struct sg_table *sgt, struct page **pages,
					    unsigned int n_pages, unsigned int offset,
					    unsigned long size, gfp_t gfp)
{
	return lkpi_sg_alloc_table_from_pages_segment(sgt, pages, n_pages, offset, size,
						      UINT_MAX, gfp);
}
static inline size_t sg_copy_from_buffer(struct scatterlist *sgl, unsigned int nents,
					 const void *buf, size_t buflen)
{
	return lkpi_sg_copy_buffer(sgl, nents, (void *)buf, buflen, 0, false);
}
static inline size_t sg_copy_to_buffer(struct scatterlist *sgl, unsigned int nents, void *buf,
				       size_t buflen)
{
	return lkpi_sg_copy_buffer(sgl, nents, buf, buflen, 0, true);
}
static inline size_t sg_pcopy_from_buffer(struct scatterlist *sgl, unsigned int nents,
					  const void *buf, size_t buflen, off_t skip)
{
	return lkpi_sg_copy_buffer(sgl, nents, (void *)buf, buflen, skip, false);
}
static inline size_t sg_pcopy_to_buffer(struct scatterlist *sgl, unsigned int nents, void *buf,
					size_t buflen, off_t skip)
{
	return lkpi_sg_copy_buffer(sgl, nents, buf, buflen, skip, true);
}

/* Recorrido por paginas (de CPU o de DMA). */
struct sg_page_iter {
	struct scatterlist *sg;
	unsigned int sg_pgoffset;
	unsigned int __nents;
	int __pg_advance;
};
struct sg_dma_page_iter {
	struct sg_page_iter base;
};
bool lkpi___sg_page_iter_next(struct sg_page_iter *piter);
bool lkpi___sg_page_iter_dma_next(struct sg_dma_page_iter *dma_iter);
static inline void __sg_page_iter_start(struct sg_page_iter *piter, struct scatterlist *sglist,
					unsigned int nents, unsigned long pgoffset)
{
	piter->__pg_advance = 0;
	piter->__nents = nents;
	piter->sg = sglist;
	piter->sg_pgoffset = pgoffset;
}
#define __sg_page_iter_next(p) lkpi___sg_page_iter_next(p)
#define __sg_page_iter_dma_next(p) lkpi___sg_page_iter_dma_next(p)
static inline struct page *sg_page_iter_page(struct sg_page_iter *piter)
{
	return nth_page(sg_page(piter->sg), piter->sg_pgoffset);
}
static inline dma_addr_t sg_page_iter_dma_address(struct sg_dma_page_iter *dma_iter)
{
	return sg_dma_address(dma_iter->base.sg) + (dma_iter->base.sg_pgoffset << PAGE_SHIFT);
}
#define for_each_sg_page(sglist, piter, nents, pgoffset) \
	for (__sg_page_iter_start((piter), (sglist), (nents), (pgoffset)); \
	     __sg_page_iter_next(piter);)
#define for_each_sg_dma_page(sglist, dma_iter, dma_nents, pgoffset) \
	for (__sg_page_iter_start(&(dma_iter)->base, sglist, dma_nents, pgoffset); \
	     __sg_page_iter_dma_next(dma_iter);)
#define for_each_sgtable_page(sgt, piter, pgoffset) \
	for_each_sg_page((sgt)->sgl, piter, (sgt)->orig_nents, pgoffset)
#define for_each_sgtable_dma_page(sgt, dma_iter, pgoffset) \
	for_each_sg_dma_page((sgt)->sgl, dma_iter, (sgt)->nents, pgoffset)

#endif
