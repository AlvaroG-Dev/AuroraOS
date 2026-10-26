// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bus de plataforma basico (K4): dispositivos con recursos fijos y match
 * por id_table o por nombre. Sin device tree ni ACPI (Aurora no los ofrece
 * a la capa todavia). /sys/devices/platform/<nombre> y /sys/bus/platform.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/idr.h>
#include <linux/dma-mapping.h>
#include "base.h"
#include "../lkpi_internal.h"

struct device lkpi_platform_bus = {
	.init_name = "platform",
};
EXPORT_SYMBOL(lkpi_platform_bus);

static DEFINE_IDA(lkpi_platform_devid_ida);

static const struct platform_device_id *lkpi_platform_match_id(const struct platform_device_id *id,
							       struct platform_device *pdev)
{
	while (id->name[0]) {
		if (strcmp(pdev->name, id->name) == 0) {
			pdev->id_entry = id;
			return id;
		}
		id++;
	}
	return NULL;
}

static int lkpi_platform_match(struct device *dev, const struct device_driver *drv)
{
	struct platform_device *pdev = to_platform_device(dev);
	const struct platform_driver *pdrv = to_platform_driver(drv);

	if (pdrv->id_table)
		return lkpi_platform_match_id(pdrv->id_table, pdev) != NULL;
	return strcmp(pdev->name, drv->name) == 0;
}

static int lkpi_platform_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct platform_device *pdev = container_of_const(dev, struct platform_device, dev);

	return add_uevent_var(env, "MODALIAS=platform:%s", pdev->name);
}

static int lkpi_platform_probe(struct device *dev)
{
	struct platform_driver *drv = to_platform_driver(dev->driver);
	struct platform_device *dev_p = to_platform_device(dev);

	return drv->probe ? drv->probe(dev_p) : 0;
}

static void lkpi_platform_remove(struct device *dev)
{
	struct platform_driver *drv = to_platform_driver(dev->driver);

	if (drv->remove)
		drv->remove(to_platform_device(dev));
}

static void lkpi_platform_shutdown(struct device *dev)
{
	struct platform_driver *drv;

	if (!dev->driver)
		return;
	drv = to_platform_driver(dev->driver);
	if (drv->shutdown)
		drv->shutdown(to_platform_device(dev));
}

const struct bus_type lkpi_platform_bus_type = {
	.name = "platform",
	.match = lkpi_platform_match,
	.uevent = lkpi_platform_uevent,
	.probe = lkpi_platform_probe,
	.remove = lkpi_platform_remove,
	.shutdown = lkpi_platform_shutdown,
};
EXPORT_SYMBOL(lkpi_platform_bus_type);

/* --------------------------------------------------------- recursos */

struct resource *lkpi_platform_get_resource(struct platform_device *dev, unsigned int type,
					    unsigned int num)
{
	for (u32 i = 0; i < dev->num_resources; i++) {
		struct resource *r = &dev->resource[i];

		if (type == resource_type(r) && num-- == 0)
			return r;
	}
	return NULL;
}
EXPORT_SYMBOL(lkpi_platform_get_resource);

struct resource *lkpi_platform_get_resource_byname(struct platform_device *dev,
						   unsigned int type, const char *name)
{
	for (u32 i = 0; i < dev->num_resources; i++) {
		struct resource *r = &dev->resource[i];

		if (unlikely(!r->name))
			continue;
		if (type == resource_type(r) && !strcmp(r->name, name))
			return r;
	}
	return NULL;
}
EXPORT_SYMBOL(lkpi_platform_get_resource_byname);

int lkpi_platform_get_irq_optional(struct platform_device *dev, unsigned int num)
{
	struct resource *r = lkpi_platform_get_resource(dev, IORESOURCE_IRQ, num);

	return r ? (int)r->start : -ENXIO;
}
EXPORT_SYMBOL(lkpi_platform_get_irq_optional);

int lkpi_platform_get_irq(struct platform_device *dev, unsigned int num)
{
	int irq = lkpi_platform_get_irq_optional(dev, num);

	if (irq < 0)
		dev_err(&dev->dev, "IRQ index %u no encontrada\n", num);
	return irq;
}
EXPORT_SYMBOL(lkpi_platform_get_irq);

