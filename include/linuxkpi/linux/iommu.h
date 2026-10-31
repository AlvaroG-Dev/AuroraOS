/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_IOMMU_H
#define _LKPI_LINUX_IOMMU_H
#include <linux/types.h>
struct device;
/* Aurora no usa IOMMU para los drivers: DMA directo, sin dominio. */
#define __IOMMU_DOMAIN_DMA_API 0x2
struct iommu_domain { unsigned int type; };
static inline struct iommu_domain *iommu_get_domain_for_dev(struct device *dev) { return NULL; }
#endif
