/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_IOMMU_H
#define _LKPI_LINUX_IOMMU_H
#include <linux/types.h>
#include <linux/device.h>
/*
 * L9: con `iommu=on` (VT-d) los dispositivos PCI que usan la API DMA tienen un
 * dominio privado (kernel/iommu.c, dma/mapping.c). Aqui solo se ve como un
 * dominio opaco de tipo DMA; no hay API de dominios para los drivers.
 */
#define __IOMMU_DOMAIN_DMA_API 0x2
#define IOMMU_DOMAIN_DMA 0x3
struct iommu_domain { unsigned int type; };
extern struct iommu_domain lkpi_iommu_dummy_domain;
static inline struct iommu_domain *iommu_get_domain_for_dev(struct device *dev)
{
	return dev->iommu_group ? &lkpi_iommu_dummy_domain : NULL;
}
#endif
