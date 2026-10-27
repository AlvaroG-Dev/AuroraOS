/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Dominios de energia: Aurora no los tiene (L9). Como Linux sin
 * CONFIG_PM_GENERIC_DOMAINS: no hay dominio que adjuntar.
 */
#ifndef _LKPI_LINUX_PM_DOMAIN_H
#define _LKPI_LINUX_PM_DOMAIN_H

#include <linux/device.h>
#include <linux/err.h>

static inline int dev_pm_domain_attach(struct device *dev, u32 flags) { return 0; }
static inline struct device *dev_pm_domain_attach_by_id(struct device *dev, unsigned int index)
{
	return NULL;
}
static inline struct device *dev_pm_domain_attach_by_name(struct device *dev, const char *name)
{
	return NULL;
}
static inline void dev_pm_domain_detach(struct device *dev, bool power_off) { }

#endif
