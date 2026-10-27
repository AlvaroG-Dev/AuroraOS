/* SPDX-License-Identifier: GPL-2.0-only */
/* Memoria reservada del device tree: no hay (ver linux/of.h). */
#ifndef _LKPI_LINUX_OF_RESERVED_MEM_H
#define _LKPI_LINUX_OF_RESERVED_MEM_H

#include <linux/of.h>
#include <linux/ioport.h>

static inline int of_reserved_mem_region_to_resource(const struct device_node *np,
						     unsigned int idx, struct resource *res)
{
	return -ENODEV;
}
static inline int of_reserved_mem_region_count(const struct device_node *np) { return 0; }

#endif
