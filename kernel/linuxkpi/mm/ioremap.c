// SPDX-License-Identifier: GPL-2.0-only
/*
 * ioremap: mapea MMIO en la ventana MMIO de Aurora (MMIO_MAP_BASE + fisica)
 * con el tipo de cache pedido. La direccion virtual depende solo de la
 * fisica, asi que el mismo rango puede estar ya mapeado por un driver de
 * Aurora (LAPIC, framebuffer...) o por otro ioremap de la capa:
 *  - una pagina mapeada por Aurora se usa tal cual y nunca se desmapea;
 *  - una pagina de la capa se desmapea cuando ningun ioremap vivo la cubre.
 * memremap(WB) de RAM devuelve la ventana directa. El lock no desactiva
 * interrupciones: mapear/desmapear puede invalidar TLB en otras CPU.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/bitmap.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "../aurora.h"
#include "../lkpi_internal.h"

struct io_map {
	struct list_head list;
	void __iomem *va;		/* lo devuelto al driver */
	phys_addr_t phys_start;		/* alineado a pagina */
	size_t len;			/* multiplo de pagina */
	int type;
	unsigned long *ours;		/* bit i: la pagina i la mapeo la capa */
};

static LIST_HEAD(io_maps);
static DEFINE_SPINLOCK(io_lock);

static u64 type_flags(int type)
{
	u64 f = AURORA_PTE_WRITABLE | AURORA_PTE_GLOBAL | AURORA_PTE_NX;
	switch (type) {
	case LKPI_IOREMAP_WC:
		return f | AURORA_PTE_WRITETHRU | AURORA_PTE_NOCACHE;	/* PAT: WC */
	case LKPI_IOREMAP_WT:
		return f | AURORA_PTE_WRITETHRU;
	case LKPI_IOREMAP_WB:
		return f;
	default:
		return f | AURORA_PTE_NOCACHE;
	}
}

static bool phys_is_ram(phys_addr_t phys, size_t size)
{
	return ((phys + size - 1) >> PAGE_SHIFT) < aurora_pmm_total_pages();
}

/* Algun ioremap vivo de la capa cubre la pagina y la tiene como suya. */
static bool page_ours_locked(phys_addr_t page, const struct io_map *except)
{
	struct io_map *it;
	list_for_each_entry(it, &io_maps, list)
		if (it != except && page >= it->phys_start && page < it->phys_start + it->len &&
		    test_bit((page - it->phys_start) >> PAGE_SHIFT, it->ours))
			return true;
	return false;
}

/*
 * Display-shadow (L5): el framebuffer del firmware (GOP) y los BAR del
 * dispositivo que lo aloja son del compositor. Un driver de Linux que los
 * mapea recibe una copia en RAM (los BAR pequenos con el contenido actual de
 * los registros), asi sus modos, su VRAM y sus registros no tocan la pantalla.
 */
struct io_shadow {
	struct list_head list;
	phys_addr_t start;
	size_t len;
	void *mem;
};
static LIST_HEAD(io_shadows);

static void __iomem *ioremap_real(phys_addr_t phys, size_t size, int type);

int lkpi_display_shadow_add(phys_addr_t start, size_t len, bool copy)
{
	struct io_shadow *s = kzalloc(sizeof(*s), GFP_KERNEL);

	if (!s)
		return -ENOMEM;
	s->mem = kvzalloc(len, GFP_KERNEL);
	if (!s->mem) {
		kfree(s);
		return -ENOMEM;
	}
	s->start = start;
	s->len = len;
	if (copy) {
		void __iomem *real = ioremap_real(start, len, LKPI_IOREMAP_UC);

		if (real) {
			/* los registros de los dispositivos de pantalla son de 16 bits */
			for (size_t i = 0; i + 1 < len; i += 2)
				*(u16 *)((char *)s->mem + i) = readw(real + i);
			lkpi_iounmap(real);
		}
	}
	spin_lock(&io_lock);
	list_add(&s->list, &io_shadows);
	spin_unlock(&io_lock);
	return 0;
}

static void *shadow_find(phys_addr_t phys, size_t size)
{
	struct io_shadow *s;
	void *mem = NULL;

	spin_lock(&io_lock);
	list_for_each_entry(s, &io_shadows, list)
		if (phys >= s->start && phys + size <= s->start + s->len) {
			mem = (char *)s->mem + (phys - s->start);
			break;
		}
	spin_unlock(&io_lock);
	return mem;
}

static bool shadow_owns(const void *addr)
{
	struct io_shadow *s;
	bool own = false;

	spin_lock(&io_lock);
	list_for_each_entry(s, &io_shadows, list)
		if ((const char *)addr >= (char *)s->mem && (const char *)addr < (char *)s->mem + s->len) {
			own = true;
			break;
		}
	spin_unlock(&io_lock);
	return own;
}

void lkpi_display_shadow_exit(void)
{
	struct io_shadow *s, *tmp;

	list_for_each_entry_safe(s, tmp, &io_shadows, list) {
		list_del(&s->list);
		kvfree(s->mem);
		kfree(s);
	}
}

