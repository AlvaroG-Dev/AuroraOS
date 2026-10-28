/* SPDX-License-Identifier: GPL-2.0-only */
/* Transporte virtio-pci "legacy" (0.9.5): BAR0 de E/S con un registro fijo. */
#ifndef _LKPI_LINUX_VIRTIO_PCI_LEGACY_H
#define _LKPI_LINUX_VIRTIO_PCI_LEGACY_H

#include <linux/mod_devicetable.h>
#include <linux/pci.h>
#include <linux/virtio_pci.h>

struct virtio_pci_legacy_device {
	struct pci_dev *pci_dev;
	u8 __iomem *isr;
	void __iomem *ioaddr;
	struct virtio_device_id id;
};

u64 vp_legacy_get_features(struct virtio_pci_legacy_device *ldev);
u64 vp_legacy_get_driver_features(struct virtio_pci_legacy_device *ldev);
void vp_legacy_set_features(struct virtio_pci_legacy_device *ldev, u32 features);
u8 vp_legacy_get_status(struct virtio_pci_legacy_device *ldev);
void vp_legacy_set_status(struct virtio_pci_legacy_device *ldev, u8 status);
u16 vp_legacy_queue_vector(struct virtio_pci_legacy_device *ldev, u16 idx, u16 vector);
u16 vp_legacy_config_vector(struct virtio_pci_legacy_device *ldev, u16 vector);
void vp_legacy_set_queue_address(struct virtio_pci_legacy_device *ldev, u16 index, u32 queue_pfn);
bool vp_legacy_get_queue_enable(struct virtio_pci_legacy_device *ldev, u16 idx);
u16 vp_legacy_get_queue_size(struct virtio_pci_legacy_device *ldev, u16 idx);
int vp_legacy_probe(struct virtio_pci_legacy_device *ldev);
void vp_legacy_remove(struct virtio_pci_legacy_device *ldev);

#endif
