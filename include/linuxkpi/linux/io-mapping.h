/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * io_mapping: un rango de BAR mapeado una vez (WC) y accedido por offset.
 * La capa lo implementa con ioremap_wc() del rango completo, asi que
 * "map_local" es aritmetica de punteros (no hay ventanas de apertura).
 */
#ifndef _LKPI_LINUX_IO_MAPPING_H
#define _LKPI_LINUX_IO_MAPPING_H

#include <linux/io.h>
#include <linux/slab.h>
#include <linux/mm.h>

struct io_mapping {
	resource_size_t base;
	unsigned long size;
	unsigned long static_off;	/* [0, static_off) solapa una sombra: se mapea por pagina */
	void __iomem *iomem;		/* mapeo unico de [static_off, size), NULL si no hay */
};

u64 lkpi_shadow_overlap_end(phys_addr_t start, size_t size);
void __iomem *lkpi_ioremap_dyn(phys_addr_t phys, size_t size);
void lkpi_io_mapping_unmap(void __iomem *addr);

static inline struct io_mapping *io_mapping_init_wc(struct io_mapping *iomap,
						    resource_size_t base, unsigned long size)
{
	unsigned long off = PAGE_ALIGN(lkpi_shadow_overlap_end(base, size));

	iomap->base = base;
	iomap->size = size;
	iomap->static_off = off < size ? off : size;
	iomap->iomem = NULL;
	if (iomap->static_off == size)
		return iomap;
	iomap->iomem = ioremap_wc(base + iomap->static_off, size - iomap->static_off);
	return iomap->iomem ? iomap : NULL;
}

static inline void io_mapping_fini(struct io_mapping *iomap)
{
	if (iomap->iomem)
		iounmap(iomap->iomem);
}

static inline struct io_mapping *io_mapping_create_wc(resource_size_t base, unsigned long size)
{
	struct io_mapping *iomap = kmalloc(sizeof(*iomap), GFP_KERNEL);

	if (!iomap)
		return NULL;
	if (!io_mapping_init_wc(iomap, base, size)) {
		kfree(iomap);
		return NULL;
	}
	return iomap;
}

static inline void io_mapping_free(struct io_mapping *iomap)
{
	io_mapping_fini(iomap);
	kfree(iomap);
}

static inline void __iomem *io_mapping_map_wc(struct io_mapping *iomap, unsigned long offset,
					      unsigned long size)
{
	if (iomap->iomem && offset >= iomap->static_off)
		return iomap->iomem + (offset - iomap->static_off);
	return lkpi_ioremap_dyn(iomap->base + offset, size);
}
#define io_mapping_map_local_wc(iomap, offset) io_mapping_map_wc(iomap, offset, PAGE_SIZE)
#define io_mapping_map_atomic_wc(iomap, offset) io_mapping_map_wc(iomap, offset, PAGE_SIZE)
#define io_mapping_unmap(addr) lkpi_io_mapping_unmap(addr)
#define io_mapping_unmap_local(addr) lkpi_io_mapping_unmap(addr)
#define io_mapping_unmap_atomic(addr) lkpi_io_mapping_unmap(addr)

#endif
