/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * PCI (K5) sobre el bus PCI de Aurora (kernel/pci.c).
 *
 * pci_register_driver() registra el driver en el pci_bus de Aurora: Aurora
 * hace el probe (tambien en hot-plug) y avisa del remove; la capa crea el
 * struct pci_dev, comprueba la id_table de Linux y llama al driver. Un
 * dispositivo que ya reclamo un driver de Aurora (.aur, .kur) no se ofrece.
 *
 * Espacio de configuracion por los puertos 0xCF8/0xCFC de Aurora: solo los
 * 256 bytes estandar (sin ECAM; las capabilities extendidas no se ven).
 * Interrupciones: INTx (IOAPIC), MSI de un vector y MSI-X, programados por
 * la capa (destino por CPU). Numero de IRQ = el de Aurora.
 */
#ifndef _LKPI_LINUX_PCI_H
#define _LKPI_LINUX_PCI_H

#include <linux/types.h>
#include <linux/mod_devicetable.h>
#include <linux/device.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/pci_regs.h>
#include <linux/pci_ids.h>
#include <linux/dma-mapping.h>
#include <linux/pm.h>

#define PCI_DEVFN(slot, func) ((((slot) & 0x1f) << 3) | ((func) & 0x07))
#define PCI_SLOT(devfn) (((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn) ((devfn) & 0x07)

#define PCI_DEVICE(vend, dev) \
	.vendor = (vend), .device = (dev), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
#define PCI_DEVICE_SUB(vend, dev, subvend, subdev) \
	.vendor = (vend), .device = (dev), .subvendor = (subvend), .subdevice = (subdev)
#define PCI_DEVICE_CLASS(dev_class, dev_class_mask) \
	.class = (dev_class), .class_mask = (dev_class_mask), .vendor = PCI_ANY_ID, \
	.device = PCI_ANY_ID, .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
#define PCI_VDEVICE(vend, dev) \
	.vendor = PCI_VENDOR_ID_##vend, .device = (dev), .subvendor = PCI_ANY_ID, \
	.subdevice = PCI_ANY_ID, 0, 0
#define PCI_DEVICE_DATA(vend, dev, data) \
	.vendor = PCI_VENDOR_ID_##vend, .device = PCI_DEVICE_ID_##vend##_##dev, \
	.subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, 0, 0, \
	.driver_data = (kernel_ulong_t)(data)

enum {
	PCI_STD_RESOURCES,
	PCI_STD_RESOURCE_END = PCI_STD_RESOURCES + PCI_STD_NUM_BARS - 1,
	PCI_ROM_RESOURCE,
	PCI_BRIDGE_RESOURCES,
	PCI_BRIDGE_RESOURCE_END = PCI_BRIDGE_RESOURCES + 3,
	PCI_NUM_RESOURCES,
	DEVICE_COUNT_RESOURCE = PCI_NUM_RESOURCES,
};

typedef int __bitwise pci_power_t;
#define PCI_D0 ((pci_power_t)0)
#define PCI_D1 ((pci_power_t)1)
#define PCI_D2 ((pci_power_t)2)
#define PCI_D3hot ((pci_power_t)3)
#define PCI_D3cold ((pci_power_t)4)
#define PCI_UNKNOWN ((pci_power_t)5)
#define PCI_POWER_ERROR ((pci_power_t)-1)

typedef unsigned int __bitwise pci_channel_state_t;
enum pci_channel_state {
	pci_channel_io_normal = 1,
	pci_channel_io_frozen = 2,
	pci_channel_io_perm_failure = 3,
};
typedef unsigned int __bitwise pci_ers_result_t;
#define PCI_ERS_RESULT_NONE ((pci_ers_result_t)1)
#define PCI_ERS_RESULT_CAN_RECOVER ((pci_ers_result_t)2)
#define PCI_ERS_RESULT_NEED_RESET ((pci_ers_result_t)3)
#define PCI_ERS_RESULT_DISCONNECT ((pci_ers_result_t)4)
#define PCI_ERS_RESULT_RECOVERED ((pci_ers_result_t)5)

