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
