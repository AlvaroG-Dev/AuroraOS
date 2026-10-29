// SPDX-License-Identifier: GPL-2.0-only
/*
 * Paginas fisicas para la capa. alloc_pages() pide al PMM de Aurora 2^order
 * marcos contiguos y crea un array de struct page (page + i es la pagina
 * i-esima). Un registro hash pfn -> struct page resuelve pfn_to_page() y
 * virt_to_page(); para marcos que no salieron de aqui se crea un descriptor
 * suelto que vive hasta que se descarga la capa.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/shrinker.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "../aurora.h"
#include "../lkpi_internal.h"

#define PAGE_HASH_BITS 12
static struct page *page_hash[1 << PAGE_HASH_BITS];
static DEFINE_SPINLOCK(page_lock);
static unsigned long pages_owned;

static unsigned int pfn_hash(unsigned long pfn)
{
	return (unsigned int)((pfn * 0x9E3779B97F4A7C15UL) >> (64 - PAGE_HASH_BITS));
}

bool lkpi_is_direct_map(const void *addr)
{
	unsigned long a = (unsigned long)addr;
	return a >= AURORA_PHYS_MAP_BASE && a - AURORA_PHYS_MAP_BASE < AURORA_PHYS_MAP_SIZE;
}

phys_addr_t lkpi_virt_to_phys(const void *addr)
{
	unsigned long a = (unsigned long)addr;
	if (lkpi_is_direct_map(addr))
		return a - AURORA_PHYS_MAP_BASE;
	return aurora_paging_get_phys(a & PAGE_MASK) + (a & ~PAGE_MASK);
}
EXPORT_SYMBOL(lkpi_virt_to_phys);

bool lkpi_virt_addr_valid(const void *addr)
{
	return lkpi_is_direct_map(addr) &&
	       ((unsigned long)addr - AURORA_PHYS_MAP_BASE) >> PAGE_SHIFT <
		       aurora_pmm_total_pages();
}
EXPORT_SYMBOL(lkpi_virt_addr_valid);

unsigned long lkpi_totalram_pages(void)
{
	return aurora_pmm_total_pages();
}
EXPORT_SYMBOL(lkpi_totalram_pages);

void lkpi_si_meminfo(struct sysinfo *si)
{
	memset(si, 0, sizeof(*si));
	si->totalram = aurora_pmm_total_pages();
	si->freeram = aurora_pmm_free_pages_count();
	si->mem_unit = PAGE_SIZE;
}
EXPORT_SYMBOL(lkpi_si_meminfo);

/* "Swap" en RAM: la mitad de lo libre (ver linux/swap.h). */
long lkpi_nr_swap_pages(void)
{
	return aurora_pmm_free_pages_count() / 2;
}
EXPORT_SYMBOL(lkpi_nr_swap_pages);

static struct page *hash_lookup_locked(unsigned long pfn)
{
	struct page *p;
	for (p = page_hash[pfn_hash(pfn)]; p; p = p->hash_next)
		if (p->pfn == pfn)
			return p;
	return NULL;
}

static void hash_add_locked(struct page *page)
{
	unsigned int h = pfn_hash(page->pfn);
	page->hash_next = page_hash[h];
	page_hash[h] = page;
}

static void hash_del_locked(struct page *page)
{
	struct page **pp = &page_hash[pfn_hash(page->pfn)];
	while (*pp && *pp != page)
		pp = &(*pp)->hash_next;
	if (*pp)
		*pp = page->hash_next;
}

struct page *lkpi_page_lookup(unsigned long pfn)
{
	unsigned long flags;
	struct page *p;
	spin_lock_irqsave(&page_lock, flags);
	p = hash_lookup_locked(pfn);
	spin_unlock_irqrestore(&page_lock, flags);
	return p;
}

static void page_desc_init(struct page *p, unsigned long pfn)
{
	memset(p, 0, sizeof(*p));
	p->pfn = pfn;
	if ((pfn << PAGE_SHIFT) < AURORA_PHYS_MAP_SIZE)
		p->virtual = __va(pfn << PAGE_SHIFT);
	atomic_set(&p->_mapcount, -1);
	INIT_LIST_HEAD(&p->lru);
}

/* Descriptor para un marco que no es de la capa (p. ej. memoria de Aurora). */
struct page *lkpi_page_alloc_desc(unsigned long pfn)
{
	struct page *p, *n;
	unsigned long flags;

