/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Bus de plataforma basico (K4): dispositivos con recursos fijos (MMIO,
 * IRQ) y match por nombre o por id_table. Sin device tree ni ACPI: los
 * dispositivos los crea quien los conoce (p. ej. el framebuffer de arranque
 * para simpledrm en L5). /sys/devices/platform/<nombre>.
 */
#ifndef _LKPI_LINUX_PLATFORM_DEVICE_H
#define _LKPI_LINUX_PLATFORM_DEVICE_H

#include <linux/device.h>
#include <linux/mod_devicetable.h>

#define PLATFORM_DEVID_NONE (-1)
#define PLATFORM_DEVID_AUTO (-2)

struct mfd_cell;
struct property_entry;
struct pdev_archdata { };

struct platform_device {
	const char *name;
	int id;
	bool id_auto;
	struct device dev;
	u64 platform_dma_mask;
	struct device_dma_parameters dma_parms;
	u32 num_resources;
	struct resource *resource;
	const struct platform_device_id *id_entry;
	struct mfd_cell *mfd_cell;
	struct pdev_archdata archdata;
};

#define platform_get_device_id(pdev) ((pdev)->id_entry)
#define dev_is_platform(dev) ((dev)->bus == &platform_bus_type)
#define to_platform_device(x) container_of((x), struct platform_device, dev)

struct platform_device_info {
	struct device *parent;
	struct fwnode_handle *fwnode;
	bool of_node_reused;
	const char *name;
	int id;
	const struct resource *res;
	unsigned int num_res;
	const void *data;
	size_t size_data;
	u64 dma_mask;
	const struct property_entry *properties;
};

struct platform_driver {
	int (*probe)(struct platform_device *);
	void (*remove)(struct platform_device *);
	void (*shutdown)(struct platform_device *);
	int (*suspend)(struct platform_device *, pm_message_t state);
	int (*resume)(struct platform_device *);
	struct device_driver driver;
	const struct platform_device_id *id_table;
	bool prevent_deferred_probe;
	bool driver_managed_dma;
};

#define to_platform_driver(drv) container_of_const((drv), struct platform_driver, driver)

extern const struct bus_type lkpi_platform_bus_type;
extern struct device lkpi_platform_bus;
#define platform_bus_type lkpi_platform_bus_type
#define platform_bus lkpi_platform_bus

int lkpi_platform_device_register(struct platform_device *pdev);
void lkpi_platform_device_unregister(struct platform_device *pdev);
struct platform_device *lkpi_platform_device_alloc(const char *name, int id);
int lkpi_platform_device_add_resources(struct platform_device *pdev,
				       const struct resource *res, unsigned int num);
int lkpi_platform_device_add_data(struct platform_device *pdev, const void *data, size_t size);
int lkpi_platform_device_add(struct platform_device *pdev);
void lkpi_platform_device_del(struct platform_device *pdev);
void lkpi_platform_device_put(struct platform_device *pdev);
struct platform_device *lkpi_platform_device_register_full(const struct platform_device_info *i);
struct resource *lkpi_platform_get_resource(struct platform_device *dev, unsigned int type,
					    unsigned int num);
struct resource *lkpi_platform_get_resource_byname(struct platform_device *dev,
						   unsigned int type, const char *name);
int lkpi_platform_get_irq_optional(struct platform_device *dev, unsigned int num);
int lkpi_platform_get_irq(struct platform_device *dev, unsigned int num);
int lkpi_platform_irq_count(struct platform_device *dev);
void __iomem *lkpi_devm_platform_get_and_ioremap_resource(struct platform_device *pdev,
							  unsigned int index,
							  struct resource **res);
int lkpi_platform_driver_register(struct platform_driver *drv, struct module *owner);
void lkpi_platform_driver_unregister(struct platform_driver *drv);

#define platform_device_register(p) lkpi_platform_device_register(p)
#define platform_device_unregister(p) lkpi_platform_device_unregister(p)
#define platform_device_alloc(n, i) lkpi_platform_device_alloc(n, i)
#define platform_device_add_resources(p, r, n) lkpi_platform_device_add_resources(p, r, n)
#define platform_device_add_data(p, d, s) lkpi_platform_device_add_data(p, d, s)
#define platform_device_add(p) lkpi_platform_device_add(p)
#define platform_device_del(p) lkpi_platform_device_del(p)
#define platform_device_put(p) lkpi_platform_device_put(p)
#define platform_device_register_full(i) lkpi_platform_device_register_full(i)
#define platform_get_resource(d, t, n) lkpi_platform_get_resource(d, t, n)
#define platform_get_resource_byname(d, t, n) lkpi_platform_get_resource_byname(d, t, n)
#define platform_get_irq_optional(d, n) lkpi_platform_get_irq_optional(d, n)
#define platform_get_irq(d, n) lkpi_platform_get_irq(d, n)
#define platform_irq_count(d) lkpi_platform_irq_count(d)
#define devm_platform_get_and_ioremap_resource(p, i, r) \
	lkpi_devm_platform_get_and_ioremap_resource(p, i, r)
#define platform_driver_register(drv) lkpi_platform_driver_register(drv, THIS_MODULE)
#define __platform_driver_register(drv, o) lkpi_platform_driver_register(drv, o)
#define platform_driver_unregister(drv) lkpi_platform_driver_unregister(drv)

static inline void __iomem *devm_platform_ioremap_resource(struct platform_device *pdev,
							   unsigned int index)
{
	return devm_platform_get_and_ioremap_resource(pdev, index, NULL);
}
static inline struct platform_device *
platform_device_register_resndata(struct device *parent, const char *name, int id,
				  const struct resource *res, unsigned int num,
				  const void *data, size_t size)
{
	struct platform_device_info pdevinfo = {
		.parent = parent, .name = name, .id = id, .res = res, .num_res = num,
		.data = data, .size_data = size, .dma_mask = 0,
	};
	return platform_device_register_full(&pdevinfo);
}
static inline struct platform_device *
platform_device_register_simple(const char *name, int id, const struct resource *res,
				unsigned int num)
{
	return platform_device_register_resndata(NULL, name, id, res, num, NULL, 0);
}
static inline struct platform_device *
platform_device_register_data(struct device *parent, const char *name, int id,
			      const void *data, size_t size)
{
	return platform_device_register_resndata(parent, name, id, NULL, 0, data, size);
}
static inline void *platform_get_drvdata(const struct platform_device *pdev)
{
	return dev_get_drvdata(&pdev->dev);
}
static inline void platform_set_drvdata(struct platform_device *pdev, void *data)
{
	dev_set_drvdata(&pdev->dev, data);
}

#define module_platform_driver(__platform_driver) \
	module_driver(__platform_driver, platform_driver_register, platform_driver_unregister)
#define builtin_platform_driver(__platform_driver) \
	builtin_driver(__platform_driver, platform_driver_register)

#endif
