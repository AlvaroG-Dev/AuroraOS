/* SPDX-License-Identifier: GPL-2.0-only */
/* Cambio de atributos de cache de la ventana fisica (RAM mapeada 1:1): UC y WB
 * de verdad (se parten las paginas de 2 MB). WC se trata como UC. Un driver
 * como el de NVIDIA lo necesita: sus paginas "uncached" no deben tener un alias
 * WB en la ventana fisica, o la CPU lee lineas viejas tras un DMA sin snoop. */
#ifndef _LKPI_ASM_SET_MEMORY_H
#define _LKPI_ASM_SET_MEMORY_H

struct page;

int lkpi_set_memory_cache(unsigned long addr, int numpages, int uncached);
void *lkpi_page_address_of(struct page *page);

static inline int set_memory_uc(unsigned long addr, int numpages) { return lkpi_set_memory_cache(addr, numpages, 1); }
static inline int set_memory_wc(unsigned long addr, int numpages) { return lkpi_set_memory_cache(addr, numpages, 1); }
static inline int set_memory_wb(unsigned long addr, int numpages) { return lkpi_set_memory_cache(addr, numpages, 0); }
static inline int set_pages_array_uc(struct page **pages, int n)
{
	for (int i = 0; i < n; i++)
		lkpi_set_memory_cache((unsigned long)lkpi_page_address_of(pages[i]), 1, 1);
	return 0;
}
static inline int set_pages_array_wc(struct page **pages, int n) { return set_pages_array_uc(pages, n); }
static inline int set_pages_array_wb(struct page **pages, int n)
{
	for (int i = 0; i < n; i++)
		lkpi_set_memory_cache((unsigned long)lkpi_page_address_of(pages[i]), 1, 0);
	return 0;
}
static inline int set_pages_uc(struct page *page, int numpages)
{
	return lkpi_set_memory_cache((unsigned long)lkpi_page_address_of(page), numpages, 1);
}
static inline int set_pages_wb(struct page *page, int numpages)
{
	return lkpi_set_memory_cache((unsigned long)lkpi_page_address_of(page), numpages, 0);
}

#endif
