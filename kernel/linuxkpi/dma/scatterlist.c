// SPDX-License-Identifier: GPL-2.0-only
/* Listas scatter/gather (K1-DMA, L4). Ver linux/scatterlist.h. */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/highmem.h>

void lkpi_sg_init_table(struct scatterlist *sgl, unsigned int nents)
{
	memset(sgl, 0, sizeof(*sgl) * nents);
	sg_init_marker(sgl, nents);
}
EXPORT_SYMBOL(lkpi_sg_init_table);

void lkpi_sg_init_one(struct scatterlist *sg, const void *buf, unsigned int buflen)
{
	lkpi_sg_init_table(sg, 1);
	sg_set_buf(sg, buf, buflen);
}
EXPORT_SYMBOL(lkpi_sg_init_one);

int lkpi_sg_nents(struct scatterlist *sg)
{
	int nents;

	for (nents = 0; sg; sg = sg_next(sg))
		nents++;
	return nents;
}
EXPORT_SYMBOL(lkpi_sg_nents);

int lkpi_sg_nents_for_len(struct scatterlist *sg, u64 len)
{
	int nents;
	u64 total;

	if (!len)
		return 0;
	for (nents = 0, total = 0; sg; sg = sg_next(sg)) {
		nents++;
		total += sg->length;
		if (total >= len)
			return nents;
	}
	return -EINVAL;
}
EXPORT_SYMBOL(lkpi_sg_nents_for_len);

struct scatterlist *lkpi_sg_last(struct scatterlist *s, unsigned int nents)
{
	struct scatterlist *sg, *ret = NULL;
	unsigned int i;

	for_each_sg(s, sg, nents, i)
		ret = sg;
	return ret;
}
EXPORT_SYMBOL(lkpi_sg_last);

int lkpi_sg_alloc_table(struct sg_table *table, unsigned int nents, gfp_t gfp)
{
	memset(table, 0, sizeof(*table));
	if (!nents)
		return -EINVAL;
	table->sgl = kvcalloc(nents, sizeof(struct scatterlist), gfp);
	if (!table->sgl)
		return -ENOMEM;
	sg_init_marker(table->sgl, nents);
	table->nents = table->orig_nents = nents;
	return 0;
}
EXPORT_SYMBOL(lkpi_sg_alloc_table);

void lkpi_sg_free_table(struct sg_table *table)
{
	kvfree(table->sgl);
	table->sgl = NULL;
	table->nents = table->orig_nents = 0;
}
EXPORT_SYMBOL(lkpi_sg_free_table);

/* Junta paginas fisicamente seguidas en segmentos de hasta max_segment. */
int lkpi_sg_alloc_table_from_pages_segment(struct sg_table *sgt, struct page **pages,
					   unsigned int n_pages, unsigned int offset,
					   unsigned long size, unsigned int max_segment,
					   gfp_t gfp)
{
	unsigned int chunks = 0, i, cur;
	struct scatterlist *sg;
	int rc;

	if (!n_pages || !size)
		return -EINVAL;
	max_segment = ALIGN_DOWN(max_segment, PAGE_SIZE);
	if (!max_segment)
		return -EINVAL;
	for (i = 0; i < n_pages; ) {
		unsigned long seg = PAGE_SIZE;

		chunks++;
		for (cur = i + 1; cur < n_pages && seg + PAGE_SIZE <= max_segment &&
		     page_to_pfn(pages[cur]) == page_to_pfn(pages[cur - 1]) + 1; cur++)
			seg += PAGE_SIZE;
		i = cur;
	}
	rc = lkpi_sg_alloc_table(sgt, chunks, gfp);
	if (rc)
		return rc;
	sg = sgt->sgl;
	for (i = 0; i < n_pages; ) {
		unsigned long seg = PAGE_SIZE, len;

		for (cur = i + 1; cur < n_pages && seg + PAGE_SIZE <= max_segment &&
		     page_to_pfn(pages[cur]) == page_to_pfn(pages[cur - 1]) + 1; cur++)
			seg += PAGE_SIZE;
		len = min_t(unsigned long, size, seg - offset);
		sg_set_page(sg, pages[i], len, offset);
		size -= len;
		offset = 0;
		i = cur;
		sg = sg_next(sg);
	}
	return 0;
}
EXPORT_SYMBOL(lkpi_sg_alloc_table_from_pages_segment);

size_t lkpi_sg_copy_buffer(struct scatterlist *sgl, unsigned int nents, void *buf,
			   size_t buflen, off_t skip, bool to_buffer)
{
	struct scatterlist *sg;
	size_t done = 0;
	unsigned int i;

	for_each_sg(sgl, sg, nents, i) {
		size_t len = sg->length, n;
		char *p = sg_virt(sg);

		if (skip >= (off_t)len) {
			skip -= len;
			continue;
		}
		p += skip;
		len -= skip;
		skip = 0;
		n = min(len, buflen - done);
		if (to_buffer)
			memcpy(buf + done, p, n);
		else
			memcpy(p, buf + done, n);
		done += n;
		if (done == buflen)
			break;
	}
	return done;
}
EXPORT_SYMBOL(lkpi_sg_copy_buffer);

static int lkpi_sg_page_count(struct scatterlist *sg)
{
	return PAGE_ALIGN(sg->offset + sg->length) >> PAGE_SHIFT;
}

bool lkpi___sg_page_iter_next(struct sg_page_iter *piter)
{
	if (!piter->__nents || !piter->sg)
		return false;
	piter->sg_pgoffset += piter->__pg_advance;
	piter->__pg_advance = 1;
	while (piter->sg_pgoffset >= (unsigned int)lkpi_sg_page_count(piter->sg)) {
		piter->sg_pgoffset -= lkpi_sg_page_count(piter->sg);
		piter->sg = sg_next(piter->sg);
		if (!--piter->__nents || !piter->sg)
			return false;
	}
	return true;
}
EXPORT_SYMBOL(lkpi___sg_page_iter_next);

static int lkpi_sg_dma_page_count(struct scatterlist *sg)
{
	return PAGE_ALIGN(sg_dma_len(sg)) >> PAGE_SHIFT;
}

bool lkpi___sg_page_iter_dma_next(struct sg_dma_page_iter *dma_iter)
{
	struct sg_page_iter *piter = &dma_iter->base;

	if (!piter->__nents || !piter->sg)
		return false;
	piter->sg_pgoffset += piter->__pg_advance;
	piter->__pg_advance = 1;
	while (piter->sg_pgoffset >= (unsigned int)lkpi_sg_dma_page_count(piter->sg)) {
		piter->sg_pgoffset -= lkpi_sg_dma_page_count(piter->sg);
		piter->sg = sg_next(piter->sg);
		if (!--piter->__nents || !piter->sg)
			return false;
	}
	return true;
}
EXPORT_SYMBOL(lkpi___sg_page_iter_dma_next);
