// SPDX-License-Identifier: GPL-2.0-only
/*
 * vmalloc: memoria virtualmente contigua, alineada a pagina, sacada del heap
 * de Aurora (que reserva paginas sueltas y las mapea seguidas). Cada bloque
 * se apunta en una lista para vfree() e is_vmalloc_addr().
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "../aurora.h"
#include "../lkpi_internal.h"

struct vm_block {
	struct list_head list;
	void *addr;		/* lo que ve el driver (alineado a pagina) */
	void *raw;		/* lo que devolvio el heap de Aurora */
	unsigned long size;	/* multiplo de PAGE_SIZE */
};

static LIST_HEAD(vm_blocks);
static DEFINE_SPINLOCK(vm_lock);

static struct vm_block *vm_find_locked(const void *addr)
{
	struct vm_block *b;
	list_for_each_entry(b, &vm_blocks, list)
		if ((unsigned long)addr >= (unsigned long)b->addr &&
		    (unsigned long)addr < (unsigned long)b->addr + b->size)
			return b;
	return NULL;
}

void *lkpi_vmalloc(unsigned long size, gfp_t gfp)
{
	struct vm_block *b;
	unsigned long flags;

	if (!size)
		return NULL;
	size = PAGE_ALIGN(size);
	b = aurora_kmalloc(sizeof(*b));
	if (!b)
		return NULL;
	b->raw = aurora_kmalloc(size + PAGE_SIZE);
	if (!b->raw) {
		aurora_kfree(b);
		return NULL;
	}
	b->addr = PTR_ALIGN(b->raw, PAGE_SIZE);
	b->size = size;
	if (gfp & __GFP_ZERO)
		memset(b->addr, 0, size);
	spin_lock_irqsave(&vm_lock, flags);
	list_add(&b->list, &vm_blocks);
	spin_unlock_irqrestore(&vm_lock, flags);
	return b->addr;
}
EXPORT_SYMBOL(lkpi_vmalloc);

void lkpi_vfree(const void *addr)
{
	struct vm_block *b;
	unsigned long flags;

	if (!addr)
		return;
	spin_lock_irqsave(&vm_lock, flags);
	b = vm_find_locked(addr);
	if (b && b->addr == addr)
		list_del(&b->list);
	else
		b = NULL;
	spin_unlock_irqrestore(&vm_lock, flags);
	if (WARN(!b, "vfree de un puntero ajeno: %px\n", addr))
		return;
	aurora_kfree(b->raw);
	aurora_kfree(b);
}
EXPORT_SYMBOL(lkpi_vfree);

bool lkpi_is_vmalloc_addr(const void *x)
{
	unsigned long flags;
	bool r;

	if ((unsigned long)x >= AURORA_VMAP_BASE &&
	    (unsigned long)x < AURORA_VMAP_BASE + AURORA_VMAP_SIZE)
		return true;	/* vmap() */

	spin_lock_irqsave(&vm_lock, flags);
	r = vm_find_locked(x) != NULL;
	spin_unlock_irqrestore(&vm_lock, flags);
	return r;
}
EXPORT_SYMBOL(lkpi_is_vmalloc_addr);

size_t lkpi_vmalloc_size(const void *p);
size_t lkpi_vmalloc_size(const void *p)
{
	unsigned long flags;
	struct vm_block *b;
	size_t sz;

	spin_lock_irqsave(&vm_lock, flags);
	b = vm_find_locked(p);
	sz = b ? b->size - ((unsigned long)p - (unsigned long)b->addr) : 0;
	spin_unlock_irqrestore(&vm_lock, flags);
	return sz;
}

unsigned long lkpi_vmalloc_to_pfn(const void *addr)
{
	return lkpi_virt_to_phys(addr) >> PAGE_SHIFT;
}
EXPORT_SYMBOL(lkpi_vmalloc_to_pfn);