/* Respuestas de los accesos de configuracion */
/* Lectura de configuracion de un dispositivo que no responde: todo unos. */
#define PCI_ERROR_RESPONSE (~0ULL)
#define PCI_SET_ERROR_RESPONSE(val) (*(val) = ((typeof(*(val)))PCI_ERROR_RESPONSE))
#define PCI_POSSIBLE_ERROR(val) ((val) == ((typeof(val))PCI_ERROR_RESPONSE))

#define PCIBIOS_SUCCESSFUL 0x00
#define PCIBIOS_FUNC_NOT_SUPPORTED 0x81
#define PCIBIOS_BAD_VENDOR_ID 0x83
#define PCIBIOS_DEVICE_NOT_FOUND 0x86
#define PCIBIOS_BAD_REGISTER_NUMBER 0x87
#define PCIBIOS_SET_FAILED 0x88
#define PCIBIOS_BUFFER_TOO_SMALL 0x89
static inline int pcibios_err_to_errno(int err)
{
	if (err <= PCIBIOS_SUCCESSFUL)
		return err;
	switch (err) {
	case PCIBIOS_FUNC_NOT_SUPPORTED: return -ENOENT;
	case PCIBIOS_BAD_VENDOR_ID: return -ENOTTY;
	case PCIBIOS_DEVICE_NOT_FOUND: return -ENODEV;
	case PCIBIOS_BAD_REGISTER_NUMBER: return -EFAULT;
	case PCIBIOS_SET_FAILED: return -EIO;
	case PCIBIOS_BUFFER_TOO_SMALL: return -ENOSPC;
	}
	return -ERANGE;
}

struct pci_dev;
struct pci_driver;

struct pci_bus {
	struct list_head node;
	struct pci_bus *parent;
	struct pci_dev *self;		/* puente; NULL en el bus raiz */
	unsigned char number;
	unsigned char primary;
	int domain_nr;
};

struct lkpi_pci_irqs;			/* vectores de la capa (pci/msi.c) */

struct pci_dev {
	struct list_head bus_list;
	struct pci_bus *bus;
	struct pci_bus *subordinate;
	void *sysdata;
	unsigned int devfn;
	unsigned short vendor;
	unsigned short device;
	unsigned short subsystem_vendor;
	unsigned short subsystem_device;
	unsigned int class;		/* clase << 16 | subclase << 8 | prog-if */
	u8 revision;
	u8 hdr_type;
	u8 pin;
	u8 pcie_cap;
	u8 msi_cap;
	u8 msix_cap;
	u8 pm_cap;
	u16 pcie_flags_reg;
	struct pci_driver *driver;
	u64 dma_mask;
	struct device_dma_parameters dma_parms;
	pci_power_t current_state;
	pci_channel_state_t error_state;
	int cfg_size;
	unsigned int irq;
	struct resource resource[DEVICE_COUNT_RESOURCE];
	unsigned int transparent:1;
	unsigned int multifunction:1;
	unsigned int is_busmaster:1;
	unsigned int no_msi:1;
	unsigned int msi_enabled:1;
	unsigned int msix_enabled:1;
	unsigned int is_managed:1;
	unsigned int is_virtfn:1;
	unsigned int is_physfn:1;
	unsigned int is_hotplug_bridge:1;
	unsigned int broken_intx_masking:1;
	unsigned int untrusted:1;
	atomic_t enable_cnt;
	u32 saved_config_space[16];
	bool state_saved;
	struct device dev;
	/* capa */
	void *lkpi_adev;		/* struct aurora_device del bus de Aurora */
	u8 lkpi_bus, lkpi_slot, lkpi_func;
	bool lkpi_bars_sized;
	unsigned long lkpi_regions;	/* BARs pedidos con pci_request_region */
	struct lkpi_pci_irqs *lkpi_irqs;
	struct list_head lkpi_node;	/* cache de pci_dev de la capa */
};

#define to_pci_dev(n) container_of(n, struct pci_dev, dev)
#define for_each_pci_dev(d) while ((d = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, d)) != NULL)