int lkpi_platform_irq_count(struct platform_device *dev)
{
	int n = 0;

	while (lkpi_platform_get_irq_optional(dev, n) >= 0)
		n++;
	return n;
}
EXPORT_SYMBOL(lkpi_platform_irq_count);

void __iomem *lkpi_devm_platform_get_and_ioremap_resource(struct platform_device *pdev,
							  unsigned int index,
							  struct resource **res)
{
	struct resource *r = lkpi_platform_get_resource(pdev, IORESOURCE_MEM, index);

	if (res)
		*res = r;
	return devm_ioremap_resource(&pdev->dev, r);
}
EXPORT_SYMBOL(lkpi_devm_platform_get_and_ioremap_resource);

/* --------------------------------------------------- dispositivos */

struct lkpi_platform_object {
	struct platform_device pdev;
	char name[];
};

static void lkpi_platform_device_release(struct device *dev)
{
	struct lkpi_platform_object *pa = container_of(dev, struct lkpi_platform_object, pdev.dev);

	kfree(pa->pdev.dev.platform_data);
	kfree(pa->pdev.resource);
	kfree(pa);
}

struct platform_device *lkpi_platform_device_alloc(const char *name, int id)
{
	struct lkpi_platform_object *pa;

	pa = kzalloc(sizeof(*pa) + strlen(name) + 1, GFP_KERNEL);
	if (!pa)
		return NULL;
	strcpy(pa->name, name);
	pa->pdev.name = pa->name;
	pa->pdev.id = id;
	device_initialize(&pa->pdev.dev);
	pa->pdev.dev.release = lkpi_platform_device_release;
	/* Como Linux: mascara de DMA de 32 bits por defecto. */
	pa->pdev.platform_dma_mask = DMA_BIT_MASK(32);
	pa->pdev.dev.dma_mask = &pa->pdev.platform_dma_mask;
	pa->pdev.dev.coherent_dma_mask = DMA_BIT_MASK(32);
	pa->pdev.dev.dma_parms = &pa->pdev.dma_parms;
	return &pa->pdev;
}
EXPORT_SYMBOL(lkpi_platform_device_alloc);

int lkpi_platform_device_add_resources(struct platform_device *pdev,
				       const struct resource *res, unsigned int num)
{
	struct resource *r = NULL;

	if (res) {
		r = kmemdup(res, sizeof(struct resource) * num, GFP_KERNEL);
		if (!r)
			return -ENOMEM;
	}
	kfree(pdev->resource);
	pdev->resource = r;
	pdev->num_resources = num;
	return 0;
}
EXPORT_SYMBOL(lkpi_platform_device_add_resources);

int lkpi_platform_device_add_data(struct platform_device *pdev, const void *data, size_t size)
{
	void *d = NULL;

	if (data) {
		d = kmemdup(data, size, GFP_KERNEL);
		if (!d)
			return -ENOMEM;
	}
	kfree(pdev->dev.platform_data);
	pdev->dev.platform_data = d;
	return 0;
}
EXPORT_SYMBOL(lkpi_platform_device_add_data);

int lkpi_platform_device_add(struct platform_device *pdev)
{
	int rc;

	if (!pdev->dev.parent)
		pdev->dev.parent = &lkpi_platform_bus;
	pdev->dev.bus = &lkpi_platform_bus_type;
	switch (pdev->id) {
	default:
		rc = dev_set_name(&pdev->dev, "%s.%d", pdev->name, pdev->id);
		break;
	case PLATFORM_DEVID_NONE:
		rc = dev_set_name(&pdev->dev, "%s", pdev->name);
		break;
	case PLATFORM_DEVID_AUTO:
		rc = ida_alloc(&lkpi_platform_devid_ida, GFP_KERNEL);
		if (rc < 0)
			return rc;
		pdev->id = rc;
		pdev->id_auto = true;
		rc = dev_set_name(&pdev->dev, "%s.%d.auto", pdev->name, pdev->id);
		break;
	}
	if (rc)
		goto err;
	/* Los recursos de la plataforma no se reservan aqui: lo hace el driver. */
	rc = device_add(&pdev->dev);
	if (rc)
		goto err;
	return 0;
err:
	if (pdev->id_auto) {
		ida_free(&lkpi_platform_devid_ida, pdev->id);
		pdev->id = PLATFORM_DEVID_AUTO;
		pdev->id_auto = false;
	}
	return rc;
}
EXPORT_SYMBOL(lkpi_platform_device_add);

