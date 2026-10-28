/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kmalloc y kmem_cache de la capa. Igual que en Linux, la memoria de
 * kmalloc es fisicamente contigua (paginas del PMM de Aurora vistas por la
 * ventana directa), virt_to_page() funciona sobre ella y los tamanos
 * potencia de dos quedan alineados a su tamano.
 */
#ifndef _LKPI_LINUX_SLAB_H
#define _LKPI_LINUX_SLAB_H

#include <linux/compiler.h>
#include <linux/cache.h>
#include <linux/gfp.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <linux/types.h>

#define SLAB_HWCACHE_ALIGN ((slab_flags_t)0x00002000U)
#define SLAB_CACHE_DMA ((slab_flags_t)0x00004000U)
#define SLAB_CACHE_DMA32 ((slab_flags_t)0x00008000U)
#define SLAB_PANIC ((slab_flags_t)0x00040000U)
#define SLAB_TYPESAFE_BY_RCU ((slab_flags_t)0x00080000U)
#define SLAB_ACCOUNT ((slab_flags_t)0x04000000U)
#define SLAB_RECLAIM_ACCOUNT ((slab_flags_t)0x00020000U)
#define SLAB_TEMPORARY SLAB_RECLAIM_ACCOUNT
#define SLAB_NO_MERGE ((slab_flags_t)0x01000000U)
#define SLAB_POISON ((slab_flags_t)0x00000800U)
#define SLAB_RED_ZONE ((slab_flags_t)0x00000400U)

#define ARCH_KMALLOC_MINALIGN 8
#ifndef ARCH_DMA_MINALIGN
#define ARCH_DMA_MINALIGN 64
#endif
#define ARCH_SLAB_MINALIGN 8
#define KMALLOC_MAX_SIZE (1UL << (12 + 10))
#define KMALLOC_MAX_CACHE_SIZE 4096
#define ZERO_SIZE_PTR ((void *)16)
#define ZERO_OR_NULL_PTR(x) ((unsigned long)(x) <= (unsigned long)ZERO_SIZE_PTR)

struct kmem_cache;

void *lkpi_kmalloc(size_t size, gfp_t gfp) __malloc;
void lkpi_kfree(const void *p);
void *lkpi_krealloc(const void *p, size_t new_size, gfp_t gfp);
size_t lkpi_ksize(const void *p);
void *lkpi_kvmalloc(size_t size, gfp_t gfp) __malloc;
void lkpi_kvfree(const void *p);
void *lkpi_kvrealloc(const void *p, size_t size, gfp_t gfp);
struct kmem_cache *lkpi_kmem_cache_create(const char *name, unsigned int size,
					  unsigned int align, slab_flags_t flags,
					  void (*ctor)(void *));
void *lkpi_kmem_cache_alloc(struct kmem_cache *c, gfp_t gfp) __malloc;
void lkpi_kmem_cache_free(struct kmem_cache *c, void *p);
void lkpi_kmem_cache_destroy(struct kmem_cache *c);
unsigned int lkpi_kmem_cache_size(struct kmem_cache *c);