void __iomem *lkpi_ioremap(phys_addr_t phys, size_t size, int type)
{
	void *shadow;

	lkpi_display_shadow_probe();
	shadow = shadow_find(phys, size);
	if (shadow)
		return (void __iomem *)shadow;
	return ioremap_real(phys, size, type);
}
EXPORT_SYMBOL(lkpi_ioremap);

static void __iomem *ioremap_real(phys_addr_t phys, size_t size, int type)
{
	phys_addr_t start = phys & PAGE_MASK, pg;
	size_t len = PAGE_ALIGN(phys + size) - start;
	unsigned long npages = len >> PAGE_SHIFT, i;
	struct io_map *m;
	bool failed = false;

	if (!size)
		return NULL;
	if (type == LKPI_IOREMAP_WB && phys_is_ram(phys, size))
		return (void __iomem *)__va(phys);
	m = aurora_kmalloc(sizeof(*m));
	if (!m)
		return NULL;
	m->ours = aurora_kmalloc(BITS_TO_LONGS(npages) * sizeof(long));
	if (!m->ours) {
		aurora_kfree(m);
		return NULL;
	}
	bitmap_zero(m->ours, npages);
	m->phys_start = start;
	m->len = len;
	m->type = type;
	m->va = (void __iomem *)(AURORA_MMIO_MAP_BASE + phys);

	spin_lock(&io_lock);
	for (i = 0, pg = start; i < npages; i++, pg += PAGE_SIZE) {
		bool mapped = aurora_paging_get_phys(AURORA_MMIO_MAP_BASE + pg) != 0;
		if (mapped && !page_ours_locked(pg, NULL))
			continue;	/* de Aurora: se usa sin tocarla */
		if (!mapped && !aurora_mmio_map(pg, PAGE_SIZE, type_flags(type))) {
			failed = true;
			break;
		}
		__set_bit(i, m->ours);
	}
	if (failed) {
		for (unsigned long k = 0; k < i; k++)
			if (test_bit(k, m->ours) &&
			    !page_ours_locked(start + (k << PAGE_SHIFT), NULL))
				aurora_mmio_unmap(start + (k << PAGE_SHIFT), PAGE_SIZE);
		spin_unlock(&io_lock);
		aurora_kfree(m->ours);
		aurora_kfree(m);
		return NULL;
	}
	list_add(&m->list, &io_maps);
	spin_unlock(&io_lock);
	return m->va;
}

static void io_unmap_locked(struct io_map *m)
{
	unsigned long i, npages = m->len >> PAGE_SHIFT;

	list_del(&m->list);
	for (i = 0; i < npages; i++) {
		phys_addr_t pg = m->phys_start + (i << PAGE_SHIFT);
		if (test_bit(i, m->ours) && !page_ours_locked(pg, NULL))
			aurora_mmio_unmap(pg, PAGE_SIZE);
	}
}

void lkpi_iounmap(volatile void __iomem *addr)
{
	struct io_map *m, *found = NULL;

	if (!addr || lkpi_is_direct_map((const void *)addr) || shadow_owns((const void *)addr))
		return;
	spin_lock(&io_lock);
	list_for_each_entry(m, &io_maps, list)
		if (m->va == addr) {
			found = m;
			break;
		}
	if (found)
		io_unmap_locked(found);
	spin_unlock(&io_lock);
	if (WARN(!found, "iounmap de una direccion no mapeada: %px\n", (void *)addr))
		return;
	aurora_kfree(found->ours);
	aurora_kfree(found);
}
EXPORT_SYMBOL(lkpi_iounmap);

void *lkpi_memremap(phys_addr_t offset, size_t size, unsigned long flags)
{
	if (flags & MEMREMAP_WB)
		return (void __force *)lkpi_ioremap(offset, size, LKPI_IOREMAP_WB);
	if (flags & MEMREMAP_WT)
		return (void __force *)lkpi_ioremap(offset, size, LKPI_IOREMAP_WT);
	if (flags & MEMREMAP_WC)
		return (void __force *)lkpi_ioremap(offset, size, LKPI_IOREMAP_WC);
	return NULL;
}
EXPORT_SYMBOL(lkpi_memremap);

void lkpi_memunmap(void *addr)
{
	lkpi_iounmap((void __iomem *)addr);
}
EXPORT_SYMBOL(lkpi_memunmap);

void lkpi_ioremap_exit(void)
{
	lkpi_display_shadow_exit();
	struct io_map *m, *tmp;
	unsigned long n = 0;

	spin_lock(&io_lock);
	list_for_each_entry_safe(m, tmp, &io_maps, list) {
		io_unmap_locked(m);
		aurora_kfree(m->ours);
		aurora_kfree(m);
		n++;
	}
	spin_unlock(&io_lock);
	if (n)
		pr_warn("linuxkpi: %lu mapeos ioremap sin deshacer al descargar\n", n);
}
