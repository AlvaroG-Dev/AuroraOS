/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * struct page de la capa. No es el struct page de Linux: los drivers solo
 * deben usarlo a traves de las funciones de mm.h (page_address,
 * page_to_pfn, get_page...). Las paginas de lkpi_alloc_pages() de orden n
 * vienen en un array contiguo (page + i funciona dentro de la asignacion);
 * pfn_to_page()/virt_to_page() de una pagina ajena crean un descriptor
 * suelto.
 */
#ifndef _LKPI_LINUX_MM_TYPES_H
#define _LKPI_LINUX_MM_TYPES_H

#include <linux/types.h>

struct address_space;
struct kmem_cache;

struct page {
	unsigned long flags;		/* PG_* */
	unsigned long pfn;		/* marco fisico */
	void *virtual;			/* direccion en la ventana directa */
	atomic_t _refcount;
	atomic_t _mapcount;
	unsigned int order;		/* orden de la asignacion (cabeza) */
	unsigned int lkpi_flags;	/* LKPI_PAGE_* (internos) */
	struct page *head;		/* cabeza de la asignacion */
	unsigned long private;
	struct address_space *mapping;
	pgoff_t index;
	struct list_head lru;
	/* slab de la capa (paginas de kmalloc/kmem_cache) */
	struct kmem_cache *slab_cache;
	void *freelist;
	unsigned int inuse;
	struct page *hash_next;		/* registro pfn -> page */
};

struct folio {
	struct page page;
};

struct vm_area_struct;
struct mm_struct;

#endif
