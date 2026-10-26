// SPDX-License-Identifier: GPL-2.0-only
/*
 * devres (K4): recursos asociados a un dispositivo que se liberan solos, en
 * orden inverso, al fallar el probe, al desvincular el driver o al liberar
 * el device. Grupos (devres_open_group) como marcas en la misma lista.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sprintf.h>
#include <linux/io.h>
#include <linux/gfp.h>
#include "base.h"
#include "../lkpi_internal.h"

struct lkpi_devres {
	struct list_head entry;
	dr_release_t release;
	const char *name;
	size_t size;
	void *group_id;			/* marca de grupo: release == NULL */
	bool group_open;
	unsigned long long data[];	/* alineado a 8, como ARCH_KMALLOC_MINALIGN */
};

#define to_devres(res) container_of((void *)(res), struct lkpi_devres, data)

static void lkpi_group_release(struct device *dev, void *res) { }

void *lkpi_devres_alloc(dr_release_t release, size_t size, gfp_t gfp, const char *name)
{
	struct lkpi_devres *dr;

	dr = kmalloc(sizeof(*dr) + size, gfp | __GFP_ZERO);
	if (!dr)
		return NULL;
	INIT_LIST_HEAD(&dr->entry);
	dr->release = release;
	dr->name = name;
	dr->size = size;
	return dr->data;
}
EXPORT_SYMBOL(lkpi_devres_alloc);

void lkpi_devres_free(void *res)
{
	if (res) {
		struct lkpi_devres *dr = to_devres(res);

		WARN_ON(!list_empty(&dr->entry));
		kfree(dr);
	}
}
EXPORT_SYMBOL(lkpi_devres_free);

void lkpi_devres_add(struct device *dev, void *res)
{
	struct lkpi_devres *dr = to_devres(res);
	unsigned long flags;

	spin_lock_irqsave(&dev->devres_lock, flags);
	list_add_tail(&dr->entry, &dev->devres_head);
	spin_unlock_irqrestore(&dev->devres_lock, flags);
}
EXPORT_SYMBOL(lkpi_devres_add);

static struct lkpi_devres *lkpi_find_dr(struct device *dev, dr_release_t release,
					dr_match_t match, void *match_data)
{
	struct lkpi_devres *dr;

	list_for_each_entry_reverse(dr, &dev->devres_head, entry) {
		if (dr->release != release)
			continue;
		if (match && !match(dev, dr->data, match_data))
			continue;
		return dr;
	}
	return NULL;
}

void *lkpi_devres_find(struct device *dev, dr_release_t release, dr_match_t match,
		       void *match_data)
{
	struct lkpi_devres *dr;
	unsigned long flags;

	spin_lock_irqsave(&dev->devres_lock, flags);
	dr = lkpi_find_dr(dev, release, match, match_data);
	spin_unlock_irqrestore(&dev->devres_lock, flags);
	return dr ? dr->data : NULL;
}
EXPORT_SYMBOL(lkpi_devres_find);

void *lkpi_devres_get(struct device *dev, void *new_res, dr_match_t match, void *match_data)
{
	struct lkpi_devres *new_dr = to_devres(new_res), *dr;
	unsigned long flags;

	spin_lock_irqsave(&dev->devres_lock, flags);
	dr = lkpi_find_dr(dev, new_dr->release, match, match_data);
	if (!dr) {
		list_add_tail(&new_dr->entry, &dev->devres_head);
		dr = new_dr;
		new_res = NULL;
	}
	spin_unlock_irqrestore(&dev->devres_lock, flags);
	lkpi_devres_free(new_res);
	return dr->data;
}
EXPORT_SYMBOL(lkpi_devres_get);

void *lkpi_devres_remove(struct device *dev, dr_release_t release, dr_match_t match,
			 void *match_data)
{
	struct lkpi_devres *dr;
	unsigned long flags;

	spin_lock_irqsave(&dev->devres_lock, flags);
	dr = lkpi_find_dr(dev, release, match, match_data);
	if (dr)
		list_del_init(&dr->entry);
	spin_unlock_irqrestore(&dev->devres_lock, flags);
	return dr ? dr->data : NULL;
}
EXPORT_SYMBOL(lkpi_devres_remove);

int lkpi_devres_destroy(struct device *dev, dr_release_t release, dr_match_t match,
			void *match_data)
{
	void *res = lkpi_devres_remove(dev, release, match, match_data);

	if (unlikely(!res))
		return -ENOENT;
	lkpi_devres_free(res);
	return 0;
}
EXPORT_SYMBOL(lkpi_devres_destroy);

int lkpi_devres_release(struct device *dev, dr_release_t release, dr_match_t match,
			void *match_data)
{
	void *res = lkpi_devres_remove(dev, release, match, match_data);

	if (unlikely(!res))
		return -ENOENT;
	release(dev, res);
	lkpi_devres_free(res);
	return 0;
}
EXPORT_SYMBOL(lkpi_devres_release);

