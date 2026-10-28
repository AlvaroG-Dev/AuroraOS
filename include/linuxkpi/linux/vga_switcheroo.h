/* SPDX-License-Identifier: GPL-2.0-only */
/* vga_switcheroo (GPU hibridas): la capa no conmuta GPU; los puntos de llamada no hacen nada. */
#ifndef _LKPI_LINUX_VGA_SWITCHEROO_H
#define _LKPI_LINUX_VGA_SWITCHEROO_H

#include <linux/types.h>
/* drm_edid.c / drm_file.c reciben de Linux seq_file.h y fb.h (KHZ2PICOS) por esta via transitiva */
#include <linux/seq_file.h>
#include <linux/fb.h>

struct pci_dev;
struct device;

enum vga_switcheroo_handler_flags_t {
	VGA_SWITCHEROO_CAN_SWITCH_DDC = (1 << 0),
	VGA_SWITCHEROO_NEEDS_EDP_CONFIG = (1 << 1),
};
enum vga_switcheroo_state { VGA_SWITCHEROO_OFF, VGA_SWITCHEROO_ON, VGA_SWITCHEROO_NOT_FOUND };
struct vga_switcheroo_client_ops {
	void (*set_gpu_state)(struct pci_dev *dev, enum vga_switcheroo_state);
	void (*reprobe)(struct pci_dev *dev);
	bool (*can_switch)(struct pci_dev *dev);
	void (*gpu_bound)(struct pci_dev *dev, int id);
};

static inline void vga_switcheroo_lock_ddc(struct pci_dev *pdev) { }
static inline void vga_switcheroo_unlock_ddc(struct pci_dev *pdev) { }
static inline int vga_switcheroo_process_delayed_switch(void) { return 0; }
static inline bool vga_switcheroo_client_probe_defer(struct pci_dev *pdev) { return false; }
static inline int vga_switcheroo_register_client(struct pci_dev *dev, const struct vga_switcheroo_client_ops *ops,
						 bool driver_power_control)
{
	return 0;
}
static inline void vga_switcheroo_unregister_client(struct pci_dev *dev) { }
static inline int vga_switcheroo_register_handler(const void *handler, int id) { return 0; }
static inline enum vga_switcheroo_handler_flags_t vga_switcheroo_handler_flags(void) { return 0; }

#endif
