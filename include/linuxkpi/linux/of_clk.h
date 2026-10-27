/* SPDX-License-Identifier: GPL-2.0-only */
/* Relojes del device tree: no hay (ver linux/of.h). */
#ifndef _LKPI_LINUX_OF_CLK_H
#define _LKPI_LINUX_OF_CLK_H

#include <linux/of.h>

static inline unsigned int of_clk_get_parent_count(const struct device_node *np) { return 0; }

#endif
