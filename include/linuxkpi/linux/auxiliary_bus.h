/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_AUXILIARY_BUS_H
#define _LKPI_LINUX_AUXILIARY_BUS_H
/* Bus auxiliar: solo la estructura; no hay bus, auxiliary_device_init/add fallan con -ENODEV (GSC, MTL+). */
#include <linux/device.h>
struct auxiliary_device {
	struct device dev;
	const char *name;
	u32 id;
};
#define to_auxiliary_dev(__dev) container_of(__dev, struct auxiliary_device, dev)
static inline int auxiliary_device_init(struct auxiliary_device *auxdev) { return -ENODEV; }
static inline int __auxiliary_device_add(struct auxiliary_device *auxdev, const char *modname) { return -ENODEV; }
#define auxiliary_device_add(auxdev) __auxiliary_device_add(auxdev, KBUILD_MODNAME)
static inline void auxiliary_device_delete(struct auxiliary_device *auxdev) {}
static inline void auxiliary_device_uninit(struct auxiliary_device *auxdev) {}
#endif