struct pci_error_handlers {
	pci_ers_result_t (*error_detected)(struct pci_dev *dev, pci_channel_state_t error);
	pci_ers_result_t (*mmio_enabled)(struct pci_dev *dev);
	pci_ers_result_t (*slot_reset)(struct pci_dev *dev);
	void (*reset_prepare)(struct pci_dev *dev);
	void (*reset_done)(struct pci_dev *dev);
	void (*resume)(struct pci_dev *dev);
	void (*cor_error_detected)(struct pci_dev *dev);
};

struct pci_dynids {
	spinlock_t lock;
	struct list_head list;
};

struct pci_driver {
	const char *name;
	const struct pci_device_id *id_table;
	int (*probe)(struct pci_dev *dev, const struct pci_device_id *id);
	void (*remove)(struct pci_dev *dev);
	int (*suspend)(struct pci_dev *dev, pm_message_t state);
	int (*resume)(struct pci_dev *dev);
	void (*shutdown)(struct pci_dev *dev);
	int (*sriov_configure)(struct pci_dev *dev, int num_vfs);
	int (*sriov_set_msix_vec_count)(struct pci_dev *vf, int msix_vec_count);
	u32 (*sriov_get_vf_total_msix)(struct pci_dev *pf);
	const struct pci_error_handlers *err_handler;
	const struct attribute_group **groups;
	const struct attribute_group **dev_groups;
	struct device_driver driver;
	struct pci_dynids dynids;
	bool driver_managed_dma;
	/* capa: el driver equivalente en el pci_bus de Aurora (struct aurora_driver) */
	void *lkpi_adrv;
};

#define to_pci_driver(__drv) \
	((__drv) ? container_of_const(__drv, struct pci_driver, driver) : NULL)

extern const struct bus_type lkpi_pci_bus_type;
#define pci_bus_type lkpi_pci_bus_type
#define dev_is_pci(d) ((d)->bus == &pci_bus_type)
#define dev_is_pf(d) (dev_is_pci(d) ? to_pci_dev(d)->is_physfn : false)

static inline const char *pci_name(const struct pci_dev *pdev) { return dev_name(&pdev->dev); }
static inline int pci_domain_nr(struct pci_bus *bus) { return 0; }
static inline bool pci_is_root_bus(struct pci_bus *pbus) { return !pbus->parent; }
static inline u16 pci_dev_id(struct pci_dev *dev)
{
	return (u16)(((u16)dev->bus->number << 8) | (dev->devfn & 0xff));
}
static inline void *pci_get_drvdata(struct pci_dev *pdev) { return dev_get_drvdata(&pdev->dev); }
static inline void pci_set_drvdata(struct pci_dev *pdev, void *data)
{
	dev_set_drvdata(&pdev->dev, data);
}
static inline bool pci_is_enabled(struct pci_dev *pdev) { return atomic_read(&pdev->enable_cnt) > 0; }
static inline bool pci_is_managed(struct pci_dev *pdev) { return pdev->is_managed; }
static inline bool pci_is_pcie(struct pci_dev *dev) { return dev->pcie_cap != 0; }
static inline int pci_pcie_type(const struct pci_dev *dev)
{
	return (dev->pcie_flags_reg & PCI_EXP_FLAGS_TYPE) >> 4;
}
static inline bool pci_is_bridge(struct pci_dev *dev)
{
	return dev->hdr_type == PCI_HEADER_TYPE_BRIDGE || dev->hdr_type == PCI_HEADER_TYPE_CARDBUS;
}
static inline bool pci_is_vga(struct pci_dev *pdev)
{
	return (pdev->class >> 8) == PCI_CLASS_DISPLAY_VGA ||
	       (pdev->class >> 8) == PCI_CLASS_NOT_DEFINED_VGA;
}
static inline bool pci_is_display(struct pci_dev *pdev)
{
	return (pdev->class >> 16) == PCI_BASE_CLASS_DISPLAY;
}
static inline struct pci_dev *pci_upstream_bridge(struct pci_dev *dev) { return NULL; }
static inline struct pci_dev *pci_physfn(struct pci_dev *dev) { return dev; }
static inline bool pci_dev_is_disconnected(const struct pci_dev *dev)
{
	return dev->error_state == pci_channel_io_perm_failure;
}
static inline int pci_channel_offline(struct pci_dev *pdev)
{
	return pdev->error_state != pci_channel_io_normal;
}