static inline __alloc_size(1) void *kmalloc(size_t size, gfp_t gfp)
{
	return lkpi_kmalloc(size, gfp);
}
static inline __alloc_size(1) void *kzalloc(size_t size, gfp_t gfp)
{
	return lkpi_kmalloc(size, gfp | __GFP_ZERO);
}
static inline void *kmalloc_array(size_t n, size_t size, gfp_t gfp)
{
	size_t bytes;
	if (unlikely(check_mul_overflow(n, size, &bytes)))
		return NULL;
	return lkpi_kmalloc(bytes, gfp);
}
static inline void *kcalloc(size_t n, size_t size, gfp_t gfp)
{
	return kmalloc_array(n, size, gfp | __GFP_ZERO);
}
static inline void *krealloc(const void *p, size_t new_size, gfp_t gfp)
{
	return lkpi_krealloc(p, new_size, gfp);
}
static inline void *krealloc_array(void *p, size_t new_n, size_t new_size, gfp_t gfp)
{
	size_t bytes;
	if (unlikely(check_mul_overflow(new_n, new_size, &bytes)))
		return NULL;
	return lkpi_krealloc(p, bytes, gfp);
}
static inline void kfree(const void *p) { lkpi_kfree(p); }
static inline void kfree_sensitive(const void *p)
{
	if (!ZERO_OR_NULL_PTR(p))
		memzero_explicit((void *)p, lkpi_ksize(p));
	lkpi_kfree(p);
}
#define kvfree_sensitive(p, len) kfree_sensitive(p)
static inline size_t ksize(const void *p) { return lkpi_ksize(p); }
#define kmalloc_node(s, g, n) kmalloc(s, g)
#define kzalloc_node(s, g, n) kzalloc(s, g)
#define kcalloc_node(n, s, g, node) kcalloc(n, s, g)
#define kmalloc_array_node(n, s, g, node) kmalloc_array(n, s, g)
#define kmalloc_track_caller(s, g) kmalloc(s, g)
#define kmalloc_node_track_caller(s, g, n) kmalloc_node(s, g, n)
#define kmalloc_size_roundup(s) (s)
#define kmalloc_obj(p, g) ((typeof(p))kmalloc(sizeof(*(p)), g))

static inline void *kvmalloc(size_t size, gfp_t gfp) { return lkpi_kvmalloc(size, gfp); }
static inline void *kvzalloc(size_t size, gfp_t gfp) { return lkpi_kvmalloc(size, gfp | __GFP_ZERO); }
static inline void *kvmalloc_array(size_t n, size_t size, gfp_t gfp)
{
	size_t bytes;
	if (unlikely(check_mul_overflow(n, size, &bytes)))
		return NULL;
	return lkpi_kvmalloc(bytes, gfp);
}
static inline void *kvcalloc(size_t n, size_t size, gfp_t gfp)
{
	return kvmalloc_array(n, size, gfp | __GFP_ZERO);
}
static inline void kvfree(const void *p) { lkpi_kvfree(p); }
static inline void *kvrealloc(const void *p, size_t size, gfp_t gfp)
{
	return lkpi_kvrealloc(p, size, gfp);
}
#define kvmalloc_node(s, g, n) kvmalloc(s, g)
#define kvzalloc_node(s, g, n) kvzalloc(s, g)

static inline struct kmem_cache *kmem_cache_create(const char *name, unsigned int size,
						   unsigned int align, slab_flags_t flags,
						   void (*ctor)(void *))
{
	return lkpi_kmem_cache_create(name, size, align, flags, ctor);
}
#define kmem_cache_create_usercopy(name, size, align, flags, uo, us, ctor) \
	kmem_cache_create(name, size, align, flags, ctor)
#define KMEM_CACHE(__struct, __flags) \
	kmem_cache_create(#__struct, sizeof(struct __struct), \
			  __alignof__(struct __struct), (__flags), NULL)
#define KMEM_CACHE_USERCOPY(__struct, __flags, __field) KMEM_CACHE(__struct, __flags)
static inline void *kmem_cache_alloc(struct kmem_cache *c, gfp_t gfp)
{
	return lkpi_kmem_cache_alloc(c, gfp);
}
static inline void *kmem_cache_zalloc(struct kmem_cache *c, gfp_t gfp)
{
	return lkpi_kmem_cache_alloc(c, gfp | __GFP_ZERO);
}
#define kmem_cache_alloc_node(c, g, n) kmem_cache_alloc(c, g)
#define kmem_cache_alloc_lru(c, l, g) kmem_cache_alloc(c, g)
static inline void kmem_cache_free(struct kmem_cache *c, void *p) { lkpi_kmem_cache_free(c, p); }
static inline void kmem_cache_destroy(struct kmem_cache *c) { lkpi_kmem_cache_destroy(c); }
static inline unsigned int kmem_cache_size(struct kmem_cache *c) { return lkpi_kmem_cache_size(c); }
static inline int kmem_cache_shrink(struct kmem_cache *c) { return 0; }

#endif
