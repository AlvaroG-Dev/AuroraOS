/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_PINCTRL_CONSUMER_H
#define _LKPI_LINUX_PINCTRL_CONSUMER_H
#include <linux/err.h>
#include <linux/device.h>
struct pinctrl;
static inline struct pinctrl *devm_pinctrl_get_select(struct device *dev, const char *name) { return ERR_PTR(-ENODEV); }
#endif