/* Recursos (BARs) */
#define pci_resource_n(dev, bar) (&(dev)->resource[(bar)])
#define pci_resource_start(dev, bar) (pci_resource_n(dev, bar)->start)
#define pci_resource_end(dev, bar) (pci_resource_n(dev, bar)->end)
#define pci_resource_flags(dev, bar) (pci_resource_n(dev, bar)->flags)
#define pci_resource_len(dev, bar) \
	(pci_resource_end((dev), (bar)) ? resource_size(pci_resource_n((dev), (bar))) : 0)
#define pci_dev_for_each_resource(dev, res) \
	for (unsigned int __i = 0; (res) = &(dev)->resource[__i], __i < PCI_NUM_RESOURCES; __i++)

/* Espacio de configuracion */
int lkpi_pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val);
int lkpi_pci_read_config_word(const struct pci_dev *dev, int where, u16 *val);
int lkpi_pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val);
int lkpi_pci_write_config_byte(const struct pci_dev *dev, int where, u8 val);
int lkpi_pci_write_config_word(const struct pci_dev *dev, int where, u16 val);
int lkpi_pci_write_config_dword(const struct pci_dev *dev, int where, u32 val);
int lkpi_pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val);
int lkpi_pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val);
int lkpi_pcie_capability_write_word(struct pci_dev *dev, int pos, u16 val);
int lkpi_pcie_capability_write_dword(struct pci_dev *dev, int pos, u32 val);
int lkpi_pcie_capability_clear_and_set_word(struct pci_dev *dev, int pos, u16 clear, u16 set);
int lkpi_pcie_capability_clear_and_set_dword(struct pci_dev *dev, int pos, u32 clear, u32 set);
u8 lkpi_pci_find_capability(struct pci_dev *dev, int cap);
u8 lkpi_pci_find_next_capability(struct pci_dev *dev, u8 pos, int cap);
u16 lkpi_pci_find_ext_capability(struct pci_dev *dev, int cap);
#define pci_read_config_byte(d, w, v) lkpi_pci_read_config_byte(d, w, v)
#define pci_read_config_word(d, w, v) lkpi_pci_read_config_word(d, w, v)
#define pci_read_config_dword(d, w, v) lkpi_pci_read_config_dword(d, w, v)
#define pci_write_config_byte(d, w, v) lkpi_pci_write_config_byte(d, w, v)
#define pci_write_config_word(d, w, v) lkpi_pci_write_config_word(d, w, v)
#define pci_write_config_dword(d, w, v) lkpi_pci_write_config_dword(d, w, v)
#define pcie_capability_read_word(d, p, v) lkpi_pcie_capability_read_word(d, p, v)
#define pcie_capability_read_dword(d, p, v) lkpi_pcie_capability_read_dword(d, p, v)
#define pcie_capability_write_word(d, p, v) lkpi_pcie_capability_write_word(d, p, v)
#define pcie_capability_write_dword(d, p, v) lkpi_pcie_capability_write_dword(d, p, v)
#define pcie_capability_clear_and_set_word(d, p, c, s) \
	lkpi_pcie_capability_clear_and_set_word(d, p, c, s)
#define pcie_capability_clear_and_set_dword(d, p, c, s) \
	lkpi_pcie_capability_clear_and_set_dword(d, p, c, s)
#define pcie_capability_set_word(d, p, s) pcie_capability_clear_and_set_word(d, p, 0, s)
#define pcie_capability_clear_word(d, p, c) pcie_capability_clear_and_set_word(d, p, c, 0)
#define pcie_capability_set_dword(d, p, s) pcie_capability_clear_and_set_dword(d, p, 0, s)
#define pcie_capability_clear_dword(d, p, c) pcie_capability_clear_and_set_dword(d, p, c, 0)
#define pci_find_capability(d, c) lkpi_pci_find_capability(d, c)
#define pci_find_next_capability(d, p, c) lkpi_pci_find_next_capability(d, p, c)
#define pci_find_ext_capability(d, c) lkpi_pci_find_ext_capability(d, c)

