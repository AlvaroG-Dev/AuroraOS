// SPDX-License-Identifier: GPL-2.0-only
/*
 * Reserva de regiones de E/S y de memoria (request_region,
 * request_mem_region). Lista plana de regiones ocupadas por padre: detecta
 * que dos drivers de Linux pidan el mismo rango. No conoce las regiones que
 * usan los drivers propios de Aurora (no las registran en ningun sitio).
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/ioport.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

struct resource lkpi_iomem_resource = {
	.name = "PCI mem", .start = 0, .end = -1, .flags = IORESOURCE_MEM,
};
EXPORT_SYMBOL(lkpi_iomem_resource);
struct resource lkpi_ioport_resource = {
	.name = "PCI IO", .start = 0, .end = 0xffff, .flags = IORESOURCE_IO,
};
EXPORT_SYMBOL(lkpi_ioport_resource);

static DEFINE_SPINLOCK(lkpi_resource_lock);

struct resource *lkpi_request_region(struct resource *parent, resource_size_t start,
				     resource_size_t n, const char *name, int flags)
{
	struct resource *res, *r;
	resource_size_t end = start + n - 1;

	if (!n || end < start || start < parent->start || end > parent->end)
		return NULL;
	res = kzalloc(sizeof(*res), GFP_KERNEL);
	if (!res)
		return NULL;
	res->name = name;
	res->start = start;
	res->end = end;
	res->flags = (parent->flags & IORESOURCE_TYPE_BITS) | IORESOURCE_BUSY | flags;

	spin_lock(&lkpi_resource_lock);
	for (r = parent->child; r; r = r->sibling) {
		if (r->start > end || r->end < start)
			continue;
		/* Las muxed se comparten entre quien las pida como muxed. */
		if ((r->flags & IORESOURCE_MUXED) && (flags & IORESOURCE_MUXED))
			continue;
		spin_unlock(&lkpi_resource_lock);
		pr_debug("linuxkpi: region [%llx-%llx] de %s ocupada por %s\n",
			 (unsigned long long)start, (unsigned long long)end, name ? name : "?",
			 r->name ? r->name : "?");
		kfree(res);
		return NULL;
	}
	res->parent = parent;
	res->sibling = parent->child;
	parent->child = res;
	spin_unlock(&lkpi_resource_lock);
	return res;
}
EXPORT_SYMBOL(lkpi_request_region);

void lkpi_release_region(struct resource *parent, resource_size_t start, resource_size_t n)
{
	struct resource **p, *r = NULL;
	resource_size_t end = start + n - 1;

	spin_lock(&lkpi_resource_lock);
	for (p = &parent->child; *p; p = &(*p)->sibling) {
		if ((*p)->start == start && (*p)->end == end) {
			r = *p;
			*p = r->sibling;
			break;
		}
	}
	spin_unlock(&lkpi_resource_lock);
	if (r)
		kfree(r);
	else
		pr_warn("linuxkpi: liberar region [%llx-%llx] no reservada\n",
			(unsigned long long)start, (unsigned long long)end);
}
EXPORT_SYMBOL(lkpi_release_region);

struct lkpi_region_devres {
	struct resource *parent;
	resource_size_t start;
	resource_size_t n;
};

static void lkpi_devm_region_release(struct device *dev, void *res)
{
	struct lkpi_region_devres *d = res;

	lkpi_release_region(d->parent, d->start, d->n);
}

static int lkpi_devm_region_match(struct device *dev, void *res, void *match_data)
{
	struct lkpi_region_devres *a = res, *b = match_data;

	return a->parent == b->parent && a->start == b->start && a->n == b->n;
}

struct resource *lkpi_devm_request_region(struct device *dev, struct resource *parent,
					  resource_size_t start, resource_size_t n,
					  const char *name)
{
	struct lkpi_region_devres *d;
	struct resource *res;

	d = devres_alloc(lkpi_devm_region_release, sizeof(*d), GFP_KERNEL);
	if (!d)
		return NULL;
	res = lkpi_request_region(parent, start, n, name, 0);
	if (!res) {
		devres_free(d);
		return NULL;
	}
	d->parent = parent;
	d->start = start;
	d->n = n;
	devres_add(dev, d);
	return res;
}
EXPORT_SYMBOL(lkpi_devm_request_region);

void lkpi_devm_release_region(struct device *dev, struct resource *parent,
			      resource_size_t start, resource_size_t n)
{
	struct lkpi_region_devres match = { parent, start, n };

	WARN_ON(devres_release(dev, lkpi_devm_region_release, lkpi_devm_region_match, &match));
}
EXPORT_SYMBOL(lkpi_devm_release_region);
