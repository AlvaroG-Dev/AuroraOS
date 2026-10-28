/* SPDX-License-Identifier: GPL-2.0-only */
/* Framework de componentes: sin master/bind en la capa, solo el registro. */
#ifndef _LKPI_LINUX_COMPONENT_H
#define _LKPI_LINUX_COMPONENT_H

#include <linux/device.h>

struct component_ops {
	int (*bind)(struct device *comp, struct device *master, void *master_data);
	void (*unbind)(struct device *comp, struct device *master, void *master_data);
};

static inline int component_add(struct device *dev, const struct component_ops *ops) { return 0; }
static inline void component_del(struct device *dev, const struct component_ops *ops) { }

#endif