/* Activacion, regiones, iomap */
int lkpi_pci_enable_device(struct pci_dev *dev);
int lkpi_pci_enable_device_mem(struct pci_dev *dev);
int lkpi_pcim_enable_device(struct pci_dev *dev);
void lkpi_pci_disable_device(struct pci_dev *dev);
void lkpi_pci_set_master(struct pci_dev *dev);
void lkpi_pci_clear_master(struct pci_dev *dev);
void lkpi_pci_intx(struct pci_dev *dev, int enable);
int lkpi_pci_select_bars(struct pci_dev *dev, unsigned long flags);
int lkpi_pci_request_region(struct pci_dev *dev, int bar, const char *name);
void lkpi_pci_release_region(struct pci_dev *dev, int bar);
int lkpi_pci_request_selected_regions(struct pci_dev *dev, int bars, const char *name);
void lkpi_pci_release_selected_regions(struct pci_dev *dev, int bars);
int lkpi_pcim_request_all_regions(struct pci_dev *dev, const char *name);
void __iomem *lkpi_pci_iomap_range(struct pci_dev *dev, int bar, unsigned long offset,
				   unsigned long maxlen, int type);
void lkpi_pci_iounmap(struct pci_dev *dev, void __iomem *addr);
void __iomem *lkpi_pcim_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen);
void __iomem *lkpi_pcim_iomap_region(struct pci_dev *pdev, int bar, const char *name);
void lkpi_pcim_iounmap(struct pci_dev *pdev, void __iomem *addr);
#define pci_enable_device(d) lkpi_pci_enable_device(d)
#define pci_enable_device_mem(d) lkpi_pci_enable_device_mem(d)
#define pcim_enable_device(d) lkpi_pcim_enable_device(d)
#define pci_disable_device(d) lkpi_pci_disable_device(d)
#define pci_set_master(d) lkpi_pci_set_master(d)
#define pci_clear_master(d) lkpi_pci_clear_master(d)
#define pci_intx(d, e) lkpi_pci_intx(d, e)
#define pci_select_bars(d, f) lkpi_pci_select_bars(d, f)
#define pci_request_region(d, b, n) lkpi_pci_request_region(d, b, n)
#define pci_release_region(d, b) lkpi_pci_release_region(d, b)
#define pci_request_selected_regions(d, b, n) lkpi_pci_request_selected_regions(d, b, n)
#define pci_request_selected_regions_exclusive(d, b, n) lkpi_pci_request_selected_regions(d, b, n)
#define pci_release_selected_regions(d, b) lkpi_pci_release_selected_regions(d, b)
#define pci_request_regions(d, n) lkpi_pci_request_selected_regions(d, (1 << PCI_STD_NUM_BARS) - 1, n)
#define pci_request_regions_exclusive(d, n) pci_request_regions(d, n)
#define pci_release_regions(d) lkpi_pci_release_selected_regions(d, (1 << PCI_STD_NUM_BARS) - 1)
#define pcim_request_all_regions(d, n) lkpi_pcim_request_all_regions(d, n)
#define pci_iomap_range(d, b, o, m) lkpi_pci_iomap_range(d, b, o, m, 0)
#define pci_iomap_wc_range(d, b, o, m) lkpi_pci_iomap_range(d, b, o, m, 1)
#define pci_iomap(d, b, m) lkpi_pci_iomap_range(d, b, 0, m, 0)
#define pci_iomap_wc(d, b, m) lkpi_pci_iomap_range(d, b, 0, m, 1)
#define pci_iounmap(d, a) lkpi_pci_iounmap(d, a)
#define pcim_iomap(d, b, m) lkpi_pcim_iomap(d, b, m)
#define pcim_iomap_region(d, b, n) lkpi_pcim_iomap_region(d, b, n)
#define pcim_iounmap(d, a) lkpi_pcim_iounmap(d, a)

