// SPDX-License-Identifier: GPL-2.0-only
/*
 * kmalloc/kmem_cache de la capa: un slab sencillo sobre lkpi_alloc_pages().
 *  - kmalloc <= 2 KiB: caches por potencia de dos (8..2048), objetos
 *    alineados a su tamano dentro de una pagina.
 *  - kmalloc > 2 KiB: paginas contiguas de orden get_order(size).
 *  - kmem_cache: objetos del tamano pedido (redondeado a la alineacion) en
 *    slabs de orden suficiente para al menos 8 objetos.
 * Los metadatos de cada slab van en su struct page (slab_cache, freelist,
 * inuse); el puntero de la lista libre va dentro del objeto libre, salvo
 * con constructor, que va justo detras para no pisar el estado construido.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "../aurora.h"
#include "../lkpi_internal.h"

struct kmem_cache {
	const char *name;
	unsigned int object_size;	/* lo que pidio el usuario */
	unsigned int size;		/* paso entre objetos */
	unsigned int align;
	unsigned int fp_offset;		/* donde va el enlace de la lista libre */
	unsigned int order;
	unsigned int objs_per_slab;
	slab_flags_t flags;
	void (*ctor)(void *);
	spinlock_t lock;
	struct list_head partial;	/* slabs con objetos libres */
	struct list_head full;
	unsigned long nr_slabs;
	unsigned long nr_inuse;
	struct list_head list;		/* caches_list */
};

#define KMALLOC_MIN_SHIFT 3
#define KMALLOC_MAX_SMALL_SHIFT 11
#define KMALLOC_NR (KMALLOC_MAX_SMALL_SHIFT - KMALLOC_MIN_SHIFT + 1)

static struct kmem_cache kmalloc_caches[KMALLOC_NR];
static const char *const kmalloc_names[KMALLOC_NR] = {
	"kmalloc-8", "kmalloc-16", "kmalloc-32", "kmalloc-64", "kmalloc-128",
	"kmalloc-256", "kmalloc-512", "kmalloc-1k", "kmalloc-2k",
};
static LIST_HEAD(caches_list);
static DEFINE_SPINLOCK(caches_lock);
static bool slab_ready;

static void **obj_fp(struct kmem_cache *s, void *obj)
{
	return (void **)((char *)obj + s->fp_offset);
}

static void cache_setup(struct kmem_cache *s, const char *name, unsigned int size,
			unsigned int align, slab_flags_t flags, void (*ctor)(void *))
{
	unsigned int sz;

	memset(s, 0, sizeof(*s));
	if (align < ARCH_SLAB_MINALIGN)
		align = ARCH_SLAB_MINALIGN;
	if (flags & SLAB_HWCACHE_ALIGN)
		align = max(align, 64U);
	s->name = name;
	s->object_size = size;
	s->align = align;
	s->flags = flags;
	s->ctor = ctor;
	sz = max(size, (unsigned int)sizeof(void *));
	if (ctor || (flags & SLAB_TYPESAFE_BY_RCU)) {
		s->fp_offset = ALIGN(sz, sizeof(void *));
		sz = s->fp_offset + sizeof(void *);
	}
	s->size = ALIGN(sz, align);
	/* Al menos 8 objetos por slab (o 1 si el objeto es enorme). */
	s->order = get_order((unsigned long)s->size * 8);
	if (s->order > 3)
		s->order = get_order(s->size);
	s->objs_per_slab = (PAGE_SIZE << s->order) / s->size;
	spin_lock_init(&s->lock);
	INIT_LIST_HEAD(&s->partial);
	INIT_LIST_HEAD(&s->full);
}

/* Slab nuevo: paginas + lista libre (con ctor ya aplicado). */
static struct page *new_slab(struct kmem_cache *s, gfp_t gfp)
{
	struct page *page = lkpi_alloc_pages(gfp & ~__GFP_ZERO, s->order);
	char *base;
	unsigned int i;

	if (!page)
		return NULL;
	base = page_address(page);
	page->slab_cache = s;
	page->inuse = 0;
	page->freelist = NULL;
	for (i = s->objs_per_slab; i-- > 0;) {
		void *obj = base + i * s->size;
		if (s->ctor)
			s->ctor(obj);
		*obj_fp(s, obj) = page->freelist;
		page->freelist = obj;
	}
	return page;
}

