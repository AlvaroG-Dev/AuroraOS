/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_GFP_H
#define _LKPI_LINUX_GFP_H

#include <linux/gfp_types.h>
#include <linux/mm_types.h>

#define MAX_PAGE_ORDER 10
#define MAX_ORDER MAX_PAGE_ORDER
#define NR_PAGE_ORDERS (MAX_PAGE_ORDER + 1)
#define NUMA_NO_NODE (-1)

struct page *lkpi_alloc_pages(gfp_t gfp, unsigned int order);
void lkpi_free_pages_page(struct page *page, unsigned int order);
unsigned long lkpi_get_free_pages(gfp_t gfp, unsigned int order);
void lkpi_free_pages_addr(unsigned long addr, unsigned int order);
void *lkpi_alloc_pages_exact(size_t size, gfp_t gfp);
void lkpi_free_pages_exact(void *virt, size_t size);

static inline struct page *alloc_pages(gfp_t gfp, unsigned int order)
{
	return lkpi_alloc_pages(gfp, order);
}
static inline struct page *alloc_page(gfp_t gfp) { return lkpi_alloc_pages(gfp, 0); }
static inline struct page *alloc_pages_node(int nid, gfp_t gfp, unsigned int order)
{
	return lkpi_alloc_pages(gfp, order);
}
#define alloc_pages_exact_nid(nid, size, gfp) lkpi_alloc_pages_exact(size, gfp)
static inline void __free_pages(struct page *page, unsigned int order)
{
	lkpi_free_pages_page(page, order);
}
#define __free_page(page) __free_pages((page), 0)
static inline unsigned long __get_free_pages(gfp_t gfp, unsigned int order)
{
	return lkpi_get_free_pages(gfp, order);
}
#define __get_free_page(gfp) __get_free_pages((gfp), 0)
static inline unsigned long get_zeroed_page(gfp_t gfp)
{
	return lkpi_get_free_pages(gfp | __GFP_ZERO, 0);
}
static inline void free_pages(unsigned long addr, unsigned int order)
{
	lkpi_free_pages_addr(addr, order);
}
#define free_page(addr) free_pages((addr), 0)
static inline void *alloc_pages_exact(size_t size, gfp_t gfp)
{
	return lkpi_alloc_pages_exact(size, gfp);
}
static inline void free_pages_exact(void *virt, size_t size)
{
	lkpi_free_pages_exact(virt, size);
}
static inline bool gfpflags_allow_blocking(const gfp_t gfp)
{
	return !!(gfp & __GFP_DIRECT_RECLAIM);
}
#define numa_node_id() 0
#define numa_mem_id() 0
#define dev_to_node(dev) NUMA_NO_NODE

/* Contexto de reclamacion de lockdep: sin lockdep, no hace nada. */
#define fs_reclaim_acquire(gfp) do { (void)(gfp); } while (0)
#define fs_reclaim_release(gfp) do { (void)(gfp); } while (0)

#endif