/* Estado, energia y reset */
int lkpi_pci_save_state(struct pci_dev *dev);
void lkpi_pci_restore_state(struct pci_dev *dev);
int lkpi_pci_set_power_state(struct pci_dev *dev, pci_power_t state);
int lkpi_pcie_flr(struct pci_dev *dev);
bool lkpi_pcie_has_flr(struct pci_dev *dev);
int lkpi_pci_reset_function(struct pci_dev *dev);
int lkpi_pci_reset_function_locked(struct pci_dev *dev);
bool lkpi_pci_wait_for_pending_transaction(struct pci_dev *dev);
bool lkpi_pci_device_is_present(struct pci_dev *pdev);
#define pci_save_state(d) lkpi_pci_save_state(d)
#define pci_restore_state(d) lkpi_pci_restore_state(d)
#define pci_set_power_state(d, s) lkpi_pci_set_power_state(d, s)
#define pcie_flr(d) lkpi_pcie_flr(d)
#define pcie_has_flr(d) lkpi_pcie_has_flr(d)
#define pci_reset_function(d) lkpi_pci_reset_function(d)
#define pci_reset_function_locked(d) lkpi_pci_reset_function_locked(d)
#define pci_wait_for_pending_transaction(d) lkpi_pci_wait_for_pending_transaction(d)
#define pci_device_is_present(d) lkpi_pci_device_is_present(d)
static inline int pci_enable_wake(struct pci_dev *dev, pci_power_t state, bool enable) { return 0; }
static inline int pci_wake_from_d3(struct pci_dev *dev, bool enable) { return 0; }
static inline void pci_d3cold_disable(struct pci_dev *dev) { }
static inline void pci_d3cold_enable(struct pci_dev *dev) { }
static inline void pci_ignore_hotplug(struct pci_dev *dev) { }
static inline int pci_enable_ptm(struct pci_dev *dev, u8 *granularity) { return -EINVAL; }

/* SR-IOV: no hay (sin ECAM no se ve la capability). */
static inline int pci_enable_sriov(struct pci_dev *dev, int nr_virtfn) { return -ENODEV; }
static inline void pci_disable_sriov(struct pci_dev *dev) { }
static inline int pci_num_vf(struct pci_dev *dev) { return 0; }
static inline int pci_vfs_assigned(struct pci_dev *dev) { return 0; }
static inline int pci_sriov_get_totalvfs(struct pci_dev *dev) { return 0; }
static inline int pci_iov_vf_id(struct pci_dev *dev) { return -ENOSYS; }
static inline void *pci_iov_get_pf_drvdata(struct pci_dev *dev, struct pci_driver *pf_driver)
{
	return ERR_PTR(-EINVAL);
}
static inline bool pci_ari_enabled(struct pci_bus *bus) { return false; }

/* Busqueda y referencias */
struct pci_dev *lkpi_pci_get_device(unsigned int vendor, unsigned int device,
				    struct pci_dev *from);
struct pci_dev *lkpi_pci_get_subsys(unsigned int vendor, unsigned int device,
				    unsigned int ss_vendor, unsigned int ss_device,
				    struct pci_dev *from);
struct pci_dev *lkpi_pci_get_class(unsigned int class, struct pci_dev *from);
struct pci_dev *lkpi_pci_get_domain_bus_and_slot(int domain, unsigned int bus,
						 unsigned int devfn);
struct pci_dev *lkpi_pci_dev_get(struct pci_dev *dev);
void lkpi_pci_dev_put(struct pci_dev *dev);
const struct pci_device_id *lkpi_pci_match_id(const struct pci_device_id *ids,
					      struct pci_dev *dev);
#define pci_get_device(v, d, f) lkpi_pci_get_device(v, d, f)
#define pci_get_subsys(v, d, sv, sd, f) lkpi_pci_get_subsys(v, d, sv, sd, f)
#define pci_get_class(c, f) lkpi_pci_get_class(c, f)
#define pci_get_domain_bus_and_slot(d, b, f) lkpi_pci_get_domain_bus_and_slot(d, b, f)
#define pci_dev_get(d) lkpi_pci_dev_get(d)
#define pci_dev_put(d) lkpi_pci_dev_put(d)
#define pci_match_id(i, d) lkpi_pci_match_id(i, d)
DEFINE_FREE(pci_dev_put, struct pci_dev *, if (_T) pci_dev_put(_T))
static inline struct pci_dev *pci_get_slot(struct pci_bus *bus, unsigned int devfn)
{
	return pci_get_domain_bus_and_slot(0, bus->number, devfn);
}