struct page *lkpi_vmalloc_to_page(const void *addr)
{
	return lkpi_page_alloc_desc(lkpi_vmalloc_to_pfn(addr));
}
EXPORT_SYMBOL(lkpi_vmalloc_to_page);

/* ---------------------------------------------------------------- vmap */

struct vmap_area_rec {
	struct list_head list;
	unsigned long addr;
	struct page **put_pages;	/* VM_MAP_PUT_PAGES */
	unsigned int count;
};
static LIST_HEAD(vmap_areas);

static u64 lkpi_vmap_pte_flags(unsigned long prot)
{
	u64 f = AURORA_PTE_NX;

	if (!prot || (prot & _PAGE_RW))
		f |= AURORA_PTE_WRITABLE;
	if (prot && !(prot & _PAGE_NX))
		f &= ~AURORA_PTE_NX;
	return f | (prot & (AURORA_PTE_WRITETHRU | AURORA_PTE_NOCACHE));
}

static void *lkpi_vmap_phys(unsigned long *pfns, struct page **pages, unsigned int count,
			    unsigned long prot, struct page **put_pages)
{
	struct vmap_area_rec *r;
	unsigned long flags;
	unsigned int i;
	u64 va;

	if (!count)
		return NULL;
	r = kmalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return NULL;
	va = aurora_vmap_area_alloc((u64)count << PAGE_SHIFT);
	if (!va) {
		kfree(r);
		return NULL;
	}
	for (i = 0; i < count; i++) {
		u64 phys = pages ? page_to_phys(pages[i]) : (u64)pfns[i] << PAGE_SHIFT;

		if (aurora_vmap_area_map(va + ((u64)i << PAGE_SHIFT), phys,
					 lkpi_vmap_pte_flags(prot))) {
			aurora_vmap_area_free(va);
			kfree(r);
			return NULL;
		}
	}
	r->addr = va;
	r->put_pages = put_pages;
	r->count = count;
	spin_lock_irqsave(&vm_lock, flags);
	list_add(&r->list, &vmap_areas);
	spin_unlock_irqrestore(&vm_lock, flags);
	return (void *)va;
}

void *lkpi_vmap(struct page **pages, unsigned int count, unsigned long flags, unsigned long prot)
{
	return lkpi_vmap_phys(NULL, pages, count, prot,
			      (flags & VM_MAP_PUT_PAGES) ? pages : NULL);
}
EXPORT_SYMBOL(lkpi_vmap);

void *lkpi_vmap_pfn(unsigned long *pfns, unsigned int count, unsigned long prot)
{
	return lkpi_vmap_phys(pfns, NULL, count, prot, NULL);
}
EXPORT_SYMBOL(lkpi_vmap_pfn);

void lkpi_vunmap(const void *addr)
{
	struct vmap_area_rec *r, *found = NULL;
	unsigned long flags;
	unsigned int i;

	if (!addr)
		return;
	spin_lock_irqsave(&vm_lock, flags);
	list_for_each_entry(r, &vmap_areas, list)
		if (r->addr == (unsigned long)addr) {
			list_del(&r->list);
			found = r;
			break;
		}
	spin_unlock_irqrestore(&vm_lock, flags);
	if (WARN(!found, "vunmap de un puntero ajeno: %px\n", addr))
		return;
	aurora_vmap_area_free(found->addr);
	if (found->put_pages) {
		for (i = 0; i < found->count; i++)
			put_page(found->put_pages[i]);
		kvfree(found->put_pages);
	}
	kfree(found);
}
EXPORT_SYMBOL(lkpi_vunmap);

void lkpi_vmalloc_exit(void)
{
	struct vm_block *b, *tmp;
	unsigned long n = 0;

	list_for_each_entry_safe(b, tmp, &vm_blocks, list) {
		list_del(&b->list);
		aurora_kfree(b->raw);
		aurora_kfree(b);
		n++;
	}
	if (n)
		pr_warn("linuxkpi: %lu bloques vmalloc sin liberar al descargar\n", n);
}
