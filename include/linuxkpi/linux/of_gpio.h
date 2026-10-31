/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_OF_GPIO_H
#define _LKPI_OF_GPIO_H
#include <linux/types.h>
#include <linux/errno.h>
struct device_node;
/* Sin arbol de dispositivos (x86): no hay GPIO por DT. */
static inline int of_get_named_gpio(const struct device_node *np, const char *propname, int index)
{
	return -ENOENT;
}
#endif