/* Drivers */
int lkpi_pci_register_driver(struct pci_driver *drv, struct module *owner, const char *mod_name);
void lkpi_pci_unregister_driver(struct pci_driver *drv);
#define pci_register_driver(drv) lkpi_pci_register_driver(drv, THIS_MODULE, KBUILD_MODNAME)
#define __pci_register_driver(drv, o, n) lkpi_pci_register_driver(drv, o, n)
#define pci_unregister_driver(drv) lkpi_pci_unregister_driver(drv)
#define module_pci_driver(__pci_driver) \
	module_driver(__pci_driver, pci_register_driver, pci_unregister_driver)
#define builtin_pci_driver(__pci_driver) builtin_driver(__pci_driver, pci_register_driver)
#define pci_dev_driver(d) ((d)->driver)

/* Interrupciones (pci/msi.c) */
#define PCI_IRQ_INTX (1 << 0)
#define PCI_IRQ_LEGACY PCI_IRQ_INTX
#define PCI_IRQ_MSI (1 << 1)
#define PCI_IRQ_MSIX (1 << 2)
#define PCI_IRQ_AFFINITY (1 << 3)
#define PCI_IRQ_ALL_TYPES (PCI_IRQ_INTX | PCI_IRQ_MSI | PCI_IRQ_MSIX)

int lkpi_pci_alloc_irq_vectors_affinity(struct pci_dev *dev, unsigned int min_vecs,
					unsigned int max_vecs, unsigned int flags,
					struct irq_affinity *affd);
void lkpi_pci_free_irq_vectors(struct pci_dev *dev);
int lkpi_pci_irq_vector(struct pci_dev *dev, unsigned int nr);
const struct cpumask *lkpi_pci_irq_get_affinity(struct pci_dev *pdev, int vec);
int lkpi_pci_msi_vec_count(struct pci_dev *dev);
int lkpi_pci_msix_vec_count(struct pci_dev *dev);
#define pci_alloc_irq_vectors_affinity(d, mi, ma, f, a) \
	lkpi_pci_alloc_irq_vectors_affinity(d, mi, ma, f, a)
#define pci_alloc_irq_vectors(d, mi, ma, f) lkpi_pci_alloc_irq_vectors_affinity(d, mi, ma, f, NULL)
#define pci_free_irq_vectors(d) lkpi_pci_free_irq_vectors(d)
#define pci_irq_vector(d, n) lkpi_pci_irq_vector(d, n)
#define pci_irq_get_affinity(d, v) lkpi_pci_irq_get_affinity(d, v)
#define pci_msi_vec_count(d) lkpi_pci_msi_vec_count(d)
#define pci_msix_vec_count(d) lkpi_pci_msix_vec_count(d)
static inline int pci_enable_msi(struct pci_dev *dev)
{
	int rc = pci_alloc_irq_vectors(dev, 1, 1, PCI_IRQ_MSI);
	return rc < 0 ? rc : 0;
}
static inline void pci_disable_msi(struct pci_dev *dev) { pci_free_irq_vectors(dev); }
static inline bool pci_msix_can_alloc_dyn(struct pci_dev *dev) { return false; }

/* Mensajes */
#define pci_printk(level, pdev, fmt, arg...) dev_printk(level, &(pdev)->dev, fmt, ##arg)
#define pci_emerg(pdev, fmt, arg...) dev_emerg(&(pdev)->dev, fmt, ##arg)
#define pci_alert(pdev, fmt, arg...) dev_alert(&(pdev)->dev, fmt, ##arg)
#define pci_crit(pdev, fmt, arg...) dev_crit(&(pdev)->dev, fmt, ##arg)
#define pci_err(pdev, fmt, arg...) dev_err(&(pdev)->dev, fmt, ##arg)
#define pci_warn(pdev, fmt, arg...) dev_warn(&(pdev)->dev, fmt, ##arg)
#define pci_warn_once(pdev, fmt, arg...) dev_warn_once(&(pdev)->dev, fmt, ##arg)
#define pci_notice(pdev, fmt, arg...) dev_notice(&(pdev)->dev, fmt, ##arg)
#define pci_info(pdev, fmt, arg...) dev_info(&(pdev)->dev, fmt, ##arg)
#define pci_dbg(pdev, fmt, arg...) dev_dbg(&(pdev)->dev, fmt, ##arg)
#define pci_info_ratelimited(pdev, fmt, arg...) dev_info_ratelimited(&(pdev)->dev, fmt, ##arg)
#define pci_WARN(pdev, condition, fmt, arg...) WARN(condition, "%s %s: " fmt, \
	dev_driver_string(&(pdev)->dev), pci_name(pdev), ##arg)
