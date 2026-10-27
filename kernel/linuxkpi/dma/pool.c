// SPDX-License-Identifier: GPL-2.0-only
/*
 * dma_pool (K1-DMA, L4): bloques de tamano fijo sacados de paginas de
 * memoria coherente del dispositivo. Cada pagina (o grupo de paginas si el
 * bloque es mayor) se trocea sin cruzar `boundary`; los libres van en una
 * lista. Las paginas no se devuelven hasta destruir el pool.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

struct lkpi_pool_page {
	struct list_head list;
	void *vaddr;
	dma_addr_t dma;
};

struct lkpi_pool_block {
	struct lkpi_pool_block *next;
	dma_addr_t dma;
};

struct dma_pool {
	struct device *dev;
	char name[32];
	size_t size, alloc_size, boundary;
	spinlock_t lock;
	struct list_head pages;
	struct lkpi_pool_block *free;
	unsigned int nr_blocks, nr_active;
};

struct dma_pool *lkpi_dma_pool_create(const char *name, struct device *dev, size_t size,
				      size_t align, size_t boundary)
{
	struct dma_pool *p;

	if (!size)
		return NULL;
	if (!align)
		align = 1;
	if (align & (align - 1))
		return NULL;
	size = ALIGN(max_t(size_t, size, sizeof(struct lkpi_pool_block)), align);
	if (!boundary)
		boundary = max_t(size_t, size, PAGE_SIZE);
	else if ((boundary & (boundary - 1)) || boundary < size)
		return NULL;
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return NULL;
	p->dev = dev;
	strscpy(p->name, name, sizeof(p->name));
	p->size = size;
	p->boundary = boundary;
	p->alloc_size = max_t(size_t, PAGE_SIZE, ALIGN(size, PAGE_SIZE));
	spin_lock_init(&p->lock);
	INIT_LIST_HEAD(&p->pages);
	return p;
}
EXPORT_SYMBOL(lkpi_dma_pool_create);

static int lkpi_pool_grow(struct dma_pool *p, gfp_t gfp)
{
	struct lkpi_pool_page *pg = kmalloc(sizeof(*pg), gfp);
	unsigned long flags;
	size_t off, next_boundary;

	if (!pg)
		return -ENOMEM;
	pg->vaddr = dma_alloc_coherent(p->dev, p->alloc_size, &pg->dma, gfp);
	if (!pg->vaddr) {
		kfree(pg);
		return -ENOMEM;
	}
	spin_lock_irqsave(&p->lock, flags);
	next_boundary = p->boundary;
	for (off = 0; off + p->size <= p->alloc_size; off += p->size) {
		struct lkpi_pool_block *b;

		if (off + p->size > next_boundary) {
			off = next_boundary;
			next_boundary += p->boundary;
			if (off + p->size > p->alloc_size)
				break;
		}
		b = pg->vaddr + off;
		b->dma = pg->dma + off;
		b->next = p->free;
		p->free = b;
		p->nr_blocks++;
	}
	list_add(&pg->list, &p->pages);
	spin_unlock_irqrestore(&p->lock, flags);
	return 0;
}

void *lkpi_dma_pool_alloc(struct dma_pool *p, gfp_t gfp, dma_addr_t *handle)
{
	struct lkpi_pool_block *b;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&p->lock, flags);
		b = p->free;
		if (b) {
			p->free = b->next;
			p->nr_active++;
		}
		spin_unlock_irqrestore(&p->lock, flags);
		if (b)
			break;
		if (lkpi_pool_grow(p, gfp))
			return NULL;
	}
	*handle = b->dma;
	if (gfp & __GFP_ZERO)
		memset(b, 0, p->size);
	return b;
}
EXPORT_SYMBOL(lkpi_dma_pool_alloc);

void lkpi_dma_pool_free(struct dma_pool *p, void *vaddr, dma_addr_t addr)
{
	struct lkpi_pool_block *b = vaddr;
	unsigned long flags;

	if (!vaddr)
		return;
	b->dma = addr;
	spin_lock_irqsave(&p->lock, flags);
	b->next = p->free;
	p->free = b;
	p->nr_active--;
	spin_unlock_irqrestore(&p->lock, flags);
}
EXPORT_SYMBOL(lkpi_dma_pool_free);

void lkpi_dma_pool_destroy(struct dma_pool *p)
{
	struct lkpi_pool_page *pg, *n;

	if (!p)
		return;
	if (p->nr_active)
		dev_err(p->dev, "dma_pool_destroy %s: %u bloques en uso\n", p->name, p->nr_active);
	list_for_each_entry_safe(pg, n, &p->pages, list) {
		dma_free_coherent(p->dev, p->alloc_size, pg->vaddr, pg->dma);
		kfree(pg);
	}
	kfree(p);
}
EXPORT_SYMBOL(lkpi_dma_pool_destroy);

static void lkpi_dmam_pool_release(struct device *dev, void *res)
{
	lkpi_dma_pool_destroy(*(struct dma_pool **)res);
}

struct dma_pool *lkpi_dmam_pool_create(const char *name, struct device *dev, size_t size,
				       size_t align, size_t allocation)
{
	struct dma_pool **ptr, *pool;

	ptr = devres_alloc(lkpi_dmam_pool_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return NULL;
	pool = *ptr = lkpi_dma_pool_create(name, dev, size, align, allocation);
	if (pool)
		devres_add(dev, ptr);
	else
		devres_free(ptr);
	return pool;
}
EXPORT_SYMBOL(lkpi_dmam_pool_create);