void lkpi_devres_for_each_res(struct device *dev, dr_release_t release, dr_match_t match,
			      void *match_data,
			      void (*fn)(struct device *, void *, void *), void *data)
{
	struct lkpi_devres *dr, *tmp;
	unsigned long flags;

	if (!fn)
		return;
	spin_lock_irqsave(&dev->devres_lock, flags);
	list_for_each_entry_safe_reverse(dr, tmp, &dev->devres_head, entry) {
		if (dr->release != release)
			continue;
		if (match && !match(dev, dr->data, match_data))
			continue;
		fn(dev, dr->data, data);
	}
	spin_unlock_irqrestore(&dev->devres_lock, flags);
}
EXPORT_SYMBOL(lkpi_devres_for_each_res);

/*
 * Libera, en orden inverso, las entradas posteriores a `from` (exclusivo;
 * NULL = todas). Las marcas de grupo se descartan. Cada entrada se saca de
 * la lista con el cerrojo y se libera sin el (release puede dormir).
 */
static int lkpi_release_after(struct device *dev, struct lkpi_devres *from)
{
	struct lkpi_devres *dr;
	unsigned long flags;
	int n = 0;

	for (;;) {
		spin_lock_irqsave(&dev->devres_lock, flags);
		if (list_empty(&dev->devres_head)) {
			spin_unlock_irqrestore(&dev->devres_lock, flags);
			break;
		}
		dr = list_last_entry(&dev->devres_head, struct lkpi_devres, entry);
		if (dr == from) {
			spin_unlock_irqrestore(&dev->devres_lock, flags);
			break;
		}
		list_del_init(&dr->entry);
		spin_unlock_irqrestore(&dev->devres_lock, flags);
		if (dr->release && dr->release != lkpi_group_release) {
			dr->release(dev, dr->data);
			n++;
		}
		kfree(dr);
	}
	return n;
}

int lkpi_devres_release_all(struct device *dev)
{
	if (!dev->devres_head.next)	/* sin inicializar */
		return -ENODEV;
	return lkpi_release_after(dev, NULL);
}

/* ------------------------------------------------------------- grupos */

static struct lkpi_devres *lkpi_find_group(struct device *dev, void *id)
{
	struct lkpi_devres *dr;

	list_for_each_entry_reverse(dr, &dev->devres_head, entry) {
		if (dr->release != lkpi_group_release)
			continue;
		if (id ? dr->group_id == id : dr->group_open)
			return dr;
	}
	return NULL;
}

void *lkpi_devres_open_group(struct device *dev, void *id, gfp_t gfp)
{
	void *res = lkpi_devres_alloc(lkpi_group_release, 0, gfp, "group");
	struct lkpi_devres *dr;

	if (!res)
		return NULL;
	dr = to_devres(res);
	dr->group_id = id ? id : res;
	dr->group_open = true;
	lkpi_devres_add(dev, res);
	return dr->group_id;
}
EXPORT_SYMBOL(lkpi_devres_open_group);

void lkpi_devres_close_group(struct device *dev, void *id)
{
	struct lkpi_devres *dr;
	unsigned long flags;

	spin_lock_irqsave(&dev->devres_lock, flags);
	dr = lkpi_find_group(dev, id);
	if (dr)
		dr->group_open = false;
	spin_unlock_irqrestore(&dev->devres_lock, flags);
	WARN_ON(!dr);
}
EXPORT_SYMBOL(lkpi_devres_close_group);

void lkpi_devres_remove_group(struct device *dev, void *id)
{
	struct lkpi_devres *dr;
	unsigned long flags;

	spin_lock_irqsave(&dev->devres_lock, flags);
	dr = lkpi_find_group(dev, id);
	if (dr)
		list_del_init(&dr->entry);
	spin_unlock_irqrestore(&dev->devres_lock, flags);
	if (dr)
		kfree(dr);
	else
		WARN_ON(1);
}
EXPORT_SYMBOL(lkpi_devres_remove_group);

/*
 * Libera lo abierto desde la marca del grupo (incluidos grupos anidados
 * posteriores). Como las entradas se anaden al final, "despues de la
 * marca" es el sufijo de la lista.
 */
int lkpi_devres_release_group(struct device *dev, void *id)
{
	struct lkpi_devres *dr;
	unsigned long flags;
	int n;

	spin_lock_irqsave(&dev->devres_lock, flags);
	dr = lkpi_find_group(dev, id);
	spin_unlock_irqrestore(&dev->devres_lock, flags);
	if (!dr) {
		WARN_ON(1);
		return 0;
	}
	n = lkpi_release_after(dev, dr);
	spin_lock_irqsave(&dev->devres_lock, flags);
	list_del_init(&dr->entry);
	spin_unlock_irqrestore(&dev->devres_lock, flags);
	kfree(dr);
	return n;
}
EXPORT_SYMBOL(lkpi_devres_release_group);

