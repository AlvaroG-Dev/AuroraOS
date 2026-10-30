/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Arbitro VGA minimo (L5): un solo dispositivo VGA de arranque (el primero de
 * clase VGA que vea el PCI) y sin recursos legados que repartir, porque el
 * driver de Linux no toca los puertos VGA reales (display-shadow).
 */
#ifndef _LKPI_LINUX_VGAARB_H
#define _LKPI_LINUX_VGAARB_H

#include <linux/pci.h>
#include <linux/types.h>

#define VGA_RSRC_NONE 0x00
#define VGA_RSRC_LEGACY_IO 0x01
#define VGA_RSRC_LEGACY_MEM 0x02
#define VGA_RSRC_LEGACY_MASK (VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM)
#define VGA_RSRC_NORMAL_IO 0x04
#define VGA_RSRC_NORMAL_MEM 0x08

struct pci_dev *lkpi_vga_default_device(void);
void lkpi_vga_set_default_device(struct pci_dev *pdev);

static inline struct pci_dev *vga_default_device(void) { return lkpi_vga_default_device(); }
static inline void vga_set_default_device(struct pci_dev *pdev) { lkpi_vga_set_default_device(pdev); }
static inline int vga_remove_vgacon(struct pci_dev *pdev) { return 0; }
static inline int vga_get(struct pci_dev *pdev, unsigned int rsrc, int interruptible) { return 0; }
static inline int vga_get_interruptible(struct pci_dev *pdev, unsigned int rsrc) { return 0; }
static inline int vga_get_uninterruptible(struct pci_dev *pdev, unsigned int rsrc) { return 0; }
static inline void vga_put(struct pci_dev *pdev, unsigned int rsrc) { }
static inline int vga_client_register(struct pci_dev *pdev,
				      unsigned int (*set_decode)(struct pci_dev *, bool))
{
	return 0;
}
static inline void vga_set_legacy_decoding(struct pci_dev *pdev, unsigned int decodes) { }


static inline void vga_client_unregister(struct pci_dev *pdev) { }
#endif
