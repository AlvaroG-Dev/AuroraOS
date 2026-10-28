/* SPDX-License-Identifier: GPL-2.0-only */
/* vga_switcheroo (GPU hibridas): la capa no lo implementa; los puntos de llamada no hacen nada. */
#ifndef _LKPI_LINUX_VGA_SWITCHEROO_H
#define _LKPI_LINUX_VGA_SWITCHEROO_H

#include <linux/types.h>
/* drm_edid.c / drm_file.c reciben de Linux seq_file.h y fb.h (KHZ2PICOS) por esta via transitiva */
#include <linux/seq_file.h>
#include <linux/fb.h>

struct pci_dev;
struct device;

static inline void vga_switcheroo_lock_ddc(struct pci_dev *pdev) { }
static inline void vga_switcheroo_unlock_ddc(struct pci_dev *pdev) { }
static inline int vga_switcheroo_process_delayed_switch(void) { return 0; }
static inline bool vga_switcheroo_client_probe_defer(struct pci_dev *pdev) { return false; }
static inline int vga_switcheroo_register_client(struct pci_dev *dev, const void *ops, bool driver_power_control) { return 0; }
static inline void vga_switcheroo_unregister_client(struct pci_dev *dev) { }
static inline int vga_switcheroo_register_handler(const void *handler, int id) { return 0; }

#endif
