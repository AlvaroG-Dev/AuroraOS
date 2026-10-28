/* SPDX-License-Identifier: GPL-2.0-only */
/* Cola de administracion de virtio-pci: la capa no la usa (sin SR-IOV ni migracion). */
#ifndef _LKPI_LINUX_VIRTIO_PCI_ADMIN_H
#define _LKPI_LINUX_VIRTIO_PCI_ADMIN_H

#include <linux/types.h>
#include <linux/pci.h>
#include <linux/scatterlist.h>

bool virtio_pci_admin_has_dev_parts(struct pci_dev *pdev);
int virtio_pci_admin_mode_set(struct pci_dev *pdev, u8 mode);
int virtio_pci_admin_obj_create(struct pci_dev *pdev, u16 obj_type, u8 operation_type, u32 *obj_id);
int virtio_pci_admin_obj_destroy(struct pci_dev *pdev, u16 obj_type, u32 id);
int virtio_pci_admin_dev_parts_metadata_get(struct pci_dev *pdev, u16 obj_type, u32 id,
					    u8 metadata_type, u32 *out);
int virtio_pci_admin_dev_parts_get(struct pci_dev *pdev, u16 obj_type, u32 id, u8 get_type,
				   struct scatterlist *res_sg, u32 *res_size);
int virtio_pci_admin_dev_parts_set(struct pci_dev *pdev, struct scatterlist *data_sg);

#endif
