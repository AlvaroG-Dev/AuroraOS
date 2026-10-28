/* SPDX-License-Identifier: GPL-2.0-only */
/* Sin device tree en Aurora: todo lo OF es "no encontrado". */
#ifndef _LKPI_LINUX_OF_DEVICE_H
#define _LKPI_LINUX_OF_DEVICE_H
#include <linux/of.h>
#include <linux/device.h>
#include <linux/property.h>
#include <linux/pm.h>

static inline int of_driver_match_device(struct device *dev, const struct device_driver *drv) { return 0; }
static inline int of_device_uevent_modalias(const struct device *dev, struct kobj_uevent_env *env) { return -ENODEV; }
static inline struct fwnode_handle *of_fwnode_handle(struct device_node *np) { return NULL; }
static inline void device_set_node(struct device *dev, struct fwnode_handle *fwnode) { dev->fwnode = fwnode; }
static inline struct device *bus_find_device_by_of_node(const struct bus_type *bus, const struct device_node *np) { return NULL; }

static inline int pm_generic_runtime_suspend(struct device *dev) { return 0; }
static inline int pm_generic_runtime_resume(struct device *dev) { return 0; }
static inline int pm_generic_suspend(struct device *dev) { return 0; }
static inline int pm_generic_resume(struct device *dev) { return 0; }
static inline int pm_generic_freeze(struct device *dev) { return 0; }
static inline int pm_generic_thaw(struct device *dev) { return 0; }
static inline int pm_generic_poweroff(struct device *dev) { return 0; }
static inline int pm_generic_restore(struct device *dev) { return 0; }
#endif