/* -------------------------------------------------------------- devm_* */

static void lkpi_devm_kmalloc_release(struct device *dev, void *res) { }

static int lkpi_devm_kmalloc_match(struct device *dev, void *res, void *data)
{
	return res == data;
}

void *lkpi_devm_kmalloc(struct device *dev, size_t size, gfp_t gfp)
{
	void *res;

	if (unlikely(!size))
		return ZERO_SIZE_PTR;
	res = lkpi_devres_alloc(lkpi_devm_kmalloc_release, size, gfp, "devm_kzalloc");
	if (unlikely(!res))
		return NULL;
	lkpi_devres_add(dev, res);
	return res;
}
EXPORT_SYMBOL(lkpi_devm_kmalloc);

void lkpi_devm_kfree(struct device *dev, const void *p)
{
	if (ZERO_OR_NULL_PTR(p))
		return;
	WARN_ON(lkpi_devres_destroy(dev, lkpi_devm_kmalloc_release, lkpi_devm_kmalloc_match,
				    (void *)p));
}
EXPORT_SYMBOL(lkpi_devm_kfree);

void *lkpi_devm_krealloc(struct device *dev, void *ptr, size_t new_size, gfp_t gfp)
{
	struct lkpi_devres *old;
	void *n;

	if (!ptr || ZERO_OR_NULL_PTR(ptr))
		return lkpi_devm_kmalloc(dev, new_size, gfp);
	if (!new_size) {
		lkpi_devm_kfree(dev, ptr);
		return ZERO_SIZE_PTR;
	}
	old = to_devres(ptr);
	if (old->size >= new_size)
		return ptr;
	n = lkpi_devm_kmalloc(dev, new_size, gfp);
	if (!n)
		return NULL;
	memcpy(n, ptr, old->size);
	lkpi_devm_kfree(dev, ptr);
	return n;
}
EXPORT_SYMBOL(lkpi_devm_krealloc);

char *lkpi_devm_kstrdup(struct device *dev, const char *s, gfp_t gfp)
{
	size_t size;
	char *buf;

	if (!s)
		return NULL;
	size = strlen(s) + 1;
	buf = lkpi_devm_kmalloc(dev, size, gfp);
	if (buf)
		memcpy(buf, s, size);
	return buf;
}
EXPORT_SYMBOL(lkpi_devm_kstrdup);

const char *lkpi_devm_kstrdup_const(struct device *dev, const char *s, gfp_t gfp)
{
	return lkpi_devm_kstrdup(dev, s, gfp);
}
EXPORT_SYMBOL(lkpi_devm_kstrdup_const);

void *lkpi_devm_kmemdup(struct device *dev, const void *src, size_t len, gfp_t gfp)
{
	void *p = lkpi_devm_kmalloc(dev, len, gfp);

	if (p)
		memcpy(p, src, len);
	return p;
}
EXPORT_SYMBOL(lkpi_devm_kmemdup);

char *lkpi_devm_kvasprintf(struct device *dev, gfp_t gfp, const char *fmt, va_list ap)
{
	unsigned int len;
	char *p;
	va_list aq;

	va_copy(aq, ap);
	len = vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);
	p = lkpi_devm_kmalloc(dev, len + 1, gfp);
	if (!p)
		return NULL;
	vsnprintf(p, len + 1, fmt, ap);
	return p;
}
EXPORT_SYMBOL(lkpi_devm_kvasprintf);

char *lkpi_devm_kasprintf(struct device *dev, gfp_t gfp, const char *fmt, ...)
{
	va_list ap;
	char *p;

	va_start(ap, fmt);
	p = lkpi_devm_kvasprintf(dev, gfp, fmt, ap);
	va_end(ap);
	return p;
}
EXPORT_SYMBOL(lkpi_devm_kasprintf);

struct lkpi_action_devres {
	void *data;
	void (*action)(void *);
};

static void lkpi_devm_action_release(struct device *dev, void *res)
{
	struct lkpi_action_devres *d = res;

	d->action(d->data);
}

static int lkpi_devm_action_match(struct device *dev, void *res, void *p)
{
	struct lkpi_action_devres *d = res, *target = p;

	return d->action == target->action && d->data == target->data;
}

int lkpi_devm_add_action(struct device *dev, void (*action)(void *), void *data,
			 const char *name)
{
	struct lkpi_action_devres *d;

	d = lkpi_devres_alloc(lkpi_devm_action_release, sizeof(*d), GFP_KERNEL, name);
	if (!d)
		return -ENOMEM;
	d->data = data;
	d->action = action;
	lkpi_devres_add(dev, d);
	return 0;
}
EXPORT_SYMBOL(lkpi_devm_add_action);