#define pci_WARN_ONCE(pdev, condition, fmt, arg...) WARN_ONCE(condition, "%s %s: " fmt, \
	dev_driver_string(&(pdev)->dev), pci_name(pdev), ##arg)


#define PCIBIOS_MIN_MEM 0x10000000UL
#define PCI_DEVID(bus, devfn) ((((u16)(bus)) << 8) | (devfn))
typedef resource_size_t (*resource_alignf)(void *data, const struct resource *res, resource_size_t size,
					   resource_size_t align);
resource_size_t lkpi_pcibios_align_resource(void *data, const struct resource *res, resource_size_t size,
					     resource_size_t align);
#define pcibios_align_resource lkpi_pcibios_align_resource
int lkpi_pci_bus_read_config_byte(struct pci_bus *bus, unsigned int devfn, int where, u8 *val);
int lkpi_pci_bus_read_config_word(struct pci_bus *bus, unsigned int devfn, int where, u16 *val);
int lkpi_pci_bus_read_config_dword(struct pci_bus *bus, unsigned int devfn, int where, u32 *val);
int lkpi_pci_bus_write_config_byte(struct pci_bus *bus, unsigned int devfn, int where, u8 val);
#define pci_bus_read_config_byte lkpi_pci_bus_read_config_byte
#define pci_bus_read_config_word lkpi_pci_bus_read_config_word
#define pci_bus_read_config_dword lkpi_pci_bus_read_config_dword
#define pci_bus_write_config_byte lkpi_pci_bus_write_config_byte
int lkpi_pci_dev_present(const struct pci_device_id *ids);
#define pci_dev_present lkpi_pci_dev_present
struct pci_dev *lkpi_pcie_find_root_port(struct pci_dev *dev);
#define pcie_find_root_port lkpi_pcie_find_root_port
/* Sin reasignacion de recursos PCI (resize BAR, ROM, reserva de ventanas): fallan con -ENOSYS / NULL. */
static inline void __iomem *pci_map_rom(struct pci_dev *pdev, size_t *size) { return NULL; }
static inline void pci_unmap_rom(struct pci_dev *pdev, void __iomem *rom) {}
static inline int pci_resize_resource(struct pci_dev *dev, int i, int size, int exclude_bars) { return -ENOSYS; }
static inline void pci_release_resource(struct pci_dev *dev, int resno) {}
static inline u32 pci_rebar_get_possible_sizes(struct pci_dev *pdev, int bar) { return 0; }
static inline int pci_rebar_bytes_to_size(u64 bytes)
{
	int size = ilog2(bytes) - 20;	/* 1 MiB = 0 */

	return size < 0 ? 0 : size;
}
static inline int pci_bus_alloc_resource(struct pci_bus *bus, struct resource *res, resource_size_t size,
					 resource_size_t align, resource_size_t min, unsigned long type_mask,
					 resource_alignf alignf, void *alignf_data)
{
	return -ENOSYS;
}
static inline bool pnp_range_reserved(resource_size_t start, resource_size_t end) { return false; }
static inline const char *pci_power_name(pci_power_t state)
{
	static const char * const names[] = { "D0", "D1", "D2", "D3hot", "D3cold", "unknown" };

	return (unsigned int)state < ARRAY_SIZE(names) ? names[state] : "unknown";
}
#define pci_bus_for_each_resource(bus, res, ...) for ((res) = NULL; (res); )
static inline void pci_assign_unassigned_bus_resources(struct pci_bus *bus) {}
#endif
