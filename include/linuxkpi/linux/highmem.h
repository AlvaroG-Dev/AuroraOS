/* SPDX-License-Identifier: GPL-2.0-only */
/* x86-64 no tiene highmem: kmap es page_address. */
#ifndef _LKPI_LINUX_HIGHMEM_H
#define _LKPI_LINUX_HIGHMEM_H

#include <linux/jump_label.h>
#include <linux/mm.h>
#include <linux/string.h>

static inline void *kmap(struct page *page) { return page_address(page); }
static inline void kunmap(struct page *page) { }
static inline void *kmap_atomic(struct page *page) { return page_address(page); }
#define kunmap_atomic(addr) do { (void)(addr); } while (0)
static inline void *kmap_local_page(struct page *page) { return page_address(page); }
static inline void *kmap_local_folio(struct folio *folio, size_t offset)
{
	return (char *)page_address(&folio->page) + offset;
}
#define kunmap_local(addr) do { (void)(addr); } while (0)
static inline void *kmap_local_pfn(unsigned long pfn) { return pfn_to_virt(pfn); }
static inline void clear_highpage(struct page *page) { memset(page_address(page), 0, PAGE_SIZE); }
static inline void copy_highpage(struct page *to, struct page *from)
{
	memcpy(page_address(to), page_address(from), PAGE_SIZE);
}
static inline void memcpy_from_page(char *to, struct page *page, size_t offset, size_t len)
{
	memcpy(to, (char *)page_address(page) + offset, len);
}
static inline void memcpy_to_page(struct page *page, size_t offset, const char *from, size_t len)
{
	memcpy((char *)page_address(page) + offset, from, len);
}
static inline void memzero_page(struct page *page, size_t offset, size_t len)
{
	memset((char *)page_address(page) + offset, 0, len);
}
#define flush_dcache_page(page) do { (void)(page); } while (0)

#endif