	p = lkpi_page_lookup(pfn);
	if (p)
		return p;
	n = aurora_kmalloc(sizeof(*n));
	if (!n)
		return NULL;
	page_desc_init(n, pfn);
	atomic_set(&n->_refcount, 1);
	n->lkpi_flags = LKPI_PAGE_FOREIGN;
	spin_lock_irqsave(&page_lock, flags);
	p = hash_lookup_locked(pfn);
	if (!p) {
		hash_add_locked(n);
		p = n;
		n = NULL;
	}
	spin_unlock_irqrestore(&page_lock, flags);
	if (n)
		aurora_kfree(n);
	return p;
}

struct page *lkpi_pfn_to_page(unsigned long pfn)
{
	return lkpi_page_alloc_desc(pfn);
}
EXPORT_SYMBOL(lkpi_pfn_to_page);

struct page *lkpi_virt_to_page(const void *addr)
{
	return lkpi_page_alloc_desc(lkpi_virt_to_phys(addr) >> PAGE_SHIFT);
}
EXPORT_SYMBOL(lkpi_virt_to_page);

struct page *lkpi_alloc_pages(gfp_t gfp, unsigned int order)
{
	unsigned long count = 1UL << order, flags, i;
	struct page *pages;
	u64 phys;

	if (order > MAX_PAGE_ORDER) {
		WARN_ON_ONCE(!(gfp & __GFP_NOWARN));
		return NULL;
	}
	phys = aurora_pmm_alloc_pages(count);
	if (!phys && !(gfp & (__GFP_NORETRY | __GFP_NOWARN)) && lkpi_shrink_slab(gfp, count))
		phys = aurora_pmm_alloc_pages(count);
	if (!phys)
		return NULL;
	if ((gfp & (__GFP_DMA | __GFP_DMA32)) &&
	    phys + (count << PAGE_SHIFT) > ((gfp & __GFP_DMA) ? (16UL << 20) : (4UL << 30))) {
		/* Zonas DMA/DMA32: llegan con L4 (K1-dma). */
		aurora_pmm_free_pages(phys, count);
		pr_warn_once("linuxkpi: alloc_pages con GFP_DMA32 por encima de 4 GiB\n");
		return NULL;
	}
	pages = aurora_kmalloc(count * sizeof(struct page));
	if (!pages) {
		aurora_pmm_free_pages(phys, count);
		return NULL;
	}
	for (i = 0; i < count; i++) {
		page_desc_init(&pages[i], (phys >> PAGE_SHIFT) + i);
		pages[i].lkpi_flags = LKPI_PAGE_OWNED;
		pages[i].head = i ? pages : NULL;
	}
	pages->order = order;
	atomic_set(&pages->_refcount, 1);
	if (order)
		__set_bit(PG_head, &pages->flags);
	if (gfp & __GFP_ZERO)
		memset(pages->virtual, 0, count << PAGE_SHIFT);

	spin_lock_irqsave(&page_lock, flags);
	for (i = 0; i < count; i++) {
		/* Un descriptor suelto anterior para este marco ya no vale. */
		struct page *old = hash_lookup_locked(pages[i].pfn);
		if (old) {
			hash_del_locked(old);
			if (old->lkpi_flags & LKPI_PAGE_FOREIGN)
				aurora_kfree(old);
		}
		hash_add_locked(&pages[i]);
	}
	pages_owned += count;
	spin_unlock_irqrestore(&page_lock, flags);
	return pages;
}
EXPORT_SYMBOL(lkpi_alloc_pages);

/* split_page(): cada pagina del bloque pasa a asignarse y liberarse sola. */
void lkpi_split_page(struct page *page, unsigned int order)
{
	unsigned long count = 1UL << order, flags, i;

	if (!order)
		return;
	if (WARN_ON_ONCE(!(page->lkpi_flags & LKPI_PAGE_OWNED) || page->head || page->order != order))
		return;
	spin_lock_irqsave(&page_lock, flags);
	for (i = 0; i < count; i++) {
		struct page *p = &page[i];

		p->head = NULL;
		p->order = 0;
		__clear_bit(PG_head, &p->flags);
		atomic_set(&p->_refcount, 1);
		p->lkpi_flags |= LKPI_PAGE_OWNED | LKPI_PAGE_SPLIT;
		p->freelist = page;
	}
	page->inuse = count;
	spin_unlock_irqrestore(&page_lock, flags);
}
EXPORT_SYMBOL(lkpi_split_page);

