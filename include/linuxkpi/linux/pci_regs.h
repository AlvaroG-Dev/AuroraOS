/* SPDX-License-Identifier: GPL-2.0-only */
#include <uapi/linux/pci_regs.h>

#ifndef PCI_LTR_VALUE_MASK
#define PCI_LTR_VALUE_MASK 0x000003ff
#define PCI_LTR_SCALE_MASK 0x00001c00
#define PCI_LTR_SCALE_SHIFT 10
#define PCI_LTR_NOSNOOP_VALUE 0x03ff0000
#define PCI_LTR_NOSNOOP_SCALE 0x1c000000
#endif
