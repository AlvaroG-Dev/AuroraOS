/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * vmalloc: memoria virtualmente contigua (paginas sueltas del PMM mapeadas
 * en el heap de Aurora). vmap() de paginas arbitrarias llega con L4.
 */
#ifndef _LKPI_LINUX_VMALLOC_H
#define _LKPI_LINUX_VMALLOC_H

#include <linux/gfp_types.h>
#include <linux/types.h>
#include <linux/overflow.h>

struct page;

void *lkpi_vmalloc(unsigned long size, gfp_t gfp);
void lkpi_vfree(const void *addr);
struct page *lkpi_vmalloc_to_page(const void *addr);

static inline void *vmalloc(unsigned long size) { return lkpi_vmalloc(size, GFP_KERNEL); }
static inline void *vzalloc(unsigned long size) { return lkpi_vmalloc(size, GFP_KERNEL | __GFP_ZERO); }
static inline void *__vmalloc(unsigned long size, gfp_t gfp) { return lkpi_vmalloc(size, gfp); }
static inline void *vmalloc_user(unsigned long size) { return lkpi_vmalloc(size, GFP_KERNEL | __GFP_ZERO); }
static inline void *vmalloc_32(unsigned long size) { return lkpi_vmalloc(size, GFP_KERNEL); }
static inline void *vmalloc_array(size_t n, size_t size)
{
	size_t bytes;
	if (check_mul_overflow(n, size, &bytes))
		return NULL;
	return lkpi_vmalloc(bytes, GFP_KERNEL);
}
static inline void *vcalloc(size_t n, size_t size)
{
	size_t bytes;
	if (check_mul_overflow(n, size, &bytes))
		return NULL;
	return lkpi_vmalloc(bytes, GFP_KERNEL | __GFP_ZERO);
}
#define vmalloc_node(s, n) vmalloc(s)
#define vzalloc_node(s, n) vzalloc(s)
static inline void vfree(const void *addr) { lkpi_vfree(addr); }
static inline struct page *vmalloc_to_page(const void *addr) { return lkpi_vmalloc_to_page(addr); }
unsigned long lkpi_vmalloc_to_pfn(const void *addr);
#define vmalloc_to_pfn(a) lkpi_vmalloc_to_pfn(a)

#endif