static void *slab_alloc(struct kmem_cache *s, gfp_t gfp)
{
	unsigned long flags;
	struct page *page;
	void *obj;

	spin_lock_irqsave(&s->lock, flags);
	if (list_empty(&s->partial)) {
		spin_unlock_irqrestore(&s->lock, flags);
		page = new_slab(s, gfp);
		if (!page)
			return NULL;
		spin_lock_irqsave(&s->lock, flags);
		list_add(&page->lru, &s->partial);
		s->nr_slabs++;
	}
	page = list_first_entry(&s->partial, struct page, lru);
	obj = page->freelist;
	page->freelist = *obj_fp(s, obj);
	page->inuse++;
	s->nr_inuse++;
	if (!page->freelist)
		list_move(&page->lru, &s->full);
	spin_unlock_irqrestore(&s->lock, flags);

	if (gfp & __GFP_ZERO)
		memset(obj, 0, s->object_size);
	return obj;
}

static void slab_free(struct kmem_cache *s, struct page *page, void *obj)
{
	unsigned long flags;
	struct page *to_free = NULL;

	spin_lock_irqsave(&s->lock, flags);
	if (!page->freelist)
		list_move(&page->lru, &s->partial);
	*obj_fp(s, obj) = page->freelist;
	page->freelist = obj;
	page->inuse--;
	s->nr_inuse--;
	/* Slab vacio: se devuelve salvo que sea el unico con hueco. */
	if (page->inuse == 0 && !list_is_singular(&s->partial)) {
		list_del(&page->lru);
		s->nr_slabs--;
		to_free = page;
	}
	spin_unlock_irqrestore(&s->lock, flags);
	if (to_free) {
		to_free->slab_cache = NULL;
		lkpi_free_pages_page(to_free, s->order);
	}
}

/* Cabeza de la asignacion a la que pertenece p (o NULL si no es nuestra). */
static struct page *obj_head_page(const void *p)
{
	struct page *page;

	if (!lkpi_is_direct_map(p))
		return NULL;
	page = lkpi_page_lookup(__pa(p) >> PAGE_SHIFT);
	return page ? compound_head(page) : NULL;
}

static struct kmem_cache *kmalloc_cache_for(size_t size)
{
	unsigned int shift = size <= 8 ? KMALLOC_MIN_SHIFT : fls_long(size - 1);
	return &kmalloc_caches[shift - KMALLOC_MIN_SHIFT];
}

void *lkpi_kmalloc(size_t size, gfp_t gfp)
{
	struct page *page;

	if (unlikely(!size))
		return ZERO_SIZE_PTR;
	if (size <= (1U << KMALLOC_MAX_SMALL_SHIFT))
		return slab_alloc(kmalloc_cache_for(size), gfp);
	if (size > KMALLOC_MAX_SIZE) {
		WARN_ON_ONCE(!(gfp & __GFP_NOWARN));
		return NULL;
	}
	page = lkpi_alloc_pages(gfp, get_order(size));
	return page ? page_address(page) : NULL;
}
EXPORT_SYMBOL(lkpi_kmalloc);

void lkpi_kfree(const void *p)
{
	struct page *head;

	if (unlikely(ZERO_OR_NULL_PTR(p)))
		return;
	head = obj_head_page(p);
	if (WARN(!head || !(head->lkpi_flags & LKPI_PAGE_OWNED), "kfree de un puntero ajeno: %px\n", p))
		return;
	if (head->slab_cache) {
		slab_free(head->slab_cache, head, (void *)p);
		return;
	}
	if (WARN(p != page_address(head), "kfree de un puntero intermedio: %px\n", p))
		return;
	lkpi_free_pages_page(head, head->order);
}
EXPORT_SYMBOL(lkpi_kfree);

size_t lkpi_ksize(const void *p)
{
	struct page *head;

	if (ZERO_OR_NULL_PTR(p))
		return 0;
	head = obj_head_page(p);
	if (!head)
		return 0;
	if (head->slab_cache)
		return head->slab_cache->object_size;
	return PAGE_SIZE << head->order;
}
EXPORT_SYMBOL(lkpi_ksize);

void *lkpi_krealloc(const void *p, size_t new_size, gfp_t gfp)
{
	size_t old_size;
	void *n;

	if (unlikely(!new_size)) {
		lkpi_kfree(p);
		return ZERO_SIZE_PTR;
	}
	old_size = lkpi_ksize(p);
	if (p && !ZERO_OR_NULL_PTR(p) && new_size <= old_size) {
		if (gfp & __GFP_ZERO)
			memset((char *)p + new_size, 0, old_size - new_size);
		return (void *)p;
	}
	n = lkpi_kmalloc(new_size, gfp);
	if (!n)
		return NULL;
	if (!ZERO_OR_NULL_PTR(p)) {
		memcpy(n, p, old_size);
		if (gfp & __GFP_ZERO)
			memset((char *)n + old_size, 0, new_size - old_size);
		lkpi_kfree(p);
	}
	return n;
}
EXPORT_SYMBOL(lkpi_krealloc);

/* ------------------------------------------------------- kvmalloc */