static void free_split_page(struct page *p)
{
	struct page *base = p->freelist;
	unsigned long flags;
	bool last;

	spin_lock_irqsave(&page_lock, flags);
	hash_del_locked(p);
	pages_owned--;
	last = --base->inuse == 0;
	spin_unlock_irqrestore(&page_lock, flags);
	aurora_pmm_free_pages((u64)p->pfn << PAGE_SHIFT, 1);
	if (last)
		aurora_kfree(base);
}

static void free_owned_block(struct page *head)
{
	if (head->lkpi_flags & LKPI_PAGE_SPLIT) {
		free_split_page(head);
		return;
	}
	unsigned long count = 1UL << head->order, flags, i;
	u64 phys = (u64)head->pfn << PAGE_SHIFT;

	spin_lock_irqsave(&page_lock, flags);
	for (i = 0; i < count; i++)
		hash_del_locked(&head[i]);
	pages_owned -= count;
	spin_unlock_irqrestore(&page_lock, flags);
	aurora_pmm_free_pages(phys, count);
	aurora_kfree(head);
}

/* put_page() llevo el contador a cero. */
void lkpi_page_release(struct page *page)
{
	if (!(page->lkpi_flags & LKPI_PAGE_OWNED) || page->head)
		return;	/* descriptor suelto o pagina de cola: nada que liberar */
	free_owned_block(page);
}
EXPORT_SYMBOL(lkpi_page_release);

void lkpi_free_pages_page(struct page *page, unsigned int order)
{
	if (!page)
		return;
	if (WARN_ON_ONCE(!(page->lkpi_flags & LKPI_PAGE_OWNED) || page->head ||
			 page->order != order))
		return;
	if (atomic_dec_and_test(&page->_refcount))
		free_owned_block(page);
}
EXPORT_SYMBOL(lkpi_free_pages_page);

unsigned long lkpi_get_free_pages(gfp_t gfp, unsigned int order)
{
	struct page *page = lkpi_alloc_pages(gfp, order);
	return page ? (unsigned long)page_address(page) : 0;
}
EXPORT_SYMBOL(lkpi_get_free_pages);

void lkpi_free_pages_addr(unsigned long addr, unsigned int order)
{
	struct page *page;
	if (!addr)
		return;
	page = lkpi_page_lookup(__pa(addr) >> PAGE_SHIFT);
	if (WARN_ON_ONCE(!page))
		return;
	lkpi_free_pages_page(page, order);
}
EXPORT_SYMBOL(lkpi_free_pages_addr);

void *lkpi_alloc_pages_exact(size_t size, gfp_t gfp)
{
	return (void *)lkpi_get_free_pages(gfp, get_order(size));
}
EXPORT_SYMBOL(lkpi_alloc_pages_exact);

void lkpi_free_pages_exact(void *virt, size_t size)
{
	lkpi_free_pages_addr((unsigned long)virt, get_order(size));
}
EXPORT_SYMBOL(lkpi_free_pages_exact);

/* Paginas propias aun reservadas (para pruebas de fugas). */
unsigned long lkpi_pages_owned(void);
unsigned long lkpi_pages_owned(void)
{
	return READ_ONCE(pages_owned);
}
EXPORT_SYMBOL(lkpi_pages_owned);

int lkpi_mm_init(void)
{
	return 0;
}

void lkpi_mm_exit(void)
{
	unsigned long flags;
	unsigned int b;

	spin_lock_irqsave(&page_lock, flags);
	for (b = 0; b < ARRAY_SIZE(page_hash); b++) {
		struct page **pp = &page_hash[b];
		while (*pp) {
			struct page *p = *pp;
			if (p->lkpi_flags & LKPI_PAGE_FOREIGN) {
				*pp = p->hash_next;
				aurora_kfree(p);
			} else {
				pp = &p->hash_next;
			}
		}
	}
	spin_unlock_irqrestore(&page_lock, flags);
	if (pages_owned)
		pr_warn("linuxkpi: %lu paginas sin liberar al descargar\n", pages_owned);
}

/* Un nodo, una zona con toda la RAM utilizable (N2). */
static struct pglist_data lkpi_node0;

struct pglist_data *lkpi_node_data(int nid)
{
	struct zone *z = &lkpi_node0.node_zones[0];

	z->zone_start_pfn = 0;
	z->spanned_pages = z->managed_pages = z->present_pages = aurora_pmm_total_pages();
	return &lkpi_node0;
}
EXPORT_SYMBOL(lkpi_node_data);
