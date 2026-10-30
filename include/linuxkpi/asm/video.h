/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_ASM_VIDEO_H
#define _LKPI_ASM_VIDEO_H

#include <linux/pci.h>

/* "VGA de arranque": el unico dispositivo de clase VGA de los QEMU de prueba */
static inline bool video_is_primary_device(struct device *dev)
{
	struct pci_dev *pdev;

	if (!dev_is_pci(dev))
		return false;
	pdev = to_pci_dev(dev);
	return (pdev->class >> 8) == PCI_CLASS_DISPLAY_VGA;
}

#endif