void *lkpi_kvmalloc(size_t size, gfp_t gfp)
{
	void *p = NULL;

	if (size <= 64 * PAGE_SIZE)
		p = lkpi_kmalloc(size, gfp | __GFP_NOWARN);
	if (!p)
		p = lkpi_vmalloc(size, gfp);
	return p;
}
EXPORT_SYMBOL(lkpi_kvmalloc);

void lkpi_kvfree(const void *p)
{
	if (lkpi_is_vmalloc_addr(p))
		lkpi_vfree(p);
	else
		lkpi_kfree(p);
}
EXPORT_SYMBOL(lkpi_kvfree);

size_t lkpi_vmalloc_size(const void *p);

void *lkpi_kvrealloc(const void *p, size_t size, gfp_t gfp)
{
	size_t old;
	void *n;

	if (!p)
		return lkpi_kvmalloc(size, gfp);
	if (!lkpi_is_vmalloc_addr(p))
		old = lkpi_ksize(p);
	else
		old = lkpi_vmalloc_size(p);
	if (size <= old)
		return (void *)p;
	n = lkpi_kvmalloc(size, gfp);
	if (!n)
		return NULL;
	memcpy(n, p, old);
	if (gfp & __GFP_ZERO)
		memset((char *)n + old, 0, size - old);
	lkpi_kvfree(p);
	return n;
}
EXPORT_SYMBOL(lkpi_kvrealloc);

/* ------------------------------------------------------ kmem_cache */

struct kmem_cache *lkpi_kmem_cache_create(const char *name, unsigned int size,
					  unsigned int align, slab_flags_t flags,
					  void (*ctor)(void *))
{
	struct kmem_cache *s;
	unsigned long irqflags;

	if (WARN_ON(!size || size > KMALLOC_MAX_SIZE / 8))
		return NULL;
	if (align & (align - 1))
		return NULL;
	s = aurora_kmalloc(sizeof(*s));
	if (!s) {
		if (flags & SLAB_PANIC)
			panic("kmem_cache_create: sin memoria para %s", name);
		return NULL;
	}
	cache_setup(s, name, size, align, flags, ctor);
	spin_lock_irqsave(&caches_lock, irqflags);
	list_add(&s->list, &caches_list);
	spin_unlock_irqrestore(&caches_lock, irqflags);
	return s;
}
EXPORT_SYMBOL(lkpi_kmem_cache_create);

void *lkpi_kmem_cache_alloc(struct kmem_cache *s, gfp_t gfp)
{
	return slab_alloc(s, gfp);
}
EXPORT_SYMBOL(lkpi_kmem_cache_alloc);

void lkpi_kmem_cache_free(struct kmem_cache *s, void *p)
{
	struct page *head;

	if (!p)
		return;
	head = obj_head_page(p);
	if (WARN(!head || head->slab_cache != s, "kmem_cache_free(%s): objeto ajeno %px\n",
		 s->name, p))
		return;
	slab_free(s, head, p);
}
EXPORT_SYMBOL(lkpi_kmem_cache_free);

static void cache_release_slabs(struct kmem_cache *s)
{
	struct page *page, *tmp;

	if (s->nr_inuse)
		pr_err("linuxkpi: kmem_cache %s destruida con %lu objetos en uso\n",
		       s->name, s->nr_inuse);
	list_for_each_entry_safe(page, tmp, &s->partial, lru) {
		list_del(&page->lru);
		page->slab_cache = NULL;
		lkpi_free_pages_page(page, s->order);
	}
	/* Los slabs llenos con objetos vivos se quedan: liberarlos corromperia. */
}

void lkpi_kmem_cache_destroy(struct kmem_cache *s)
{
	unsigned long flags;

	if (!s)
		return;
	spin_lock_irqsave(&caches_lock, flags);
	list_del(&s->list);
	spin_unlock_irqrestore(&caches_lock, flags);
	cache_release_slabs(s);
	aurora_kfree(s);
}
EXPORT_SYMBOL(lkpi_kmem_cache_destroy);

unsigned int lkpi_kmem_cache_size(struct kmem_cache *s)
{
	return s->object_size;
}
EXPORT_SYMBOL(lkpi_kmem_cache_size);

int lkpi_slab_init(void)
{
	for (unsigned int i = 0; i < KMALLOC_NR; i++) {
		unsigned int sz = 1U << (i + KMALLOC_MIN_SHIFT);
		cache_setup(&kmalloc_caches[i], kmalloc_names[i], sz, sz, 0, NULL);
	}
	slab_ready = true;
	return 0;
}

void lkpi_slab_exit(void)
{
	for (unsigned int i = 0; i < KMALLOC_NR; i++)
		cache_release_slabs(&kmalloc_caches[i]);
	slab_ready = false;
}
