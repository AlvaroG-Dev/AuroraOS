/* SPDX-License-Identifier: GPL-2.0-only */
/* Puntero a memoria de sistema o de E/S (dma_buf_vmap, DRM). En x86 la de
 * E/S tambien se lee con memcpy_fromio/memcpy_toio. */
#ifndef _LKPI_LINUX_IOSYS_MAP_H
#define _LKPI_LINUX_IOSYS_MAP_H

#include <linux/types.h>
#include <linux/string.h>
#include <linux/io.h>

struct iosys_map {
	union {
		void __iomem *vaddr_iomem;
		void *vaddr;
	};
	bool is_iomem;
};

#define IOSYS_MAP_INIT_VADDR(vaddr_) { .vaddr = (vaddr_), .is_iomem = false }
#define IOSYS_MAP_INIT_VADDR_IOMEM(vaddr_iomem_) \
	{ .vaddr_iomem = (vaddr_iomem_), .is_iomem = true }
#define IOSYS_MAP_INIT_OFFSET(map_, offset_) ({ \
	struct iosys_map copy_ = *(map_); \
	iosys_map_incr(&copy_, offset_); \
	copy_; })

static inline void iosys_map_set_vaddr(struct iosys_map *map, void *vaddr)
{
	map->vaddr = vaddr;
	map->is_iomem = false;
}
static inline void iosys_map_set_vaddr_iomem(struct iosys_map *map, void __iomem *vaddr_iomem)
{
	map->vaddr_iomem = vaddr_iomem;
	map->is_iomem = true;
}
static inline bool iosys_map_is_null(const struct iosys_map *map) { return !map->vaddr; }
static inline bool iosys_map_is_set(const struct iosys_map *map) { return !!map->vaddr; }
static inline bool iosys_map_is_equal(const struct iosys_map *a, const struct iosys_map *b)
{
	return a->is_iomem == b->is_iomem && a->vaddr == b->vaddr;
}
static inline void iosys_map_clear(struct iosys_map *map)
{
	map->vaddr = NULL;
	map->is_iomem = false;
}
static inline void iosys_map_incr(struct iosys_map *map, size_t incr)
{
	map->vaddr = (u8 *)map->vaddr + incr;
}
static inline void iosys_map_memcpy_to(struct iosys_map *dst, size_t dst_offset,
				       const void *src, size_t len)
{
	if (dst->is_iomem)
		memcpy_toio(dst->vaddr_iomem + dst_offset, src, len);
	else
		memcpy(dst->vaddr + dst_offset, src, len);
}
static inline void iosys_map_memcpy_from(void *dst, const struct iosys_map *src,
					 size_t src_offset, size_t len)
{
	if (src->is_iomem)
		memcpy_fromio(dst, src->vaddr_iomem + src_offset, len);
	else
		memcpy(dst, src->vaddr + src_offset, len);
}
static inline void iosys_map_memset(struct iosys_map *dst, size_t offset, int value, size_t len)
{
	if (dst->is_iomem)
		memset_io(dst->vaddr_iomem + offset, value, len);
	else
		memset(dst->vaddr + offset, value, len);
}

#endif
