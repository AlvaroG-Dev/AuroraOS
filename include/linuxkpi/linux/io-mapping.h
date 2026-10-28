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

struct io_mapping {
	resource_size_t base;
	unsigned long size;
	void __iomem *iomem;
};

static inline struct io_mapping *io_mapping_init_wc(struct io_mapping *iomap,
						    resource_size_t base, unsigned long size)
{
	iomap->base = base;
	iomap->size = size;
	iomap->iomem = ioremap_wc(base, size);
	return iomap->iomem ? iomap : NULL;
}

static inline void io_mapping_fini(struct io_mapping *iomap)
{
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
	return iomap->iomem + offset;
}
#define io_mapping_map_local_wc(iomap, offset) io_mapping_map_wc(iomap, offset, PAGE_SIZE)
#define io_mapping_map_atomic_wc(iomap, offset) io_mapping_map_wc(iomap, offset, PAGE_SIZE)
#define io_mapping_unmap(addr) do { (void)(addr); } while (0)
#define io_mapping_unmap_local(addr) do { (void)(addr); } while (0)
#define io_mapping_unmap_atomic(addr) do { (void)(addr); } while (0)

#endif
