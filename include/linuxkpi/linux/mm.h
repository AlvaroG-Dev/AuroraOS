/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Paginas y direcciones (K1 sin DMA). La ventana directa de Aurora mapea
 * toda la RAM en PHYS_MAP_BASE; __va/__pa traducen sobre ella.
 * vm_area_struct, mmap y remap_pfn_range llegan con K8 (L4).
 */
#ifndef _LKPI_LINUX_MM_H
#define _LKPI_LINUX_MM_H

#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/gfp.h>
#include <linux/mm_types.h>
#include <linux/list.h>
#include <linux/sizes.h>
#include <asm/page.h>

#define PG_locked 0
#define PG_reserved 1
#define PG_dirty 2
#define PG_head 3
#define PG_private 4

struct page *lkpi_pfn_to_page(unsigned long pfn);
struct page *lkpi_virt_to_page(const void *addr);
void lkpi_page_release(struct page *page);
phys_addr_t lkpi_virt_to_phys(const void *addr);

static inline unsigned long page_to_pfn(const struct page *page) { return page->pfn; }
static inline struct page *pfn_to_page(unsigned long pfn) { return lkpi_pfn_to_page(pfn); }
static inline void *page_address(const struct page *page) { return page->virtual; }
static inline phys_addr_t page_to_phys(const struct page *page)
{
	return (phys_addr_t)page->pfn << PAGE_SHIFT;
}
#define phys_to_page(phys) pfn_to_page((phys) >> PAGE_SHIFT)
static inline struct page *virt_to_page(const void *addr) { return lkpi_virt_to_page(addr); }
#define virt_to_pfn(kaddr) (__pa(kaddr) >> PAGE_SHIFT)
#define pfn_to_virt(pfn) __va((phys_addr_t)(pfn) << PAGE_SHIFT)
#define virt_addr_valid(kaddr) lkpi_virt_addr_valid((const void *)(kaddr))
bool lkpi_virt_addr_valid(const void *addr);
#define nth_page(page, n) ((page) + (n))
#define page_to_virt(page) page_address(page)
#define lowmem_page_address(page) page_address(page)
#define offset_in_page(p) ((unsigned long)(p) & ~PAGE_MASK)

static inline int page_count(const struct page *page) { return atomic_read(&page->_refcount); }
static inline int page_ref_count(const struct page *page) { return atomic_read(&page->_refcount); }
static inline void set_page_count(struct page *page, int v) { atomic_set(&page->_refcount, v); }
static inline void init_page_count(struct page *page) { atomic_set(&page->_refcount, 1); }
static inline void get_page(struct page *page) { atomic_inc(&page->_refcount); }
static inline bool get_page_unless_zero(struct page *page)
{
	return atomic_inc_not_zero(&page->_refcount);
}
static inline void put_page(struct page *page)
{
	if (atomic_dec_and_test(&page->_refcount))
		lkpi_page_release(page);
}
static inline void page_ref_inc(struct page *page) { atomic_inc(&page->_refcount); }
static inline void page_ref_dec(struct page *page) { atomic_dec(&page->_refcount); }
static inline int page_mapcount(struct page *page) { return atomic_read(&page->_mapcount) + 1; }
static inline struct page *compound_head(const struct page *page)
{
	return page->head ? page->head : (struct page *)page;
}
static inline unsigned int compound_order(const struct page *page)
{
	return compound_head(page)->order;
}
static inline bool PageReserved(const struct page *page) { return test_bit(PG_reserved, &page->flags); }
static inline void SetPageReserved(struct page *page) { set_bit(PG_reserved, &page->flags); }
static inline void ClearPageReserved(struct page *page) { clear_bit(PG_reserved, &page->flags); }
static inline bool PageDirty(const struct page *page) { return test_bit(PG_dirty, &page->flags); }
static inline void SetPageDirty(struct page *page) { set_bit(PG_dirty, &page->flags); }
static inline void ClearPageDirty(struct page *page) { clear_bit(PG_dirty, &page->flags); }
static inline bool set_page_dirty(struct page *page) { return !test_and_set_bit(PG_dirty, &page->flags); }
#define set_page_dirty_lock set_page_dirty
static inline bool PageHighMem(const struct page *page) { return false; }
static inline bool PageCompound(const struct page *page) { return page->order > 0 || page->head; }
static inline bool PageHead(const struct page *page) { return page->order > 0 && !page->head; }
static inline bool PageTail(const struct page *page) { return page->head != NULL; }
static inline void set_page_private(struct page *page, unsigned long data) { page->private = data; }
static inline unsigned long page_private(const struct page *page) { return page->private; }
static inline struct folio *page_folio(struct page *page) { return (struct folio *)compound_head(page); }
static inline struct page *folio_page(struct folio *folio, unsigned long n) { return &folio->page + n; }
static inline void *folio_address(const struct folio *folio) { return page_address(&folio->page); }
static inline void folio_get(struct folio *folio) { get_page(&folio->page); }
static inline void folio_put(struct folio *folio) { put_page(&folio->page); }
static inline unsigned long folio_nr_pages(const struct folio *folio) { return 1UL << folio->page.order; }
static inline size_t folio_size(const struct folio *folio) { return PAGE_SIZE << folio->page.order; }
static inline unsigned long folio_pfn(const struct folio *folio) { return folio->page.pfn; }
#define page_size(page) (PAGE_SIZE << compound_order(page))
#define totalram_pages() lkpi_totalram_pages()
unsigned long lkpi_totalram_pages(void);
#define si_meminfo(i) do { } while (0)
#define is_vmalloc_addr(x) lkpi_is_vmalloc_addr((const void *)(x))
bool lkpi_is_vmalloc_addr(const void *x);

static inline int get_order(unsigned long size)
{
	if (size <= PAGE_SIZE)
		return 0;
	return fls_long((size - 1) >> PAGE_SHIFT);
}

#define PAGE_ALIGN(addr) ALIGN(addr, PAGE_SIZE)
#define PAGE_ALIGN_DOWN(addr) ALIGN_DOWN(addr, PAGE_SIZE)
#define PAGE_ALIGNED(addr) IS_ALIGNED((unsigned long)(addr), PAGE_SIZE)

#endif