void lkpi_platform_device_del(struct platform_device *pdev)
{
	if (IS_ERR_OR_NULL(pdev))
		return;
	device_del(&pdev->dev);
	if (pdev->id_auto) {
		ida_free(&lkpi_platform_devid_ida, pdev->id);
		pdev->id = PLATFORM_DEVID_AUTO;
		pdev->id_auto = false;
	}
}
EXPORT_SYMBOL(lkpi_platform_device_del);

void lkpi_platform_device_put(struct platform_device *pdev)
{
	if (!IS_ERR_OR_NULL(pdev))
		put_device(&pdev->dev);
}
EXPORT_SYMBOL(lkpi_platform_device_put);

/* Para platform_device estaticos (sin release propio). */
static void lkpi_platform_static_release(struct device *dev) { }

int lkpi_platform_device_register(struct platform_device *pdev)
{
	device_initialize(&pdev->dev);
	if (!pdev->dev.release)
		pdev->dev.release = lkpi_platform_static_release;
	if (!pdev->dev.dma_mask) {
		pdev->platform_dma_mask = DMA_BIT_MASK(32);
		pdev->dev.dma_mask = &pdev->platform_dma_mask;
		pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);
	}
	return lkpi_platform_device_add(pdev);
}
EXPORT_SYMBOL(lkpi_platform_device_register);

void lkpi_platform_device_unregister(struct platform_device *pdev)
{
	if (IS_ERR_OR_NULL(pdev))
		return;
	lkpi_platform_device_del(pdev);
	lkpi_platform_device_put(pdev);
}
EXPORT_SYMBOL(lkpi_platform_device_unregister);

struct platform_device *lkpi_platform_device_register_full(const struct platform_device_info *pi)
{
	struct platform_device *pdev;
	int rc;

	pdev = lkpi_platform_device_alloc(pi->name, pi->id);
	if (!pdev)
		return ERR_PTR(-ENOMEM);
	pdev->dev.parent = pi->parent;
	pdev->dev.fwnode = pi->fwnode;
	if (pi->dma_mask) {
		pdev->platform_dma_mask = pi->dma_mask;
		pdev->dev.coherent_dma_mask = pi->dma_mask;
	}
	rc = lkpi_platform_device_add_resources(pdev, pi->res, pi->num_res);
	if (!rc)
		rc = lkpi_platform_device_add_data(pdev, pi->data, pi->size_data);
	if (!rc)
		rc = lkpi_platform_device_add(pdev);
	if (rc) {
		lkpi_platform_device_put(pdev);
		return ERR_PTR(rc);
	}
	return pdev;
}
EXPORT_SYMBOL(lkpi_platform_device_register_full);

/* ---------------------------------------------------------- drivers */

int lkpi_platform_driver_register(struct platform_driver *drv, struct module *owner)
{
	drv->driver.owner = owner;
	drv->driver.bus = &lkpi_platform_bus_type;
	return driver_register(&drv->driver);
}
EXPORT_SYMBOL(lkpi_platform_driver_register);

void lkpi_platform_driver_unregister(struct platform_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL(lkpi_platform_driver_unregister);

/* ---------------------------------------------------------- arranque */

static void lkpi_platform_bus_release(struct device *dev) { }

int lkpi_platform_bus_init(void)
{
	int rc;

	memset(&lkpi_platform_bus, 0, sizeof(lkpi_platform_bus));
	lkpi_platform_bus.init_name = "platform";
	lkpi_platform_bus.release = lkpi_platform_bus_release;
	rc = device_register(&lkpi_platform_bus);
	if (rc) {
		put_device(&lkpi_platform_bus);
		return rc;
	}
	rc = bus_register(&lkpi_platform_bus_type);
	if (rc)
		device_unregister(&lkpi_platform_bus);
	return rc;
}

void lkpi_platform_bus_exit(void)
{
	bus_unregister(&lkpi_platform_bus_type);
	device_unregister(&lkpi_platform_bus);
	ida_destroy(&lkpi_platform_devid_ida);
}