void lkpi_devm_remove_action(struct device *dev, void (*action)(void *), void *data)
{
	struct lkpi_action_devres d = { .data = data, .action = action };

	WARN_ON(lkpi_devres_destroy(dev, lkpi_devm_action_release, lkpi_devm_action_match, &d));
}
EXPORT_SYMBOL(lkpi_devm_remove_action);

void lkpi_devm_release_action(struct device *dev, void (*action)(void *), void *data)
{
	struct lkpi_action_devres d = { .data = data, .action = action };

	WARN_ON(lkpi_devres_release(dev, lkpi_devm_action_release, lkpi_devm_action_match, &d));
}
EXPORT_SYMBOL(lkpi_devm_release_action);

struct lkpi_pages_devres {
	unsigned long addr;
	unsigned int order;
};

static void lkpi_devm_pages_release(struct device *dev, void *res)
{
	struct lkpi_pages_devres *d = res;

	free_pages(d->addr, d->order);
}

static int lkpi_devm_pages_match(struct device *dev, void *res, void *p)
{
	return ((struct lkpi_pages_devres *)res)->addr == *(unsigned long *)p;
}

unsigned long lkpi_devm_get_free_pages(struct device *dev, gfp_t gfp, unsigned int order)
{
	struct lkpi_pages_devres *d;
	unsigned long addr = __get_free_pages(gfp, order);

	if (!addr)
		return 0;
	d = lkpi_devres_alloc(lkpi_devm_pages_release, sizeof(*d), GFP_KERNEL, "pages");
	if (!d) {
		free_pages(addr, order);
		return 0;
	}
	d->addr = addr;
	d->order = order;
	lkpi_devres_add(dev, d);
	return addr;
}
EXPORT_SYMBOL(lkpi_devm_get_free_pages);

void lkpi_devm_free_pages(struct device *dev, unsigned long addr)
{
	WARN_ON(lkpi_devres_release(dev, lkpi_devm_pages_release, lkpi_devm_pages_match, &addr));
}
EXPORT_SYMBOL(lkpi_devm_free_pages);

/* --------------------------------------------- devm_ioremap y regiones */

struct lkpi_ioremap_devres {
	void __iomem *addr;
};

static void lkpi_devm_ioremap_release(struct device *dev, void *res)
{
	iounmap(((struct lkpi_ioremap_devres *)res)->addr);
}

static int lkpi_devm_ioremap_match(struct device *dev, void *res, void *data)
{
	return ((struct lkpi_ioremap_devres *)res)->addr == data;
}

void __iomem *lkpi_devm_ioremap(struct device *dev, resource_size_t offset,
				resource_size_t size, int type)
{
	struct lkpi_ioremap_devres *d;
	void __iomem *addr;

	d = lkpi_devres_alloc(lkpi_devm_ioremap_release, sizeof(*d), GFP_KERNEL, "ioremap");
	if (!d)
		return NULL;
	addr = lkpi_ioremap(offset, size, type ? LKPI_IOREMAP_WC : LKPI_IOREMAP_UC);
	if (!addr) {
		lkpi_devres_free(d);
		return NULL;
	}
	d->addr = addr;
	lkpi_devres_add(dev, d);
	return addr;
}
EXPORT_SYMBOL(lkpi_devm_ioremap);

void lkpi_devm_iounmap(struct device *dev, void __iomem *addr)
{
	WARN_ON(lkpi_devres_destroy(dev, lkpi_devm_ioremap_release, lkpi_devm_ioremap_match,
				    (void __force *)addr));
	iounmap(addr);
}
EXPORT_SYMBOL(lkpi_devm_iounmap);

void __iomem *lkpi_devm_ioremap_resource(struct device *dev, const struct resource *res,
					 int type)
{
	resource_size_t size;
	void __iomem *dest;

	if (!res || resource_type(res) != IORESOURCE_MEM) {
		dev_err(dev, "recurso invalido\n");
		return IOMEM_ERR_PTR(-EINVAL);
	}
	size = resource_size(res);
	if (!lkpi_devm_request_region(dev, &iomem_resource, res->start, size,
				      res->name ? res->name : dev_name(dev))) {
		dev_err(dev, "no se pudo reservar la region %pa\n", &res->start);
		return IOMEM_ERR_PTR(-EBUSY);
	}
	dest = lkpi_devm_ioremap(dev, res->start, size, type);
	if (!dest) {
		dev_err(dev, "ioremap de %pa fallo\n", &res->start);
		lkpi_devm_release_region(dev, &iomem_resource, res->start, size);
		return IOMEM_ERR_PTR(-ENOMEM);
	}
	return dest;
}
EXPORT_SYMBOL(lkpi_devm_ioremap_resource);
