// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCI de Linux sobre el bus PCI de Aurora (K5).
 *
 * Cada pci_driver de Linux tiene un "driver sombra" en el pci_bus de Aurora
 * (struct aurora_driver con una id_table comodin). Aurora lo prueba con los
 * dispositivos libres (al registrarlo y en hot-plug); la sombra crea el
 * struct pci_dev, comprueba la id_table de Linux (devuelve -ENODEV si no
 * casa, y Aurora sigue con el siguiente driver) y vincula el driver de Linux
 * en el modelo de dispositivos de la capa (device_driver_attach). El remove
 * de Aurora (hot-unplug, bus_del_driver) desvincula y quita el pci_dev.
 *
 * En la capa el bus "pci" no hace match propio: quien decide es Aurora.
 * /sys/devices/pci0000:00/<BDF> es el pci_dev de la capa; /sys/bus/pci/devices
 * sigue siendo el de Aurora (con el enlace driver que anade la capa).
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include "../aurora.h"
#include "../lkpi_internal.h"
#include "../base/base.h"

static struct device *lkpi_pci_root;		/* /sys/devices/pci0000:00 */
static struct pci_bus *lkpi_pci_buses[256];
static LIST_HEAD(lkpi_pci_devs);		/* todos los pci_dev de la capa */
static DEFINE_MUTEX(lkpi_pci_mutex);

static inline u8 bus_of(const struct pci_dev *d) { return d->lkpi_bus; }
static inline u8 slot_of(const struct pci_dev *d) { return d->lkpi_slot; }
static inline u8 func_of(const struct pci_dev *d) { return d->lkpi_func; }

/* ------------------------------------------- espacio de configuracion */

static bool lkpi_cfg_ok(int where, int size)
{
	return where >= 0 && where + size <= PCI_CFG_SPACE_SIZE && !(where & (size - 1));
}

/* Offsets 256..4095 (capacidades extendidas): solo lectura, por ECAM si hay MCFG. */
static bool lkpi_cfg_ext_ok(int where, int size)
{
	return where >= PCI_CFG_SPACE_SIZE && where + size <= PCI_CFG_SPACE_EXP_SIZE &&
	       !(where & (size - 1)) && aurora_pci_ecam_available();
}

static u32 lkpi_cfg_ext_read(const struct pci_dev *dev, int where, int size)
{
	u32 v = aurora_pci_read_config_ext_dword(bus_of(dev), slot_of(dev), func_of(dev),
						 where & ~3);

	return size == 4 ? v : (v >> ((where & 3) * 8)) & (size == 2 ? 0xffff : 0xff);
}

int lkpi_pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val)
{
	if (lkpi_cfg_ext_ok(where, 1)) {
		*val = lkpi_cfg_ext_read(dev, where, 1);
		return PCIBIOS_SUCCESSFUL;
	}
	if (!lkpi_cfg_ok(where, 1)) {
		*val = 0xff;
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}
	*val = aurora_pci_read_config_byte(bus_of(dev), slot_of(dev), func_of(dev), where);
	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL(lkpi_pci_read_config_byte);

int lkpi_pci_read_config_word(const struct pci_dev *dev, int where, u16 *val)
{
	if (lkpi_cfg_ext_ok(where, 2)) {
		*val = lkpi_cfg_ext_read(dev, where, 2);
		return PCIBIOS_SUCCESSFUL;
	}
	if (!lkpi_cfg_ok(where, 2)) {
		*val = 0xffff;
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}
	*val = aurora_pci_read_config_word(bus_of(dev), slot_of(dev), func_of(dev), where);
	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL(lkpi_pci_read_config_word);

int lkpi_pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val)
{
	if (lkpi_cfg_ext_ok(where, 4)) {
		*val = lkpi_cfg_ext_read(dev, where, 4);
		return PCIBIOS_SUCCESSFUL;
	}
	if (!lkpi_cfg_ok(where, 4)) {
		*val = 0xffffffff;
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}
	*val = aurora_pci_read_config_dword(bus_of(dev), slot_of(dev), func_of(dev), where);
	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL(lkpi_pci_read_config_dword);

int lkpi_pci_write_config_byte(const struct pci_dev *dev, int where, u8 val)
{
	if (!lkpi_cfg_ok(where, 1))
		return PCIBIOS_BAD_REGISTER_NUMBER;
	aurora_pci_write_config_byte(bus_of(dev), slot_of(dev), func_of(dev), where, val);
	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL(lkpi_pci_write_config_byte);

int lkpi_pci_write_config_word(const struct pci_dev *dev, int where, u16 val)
{
	if (!lkpi_cfg_ok(where, 2))
		return PCIBIOS_BAD_REGISTER_NUMBER;
	aurora_pci_write_config_word(bus_of(dev), slot_of(dev), func_of(dev), where, val);
	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL(lkpi_pci_write_config_word);

int lkpi_pci_write_config_dword(const struct pci_dev *dev, int where, u32 val)
{
	if (!lkpi_cfg_ok(where, 4))
		return PCIBIOS_BAD_REGISTER_NUMBER;
	aurora_pci_write_config_dword(bus_of(dev), slot_of(dev), func_of(dev), where, val);
	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL(lkpi_pci_write_config_dword);

static u16 cfg16(struct pci_dev *d, int where)
{
	u16 v;

	pci_read_config_word(d, where, &v);
	return v;
}

static u32 cfg32(struct pci_dev *d, int where)
{
	u32 v;

	pci_read_config_dword(d, where, &v);
	return v;
}

/* --------------------------------------------------- capabilities */

/* Recorre la lista desde el byte `pos`, que contiene el puntero al siguiente. */
static u8 pci_find_cap_from(struct pci_dev *dev, u8 pos, int cap)
{
	u8 id;
	int ttl = 48;

	pci_read_config_byte(dev, pos, &pos);
	while (ttl-- && pos >= PCI_STD_HEADER_SIZEOF) {
		pos &= ~3;
		pci_read_config_byte(dev, pos + PCI_CAP_LIST_ID, &id);
		if (id == 0xff)
			break;
		if (id == cap)
			return pos;
		pci_read_config_byte(dev, pos + PCI_CAP_LIST_NEXT, &pos);
	}
	return 0;
}

/* pos es el desplazamiento de una capability: se sigue su puntero al siguiente. */
u8 lkpi_pci_find_next_capability(struct pci_dev *dev, u8 pos, int cap)
{
	return pci_find_cap_from(dev, pos + PCI_CAP_LIST_NEXT, cap);
}
EXPORT_SYMBOL(lkpi_pci_find_next_capability);

u8 lkpi_pci_find_capability(struct pci_dev *dev, int cap)
{
	if (!(cfg16(dev, PCI_STATUS) & PCI_STATUS_CAP_LIST))
		return 0;
	return pci_find_cap_from(dev, PCI_CAPABILITY_LIST, cap);
}
EXPORT_SYMBOL(lkpi_pci_find_capability);

/* Sin MCFG (Aurora usa 0xCF8/0xCFC) el espacio extendido no se ve: devuelve 0. */
u16 lkpi_pci_find_ext_capability(struct pci_dev *dev, int cap)
{
	if (!aurora_pci_ecam_available())
		return 0;
	return aurora_pci_find_ext_capability(bus_of(dev), slot_of(dev), func_of(dev), cap);
}
EXPORT_SYMBOL(lkpi_pci_find_ext_capability);

/* Registros de la capability PCIe que existen segun el tipo (v1 o v2). */
static bool pcie_cap_has_reg(struct pci_dev *dev, int pos)
{
	if (!pci_is_pcie(dev))
		return false;
	if (pos < PCI_EXP_SLTCAP)
		return true;
	return (dev->pcie_flags_reg & PCI_EXP_FLAGS_VERS) > 1 || pos < PCI_EXP_DEVCAP2;
}

int lkpi_pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val)
{
	*val = 0;
	if (pos & 1)
		return PCIBIOS_BAD_REGISTER_NUMBER;
	if (!pcie_cap_has_reg(dev, pos))
		return 0;
	return pci_read_config_word(dev, dev->pcie_cap + pos, val);
}
EXPORT_SYMBOL(lkpi_pcie_capability_read_word);

int lkpi_pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val)
{
	*val = 0;
	if (pos & 3)
		return PCIBIOS_BAD_REGISTER_NUMBER;
	if (!pcie_cap_has_reg(dev, pos))
		return 0;
	return pci_read_config_dword(dev, dev->pcie_cap + pos, val);
}
EXPORT_SYMBOL(lkpi_pcie_capability_read_dword);

int lkpi_pcie_capability_write_word(struct pci_dev *dev, int pos, u16 val)
{
	if (pos & 1)
		return PCIBIOS_BAD_REGISTER_NUMBER;
	if (!pcie_cap_has_reg(dev, pos))
		return 0;
	return pci_write_config_word(dev, dev->pcie_cap + pos, val);
}
EXPORT_SYMBOL(lkpi_pcie_capability_write_word);

int lkpi_pcie_capability_write_dword(struct pci_dev *dev, int pos, u32 val)
{
	if (pos & 3)
		return PCIBIOS_BAD_REGISTER_NUMBER;
	if (!pcie_cap_has_reg(dev, pos))
		return 0;
	return pci_write_config_dword(dev, dev->pcie_cap + pos, val);
}
EXPORT_SYMBOL(lkpi_pcie_capability_write_dword);

int lkpi_pcie_capability_clear_and_set_word(struct pci_dev *dev, int pos, u16 clear, u16 set)
{
	u16 val;
	int rc = lkpi_pcie_capability_read_word(dev, pos, &val);

	if (rc)
		return rc;
	return lkpi_pcie_capability_write_word(dev, pos, (val & ~clear) | set);
}
EXPORT_SYMBOL(lkpi_pcie_capability_clear_and_set_word);

int lkpi_pcie_capability_clear_and_set_dword(struct pci_dev *dev, int pos, u32 clear, u32 set)
{
	u32 val;
	int rc = lkpi_pcie_capability_read_dword(dev, pos, &val);

	if (rc)
		return rc;
	return lkpi_pcie_capability_write_dword(dev, pos, (val & ~clear) | set);
}
EXPORT_SYMBOL(lkpi_pcie_capability_clear_and_set_dword);

/* ------------------------------------------------------------ BARs */

/* BAR sin tamano: solo la direccion (para pci_dev de dispositivos ajenos). */
static void lkpi_pci_read_bars(struct pci_dev *pdev)
{
	for (int i = 0; i < PCI_STD_NUM_BARS; i++) {
		u32 v = cfg32(pdev, PCI_BASE_ADDRESS_0 + 4 * i);
		struct resource *r = &pdev->resource[i];

		r->name = pci_name(pdev);
		if (!v)
			continue;
		if (v & PCI_BASE_ADDRESS_SPACE_IO) {
			r->start = v & PCI_BASE_ADDRESS_IO_MASK;
			r->flags = IORESOURCE_IO;
			continue;
		}
		r->start = v & PCI_BASE_ADDRESS_MEM_MASK;
		/* Como decode_bar() de Linux: los bits bajos del BAR (tipo 64 bit,
		 * prefetch) se conservan; el driver de NVIDIA los compara con
		 * PCI_BASE_ADDRESS_MEM_TYPE_64/PREFETCH. */
		r->flags = IORESOURCE_MEM | (v & ~PCI_BASE_ADDRESS_MEM_MASK & 0xf);
		if (v & PCI_BASE_ADDRESS_MEM_PREFETCH)
			r->flags |= IORESOURCE_PREFETCH;
		if ((v & PCI_BASE_ADDRESS_MEM_TYPE_MASK) == PCI_BASE_ADDRESS_MEM_TYPE_64 && i < 5) {
			r->start |= (u64)cfg32(pdev, PCI_BASE_ADDRESS_0 + 4 * (i + 1)) << 32;
			r->flags |= IORESOURCE_MEM_64;
			i++;
		}
	}
}

/*
 * Mide los BARs (escribir unos y leer la mascara) con la decodificacion
 * apagada, como la enumeracion de Linux. Solo al vincular un driver de la
 * capa: el dispositivo no lo usa nadie mas en ese momento.
 */
static void lkpi_pci_size_bars(struct pci_dev *pdev)
{
	u16 cmd;

	if (pdev->lkpi_bars_sized || pdev->hdr_type != PCI_HEADER_TYPE_NORMAL)
		return;
	memset(pdev->resource, 0, sizeof(pdev->resource));
	cmd = cfg16(pdev, PCI_COMMAND);
	pci_write_config_word(pdev, PCI_COMMAND, cmd & ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY));
	for (int i = 0; i < PCI_STD_NUM_BARS; i++) {
		int off = PCI_BASE_ADDRESS_0 + 4 * i;
		struct resource *r = &pdev->resource[i];
		u32 orig = cfg32(pdev, off), mask;
		u64 base, size;

		r->name = pci_name(pdev);
		pci_write_config_dword(pdev, off, ~0U);
		mask = cfg32(pdev, off);
		pci_write_config_dword(pdev, off, orig);
		if (!mask || mask == ~0U)
			continue;
		if (orig & PCI_BASE_ADDRESS_SPACE_IO) {
			base = orig & PCI_BASE_ADDRESS_IO_MASK;
			size = (u32)~(mask & PCI_BASE_ADDRESS_IO_MASK) + 1;
			size &= 0xffff;
			if (!size)
				continue;
			r->start = base;
			r->end = base + size - 1;
			r->flags = IORESOURCE_IO | IORESOURCE_SIZEALIGN;
			continue;
		}
		base = orig & PCI_BASE_ADDRESS_MEM_MASK;
		r->flags = IORESOURCE_MEM | IORESOURCE_SIZEALIGN | (orig & ~PCI_BASE_ADDRESS_MEM_MASK & 0xf);
		if (orig & PCI_BASE_ADDRESS_MEM_PREFETCH)
			r->flags |= IORESOURCE_PREFETCH;
		if ((orig & PCI_BASE_ADDRESS_MEM_TYPE_MASK) == PCI_BASE_ADDRESS_MEM_TYPE_64 &&
		    i < PCI_STD_NUM_BARS - 1) {
			u32 orig_hi = cfg32(pdev, off + 4), mask_hi;
			u64 m64;

			pci_write_config_dword(pdev, off + 4, ~0U);
			mask_hi = cfg32(pdev, off + 4);
			pci_write_config_dword(pdev, off + 4, orig_hi);
			base |= (u64)orig_hi << 32;
			m64 = ((u64)mask_hi << 32) | (mask & PCI_BASE_ADDRESS_MEM_MASK);
			size = ~m64 + 1;
			r->flags |= IORESOURCE_MEM_64;
			i++;
		} else {
			size = (u32)~(mask & PCI_BASE_ADDRESS_MEM_MASK) + 1;
		}
		if (!size)
			continue;
		r->start = base;
		r->end = base + size - 1;
		if (!base)
			r->flags |= IORESOURCE_UNSET;
	}
	pci_write_config_word(pdev, PCI_COMMAND, cmd);
	pdev->lkpi_bars_sized = true;
}

/* ------------------------------------------------- pci_dev y cache */

static struct pci_bus *lkpi_pci_bus_get(u8 number)
{
	struct pci_bus *b;

	mutex_lock(&lkpi_pci_mutex);
	b = lkpi_pci_buses[number];
	if (!b) {
		b = kzalloc(sizeof(*b), GFP_KERNEL);
		if (b) {
			INIT_LIST_HEAD(&b->node);
			b->number = number;
			lkpi_pci_buses[number] = b;
		}
	}
	mutex_unlock(&lkpi_pci_mutex);
	return b;
}

static void lkpi_pci_release_dev(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	mutex_lock(&lkpi_pci_mutex);
	list_del(&pdev->lkpi_node);
	mutex_unlock(&lkpi_pci_mutex);
	WARN_ON(pdev->lkpi_irqs);
	kfree(pdev);
}

/* El pci_dev de B:S.F con una referencia, si existe y sigue vivo. */
static struct pci_dev *lkpi_pci_cache_find(u8 bus, u8 slot, u8 func)
{
	struct pci_dev *p, *found = NULL;

	mutex_lock(&lkpi_pci_mutex);
	list_for_each_entry(p, &lkpi_pci_devs, lkpi_node)
		if (p->lkpi_bus == bus && p->lkpi_slot == slot && p->lkpi_func == func &&
		    kobject_get_unless_zero(&p->dev.kobj)) {
			found = p;
			break;
		}
	mutex_unlock(&lkpi_pci_mutex);
	return found;
}

static struct pci_dev *lkpi_pci_dev_create(u8 bus, u8 slot, u8 func)
{
	struct pci_dev *pdev;
	u32 cls;
	u8 line;

	pdev = kzalloc(sizeof(*pdev), GFP_KERNEL);
	if (!pdev)
		return NULL;
	pdev->bus = lkpi_pci_bus_get(bus);
	if (!pdev->bus) {
		kfree(pdev);
		return NULL;
	}
	pdev->lkpi_bus = bus;
	pdev->lkpi_slot = slot;
	pdev->lkpi_func = func;
	pdev->devfn = PCI_DEVFN(slot, func);
	device_initialize(&pdev->dev);
	pdev->dev.release = lkpi_pci_release_dev;
	pdev->dev.bus = &lkpi_pci_bus_type;
	pdev->dev.parent = lkpi_pci_root;
	dev_set_name(&pdev->dev, "0000:%02x:%02x.%d", bus, slot, func);
	INIT_LIST_HEAD(&pdev->bus_list);

	pdev->vendor = cfg16(pdev, PCI_VENDOR_ID);
	pdev->device = cfg16(pdev, PCI_DEVICE_ID);
	pdev->subsystem_vendor = cfg16(pdev, PCI_SUBSYSTEM_VENDOR_ID);
	pdev->subsystem_device = cfg16(pdev, PCI_SUBSYSTEM_ID);
	cls = cfg32(pdev, PCI_CLASS_REVISION);
	pdev->revision = cls & 0xff;
	pdev->class = cls >> 8;
	pci_read_config_byte(pdev, PCI_HEADER_TYPE, &pdev->hdr_type);
	pdev->multifunction = !!(pdev->hdr_type & PCI_HEADER_TYPE_MFD);
	pdev->hdr_type &= PCI_HEADER_TYPE_MASK;
	pci_read_config_byte(pdev, PCI_INTERRUPT_PIN, &pdev->pin);
	pci_read_config_byte(pdev, PCI_INTERRUPT_LINE, &line);
	/* INTx: la linea que dejo el firmware, si es una IRQ del IOAPIC (1..15). */
	pdev->irq = (pdev->pin && line > 0 && line < 16) ? line : 0;
	pdev->msi_cap = pci_find_capability(pdev, PCI_CAP_ID_MSI);
	pdev->msix_cap = pci_find_capability(pdev, PCI_CAP_ID_MSIX);
	pdev->pm_cap = pci_find_capability(pdev, PCI_CAP_ID_PM);
	pdev->pcie_cap = pci_find_capability(pdev, PCI_CAP_ID_EXP);
	if (pdev->pcie_cap)
		pdev->pcie_flags_reg = cfg16(pdev, pdev->pcie_cap + PCI_EXP_FLAGS);
	pdev->cfg_size = PCI_CFG_SPACE_SIZE;
	pdev->current_state = PCI_D0;
	pdev->error_state = pci_channel_io_normal;
	/* Como Linux: DMA de 32 bits hasta que el driver pida otra mascara. */
	pdev->dma_mask = DMA_BIT_MASK(32);
	pdev->dev.dma_mask = &pdev->dma_mask;
	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);
	pdev->dma_parms.max_segment_size = 0x10000;
	pdev->dev.dma_parms = &pdev->dma_parms;
	lkpi_pci_read_bars(pdev);

	mutex_lock(&lkpi_pci_mutex);
	list_add_tail(&pdev->lkpi_node, &lkpi_pci_devs);
	mutex_unlock(&lkpi_pci_mutex);
	return pdev;
}

static struct pci_dev *lkpi_pci_dev_get_or_create(u8 bus, u8 slot, u8 func)
{
	struct pci_dev *p = lkpi_pci_cache_find(bus, slot, func);

	return p ? p : lkpi_pci_dev_create(bus, slot, func);
}

struct pci_dev *lkpi_pci_dev_get(struct pci_dev *dev)
{
	if (dev)
		get_device(&dev->dev);
	return dev;
}
EXPORT_SYMBOL(lkpi_pci_dev_get);

void lkpi_pci_dev_put(struct pci_dev *dev)
{
	if (dev)
		put_device(&dev->dev);
}
EXPORT_SYMBOL(lkpi_pci_dev_put);

/* ----------------------------------------------------------- busqueda */

/* Dispositivo i-esimo presente en el bus de Aurora (B:S.F). */
static bool lkpi_pci_nth(int i, u8 *bus, u8 *slot, u8 *func)
{
	int count = 0;
	const struct aurora_pci_device *devs = aurora_pci_get_devices(&count);

	if (i >= count)
		return false;
	*bus = devs[i].bus;
	*slot = devs[i].slot;
	*func = devs[i].func;
	return true;
}

typedef bool (*lkpi_pci_matchfn)(struct pci_dev *pdev, const void *data);

static struct pci_dev *lkpi_pci_get_next(lkpi_pci_matchfn match, const void *data,
					 struct pci_dev *from)
{
	struct pci_dev *found = NULL;
	bool started = !from;
	u8 b, s, f;

	for (int i = 0; lkpi_pci_nth(i, &b, &s, &f); i++) {
		struct pci_dev *p;

		if (aurora_pci_read_config_word(b, s, f, PCI_VENDOR_ID) == 0xffff)
			continue;		/* hueco de un dispositivo retirado */
		if (!started) {
			started = from->lkpi_bus == b && from->lkpi_slot == s && from->lkpi_func == f;
			continue;
		}
		p = lkpi_pci_dev_get_or_create(b, s, f);
		if (!p)
			break;
		if (match(p, data)) {
			found = p;
			break;
		}
		pci_dev_put(p);
	}
	pci_dev_put(from);
	return found;
}

struct lkpi_pci_subsys {
	unsigned int vendor, device, ss_vendor, ss_device;
};

static bool lkpi_pci_match_subsys(struct pci_dev *p, const void *data)
{
	const struct lkpi_pci_subsys *id = data;

	return (id->vendor == PCI_ANY_ID || id->vendor == p->vendor) &&
	       (id->device == PCI_ANY_ID || id->device == p->device) &&
	       (id->ss_vendor == PCI_ANY_ID || id->ss_vendor == p->subsystem_vendor) &&
	       (id->ss_device == PCI_ANY_ID || id->ss_device == p->subsystem_device);
}

struct pci_dev *lkpi_pci_get_subsys(unsigned int vendor, unsigned int device,
				    unsigned int ss_vendor, unsigned int ss_device,
				    struct pci_dev *from)
{
	struct lkpi_pci_subsys id = { vendor, device, ss_vendor, ss_device };

	return lkpi_pci_get_next(lkpi_pci_match_subsys, &id, from);
}
EXPORT_SYMBOL(lkpi_pci_get_subsys);

struct pci_dev *lkpi_pci_get_device(unsigned int vendor, unsigned int device,
				    struct pci_dev *from)
{
	return lkpi_pci_get_subsys(vendor, device, PCI_ANY_ID, PCI_ANY_ID, from);
}
EXPORT_SYMBOL(lkpi_pci_get_device);

static bool lkpi_pci_match_class(struct pci_dev *p, const void *data)
{
	return p->class == *(const unsigned int *)data;
}

struct pci_dev *lkpi_pci_get_class(unsigned int class, struct pci_dev *from)
{
	return lkpi_pci_get_next(lkpi_pci_match_class, &class, from);
}
EXPORT_SYMBOL(lkpi_pci_get_class);

static struct pci_dev *lkpi_vga_default;
void lkpi_vga_set_default_device(struct pci_dev *pdev);

/*
 * Display-shadow: el dispositivo de pantalla que aloja el framebuffer del
 * firmware (alguno de sus BAR contiene la direccion fisica del GOP) pasa a
 * ser "sombra" para los drivers de Linux: ver mm/ioremap.c. Se calcula la
 * primera vez que alguien hace ioremap. `lkpi.shadow=0` en la linea del
 * kernel lo desactiva (un driver de pantalla de Linux con control real, L5b).
 */
static bool lkpi_bar_has(struct pci_dev *p, u64 addr)
{
	for (int i = 0; i < PCI_STD_NUM_BARS; i++) {
		struct resource *r = &p->resource[i];

		if ((r->flags & IORESOURCE_MEM) && r->end > r->start && addr >= r->start && addr <= r->end)
			return true;
	}
	return false;
}

static bool lkpi_match_display(struct pci_dev *p, const void *data)
{
	return (p->class >> 16) == PCI_BASE_CLASS_DISPLAY;
}

void lkpi_display_shadow_probe(void)
{
	static DEFINE_MUTEX(probe_lock);
	static bool done, probing;
	struct pci_dev *p = NULL;
	char opt[8];
	u64 fbp, fbs;
	bool fb_only = false;

	if (READ_ONCE(done) || READ_ONCE(probing))
		return;
	mutex_lock(&probe_lock);
	if (done || probing) {
		mutex_unlock(&probe_lock);
		return;
	}
	probing = true;
	/* lkpi.shadow=0: sin sombra; =fb: solo el BAR que aloja el framebuffer
	 * (los registros del dispositivo son reales: i915 los necesita). */
	if (aurora_cmdline_get("lkpi.shadow", opt, sizeof(opt))) {
		if (opt[0] == '0')
			goto out;
		fb_only = opt[0] == 'f';
	}
	if (!aurora_gpup_get_framebuffer(&fbp, &fbs))
		goto out;
	while ((p = lkpi_pci_get_next(lkpi_match_display, NULL, p)) != NULL) {
		bool only_fb = fb_only;

		if (!lkpi_bar_has(p, fbp))
			continue;
		/* GPUs reales (Intel, NVIDIA): los registros (BAR0) son de verdad; solo el
		 * BAR del framebuffer va en sombra. Con sombra en BAR0 RM de NVIDIA lee
		 * PMC_BOOT_0/42 = 0 y rechaza la GPU. */
		if (p->vendor == 0x8086 || p->vendor == 0x10de)
			only_fb = true;
		/* NVIDIA: nada en sombra. RM prueba el copy engine escribiendo VRAM por el
		 * BAR1 y leyendo por DMA; con el BAR1 en sombra (RAM) el CPU escribe en
		 * RAM y la GPU copia VRAM real, y memmgrTestCeUtils falla. */
		if (p->vendor == 0x10de) {
			lkpi_vga_set_default_device(p);
			pr_info("linuxkpi: %s aloja el framebuffer del firmware: sin sombra (NVIDIA)\n",
				pci_name(p));
			pci_dev_put(p);
			break;
		}
		for (int i = 0; i < PCI_STD_NUM_BARS; i++) {
			struct resource *r = &p->resource[i];
			u64 len = r->end - r->start + 1, start = r->start;
			bool has_fb = fbp >= r->start && fbp < r->end;

			if (!(r->flags & IORESOURCE_MEM) || r->end <= r->start)
				continue;
			if (only_fb && !has_fb)
				continue;
			/* lkpi_vmalloc() no pasa de 64 MiB - 4 KiB (limite del heap): la
			 * sombra de un BAR grande cubre solo el framebuffer del firmware. */
			if (len > (32ULL << 20)) {
				len = 32ULL << 20;
				if (has_fb && fbp + fbs > start + len) {
					start = fbp & ~0xfffULL;
					len = min_t(u64, ALIGN(fbp + fbs, PAGE_SIZE) - start, len);
				}
			}
			if (lkpi_display_shadow_add(start, len, len <= 0x10000))
				pr_err("linuxkpi: %s: sin memoria para la sombra de BAR%d (%llu KiB)\n",
				       pci_name(p), i, (unsigned long long)(len >> 10));
		}
		lkpi_vga_set_default_device(p);
		pr_info("linuxkpi: %s aloja el framebuffer del firmware: %s en sombra\n",
			pci_name(p), only_fb ? "su BAR del framebuffer" : "sus BAR");
		pci_dev_put(p);
		break;
	}
out:
	WRITE_ONCE(done, true);
	probing = false;
	mutex_unlock(&probe_lock);
}

/* VGA de arranque (vgaarb): el que se fije o, si no, el primero de clase VGA. */

struct pci_dev *lkpi_vga_default_device(void)
{
	if (!lkpi_vga_default)
		lkpi_vga_default = lkpi_pci_get_class(PCI_CLASS_DISPLAY_VGA << 8, NULL);
	return lkpi_vga_default;
}
EXPORT_SYMBOL(lkpi_vga_default_device);

void lkpi_vga_set_default_device(struct pci_dev *pdev)
{
	lkpi_vga_default = pdev;
}
EXPORT_SYMBOL(lkpi_vga_set_default_device);

struct pci_dev *lkpi_pci_get_domain_bus_and_slot(int domain, unsigned int bus,
						 unsigned int devfn)
{
	u8 b, s, f;

	if (domain != 0 || bus > 255)
		return NULL;
	for (int i = 0; lkpi_pci_nth(i, &b, &s, &f); i++)
		if (b == bus && s == PCI_SLOT(devfn) && f == PCI_FUNC(devfn) &&
		    aurora_pci_read_config_word(b, s, f, PCI_VENDOR_ID) != 0xffff)
			return lkpi_pci_dev_get_or_create(b, s, f);
	return NULL;
}
EXPORT_SYMBOL(lkpi_pci_get_domain_bus_and_slot);

const struct pci_device_id *lkpi_pci_match_id(const struct pci_device_id *ids,
					      struct pci_dev *dev)
{
	if (!ids)
		return NULL;
	for (; ids->vendor || ids->subvendor || ids->class_mask; ids++) {
		if ((ids->vendor == PCI_ANY_ID || ids->vendor == dev->vendor) &&
		    (ids->device == PCI_ANY_ID || ids->device == dev->device) &&
		    (ids->subvendor == PCI_ANY_ID || ids->subvendor == dev->subsystem_vendor) &&
		    (ids->subdevice == PCI_ANY_ID || ids->subdevice == dev->subsystem_device) &&
		    !((ids->class ^ dev->class) & ids->class_mask))
			return ids;
	}
	return NULL;
}
EXPORT_SYMBOL(lkpi_pci_match_id);

/* --------------------------------------------- activacion y master */

static void lkpi_pci_cmd_update(struct pci_dev *dev, u16 clear, u16 set)
{
	u16 cmd = cfg16(dev, PCI_COMMAND), n = (cmd & ~clear) | set;

	if (n != cmd)
		pci_write_config_word(dev, PCI_COMMAND, n);
}

static int lkpi_pci_do_enable(struct pci_dev *dev, unsigned long mask)
{
	u16 set = 0;

	if (atomic_inc_return(&dev->enable_cnt) > 1)
		return 0;
	for (int i = 0; i < PCI_STD_NUM_BARS; i++) {
		unsigned long f = dev->resource[i].flags & mask;

		if (f & IORESOURCE_IO)
			set |= PCI_COMMAND_IO;
		if (f & IORESOURCE_MEM)
			set |= PCI_COMMAND_MEMORY;
	}
	lkpi_pci_cmd_update(dev, 0, set);
	if (dev->current_state != PCI_D0)
		pci_set_power_state(dev, PCI_D0);
	return 0;
}

int lkpi_pci_enable_device(struct pci_dev *dev)
{
	return lkpi_pci_do_enable(dev, IORESOURCE_IO | IORESOURCE_MEM);
}
EXPORT_SYMBOL(lkpi_pci_enable_device);

int lkpi_pci_enable_device_mem(struct pci_dev *dev)
{
	return lkpi_pci_do_enable(dev, IORESOURCE_MEM);
}
EXPORT_SYMBOL(lkpi_pci_enable_device_mem);

void lkpi_pci_disable_device(struct pci_dev *dev)
{
	if (atomic_read(&dev->enable_cnt) <= 0) {
		dev_warn(&dev->dev, "pci_disable_device sin pci_enable_device\n");
		return;
	}
	if (atomic_dec_return(&dev->enable_cnt) != 0)
		return;
	/* Como Linux: solo se quita el bus master (los BARs siguen decodificando). */
	lkpi_pci_cmd_update(dev, PCI_COMMAND_MASTER, 0);
	dev->is_busmaster = 0;
}
EXPORT_SYMBOL(lkpi_pci_disable_device);

static void lkpi_pcim_disable(void *data)
{
	struct pci_dev *pdev = data;

	if (pci_is_enabled(pdev))
		pci_disable_device(pdev);
}

int lkpi_pcim_enable_device(struct pci_dev *pdev)
{
	int rc = devm_add_action(&pdev->dev, lkpi_pcim_disable, pdev);

	if (rc)
		return rc;
	pdev->is_managed = 1;
	rc = pci_enable_device(pdev);
	if (rc)
		devm_remove_action(&pdev->dev, lkpi_pcim_disable, pdev);
	return rc;
}
EXPORT_SYMBOL(lkpi_pcim_enable_device);

void lkpi_pci_set_master(struct pci_dev *dev)
{
	lkpi_pci_cmd_update(dev, 0, PCI_COMMAND_MASTER);
	dev->is_busmaster = 1;
}
EXPORT_SYMBOL(lkpi_pci_set_master);

void lkpi_pci_clear_master(struct pci_dev *dev)
{
	lkpi_pci_cmd_update(dev, PCI_COMMAND_MASTER, 0);
	dev->is_busmaster = 0;
}
EXPORT_SYMBOL(lkpi_pci_clear_master);

void lkpi_pci_intx(struct pci_dev *dev, int enable)
{
	if (enable)
		lkpi_pci_cmd_update(dev, PCI_COMMAND_INTX_DISABLE, 0);
	else
		lkpi_pci_cmd_update(dev, 0, PCI_COMMAND_INTX_DISABLE);
}
EXPORT_SYMBOL(lkpi_pci_intx);

/* ---------------------------------------------------------- regiones */

int lkpi_pci_select_bars(struct pci_dev *dev, unsigned long flags)
{
	int bars = 0;

	for (int i = 0; i < PCI_STD_NUM_BARS; i++)
		if (pci_resource_flags(dev, i) & flags)
			bars |= 1 << i;
	return bars;
}
EXPORT_SYMBOL(lkpi_pci_select_bars);

int lkpi_pci_request_region(struct pci_dev *pdev, int bar, const char *name)
{
	struct resource *parent;

	if (bar < 0 || bar >= PCI_STD_NUM_BARS || pci_resource_len(pdev, bar) == 0)
		return 0;
	if (pci_resource_flags(pdev, bar) & IORESOURCE_IO)
		parent = &ioport_resource;
	else if (pci_resource_flags(pdev, bar) & IORESOURCE_MEM)
		parent = &iomem_resource;
	else
		return 0;
	if (!__request_region(parent, pci_resource_start(pdev, bar), pci_resource_len(pdev, bar),
			      name, 0)) {
		pci_warn(pdev, "BAR %d: no se pudo reservar\n", bar);
		return -EBUSY;
	}
	set_bit(bar, &pdev->lkpi_regions);
	return 0;
}
EXPORT_SYMBOL(lkpi_pci_request_region);

void lkpi_pci_release_region(struct pci_dev *pdev, int bar)
{
	struct resource *parent;

	if (bar < 0 || bar >= PCI_STD_NUM_BARS || !test_and_clear_bit(bar, &pdev->lkpi_regions))
		return;
	parent = (pci_resource_flags(pdev, bar) & IORESOURCE_IO) ? &ioport_resource
								  : &iomem_resource;
	__release_region(parent, pci_resource_start(pdev, bar), pci_resource_len(pdev, bar));
}
EXPORT_SYMBOL(lkpi_pci_release_region);

int lkpi_pci_request_selected_regions(struct pci_dev *pdev, int bars, const char *name)
{
	int i, rc = 0;

	for (i = 0; i < PCI_STD_NUM_BARS && !rc; i++)
		if (bars & (1 << i))
			rc = lkpi_pci_request_region(pdev, i, name);
	if (rc)
		while (--i >= 0)
			if (bars & (1 << i))
				lkpi_pci_release_region(pdev, i);
	return rc;
}
EXPORT_SYMBOL(lkpi_pci_request_selected_regions);

void lkpi_pci_release_selected_regions(struct pci_dev *pdev, int bars)
{
	for (int i = 0; i < PCI_STD_NUM_BARS; i++)
		if (bars & (1 << i))
			lkpi_pci_release_region(pdev, i);
}
EXPORT_SYMBOL(lkpi_pci_release_selected_regions);

static void lkpi_pcim_release_regions(void *data)
{
	struct pci_dev *pdev = data;

	lkpi_pci_release_selected_regions(pdev, (1 << PCI_STD_NUM_BARS) - 1);
}

int lkpi_pcim_request_all_regions(struct pci_dev *pdev, const char *name)
{
	int rc = lkpi_pci_request_selected_regions(pdev, (1 << PCI_STD_NUM_BARS) - 1, name);

	if (rc)
		return rc;
	return devm_add_action_or_reset(&pdev->dev, lkpi_pcim_release_regions, pdev);
}
EXPORT_SYMBOL(lkpi_pcim_request_all_regions);

/* ------------------------------------------------------------- iomap */

void __iomem *lkpi_pci_iomap_range(struct pci_dev *dev, int bar, unsigned long offset,
				   unsigned long maxlen, int type)
{
	resource_size_t start, len;
	unsigned long flags;

	if (bar < 0 || bar >= PCI_STD_NUM_BARS)
		return NULL;
	start = pci_resource_start(dev, bar);
	len = pci_resource_len(dev, bar);
	flags = pci_resource_flags(dev, bar);
	if (len <= offset || !start)
		return NULL;
	len -= offset;
	start += offset;
	if (maxlen && len > maxlen)
		len = maxlen;
	if (flags & IORESOURCE_IO)
		return ioport_map(start, len);
	if (flags & IORESOURCE_MEM)
		return type ? ioremap_wc(start, len) : ioremap(start, len);
	return NULL;
}
EXPORT_SYMBOL(lkpi_pci_iomap_range);

void lkpi_pci_iounmap(struct pci_dev *dev, void __iomem *addr)
{
	if (lkpi_iomap_is_port(addr))
		ioport_unmap(addr);
	else if (addr)
		iounmap(addr);
}
EXPORT_SYMBOL(lkpi_pci_iounmap);

struct lkpi_pcim_iomap_devres {
	void __iomem *addr;
	struct pci_dev *pdev;
};

static void lkpi_pcim_iomap_release(struct device *dev, void *res)
{
	struct lkpi_pcim_iomap_devres *r = res;

	lkpi_pci_iounmap(r->pdev, r->addr);
}

static int lkpi_pcim_iomap_match(struct device *dev, void *res, void *data)
{
	return ((struct lkpi_pcim_iomap_devres *)res)->addr == data;
}

void __iomem *lkpi_pcim_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen)
{
	struct lkpi_pcim_iomap_devres *r;

	r = devres_alloc(lkpi_pcim_iomap_release, sizeof(*r), GFP_KERNEL);
	if (!r)
		return NULL;
	r->pdev = pdev;
	r->addr = lkpi_pci_iomap_range(pdev, bar, 0, maxlen, 0);
	if (!r->addr) {
		devres_free(r);
		return NULL;
	}
	devres_add(&pdev->dev, r);
	return r->addr;
}
EXPORT_SYMBOL(lkpi_pcim_iomap);

void lkpi_pcim_iounmap(struct pci_dev *pdev, void __iomem *addr)
{
	WARN_ON(devres_release(&pdev->dev, lkpi_pcim_iomap_release, lkpi_pcim_iomap_match,
			       (void __force *)addr));
}
EXPORT_SYMBOL(lkpi_pcim_iounmap);

struct lkpi_pcim_region {
	struct pci_dev *pdev;
	int bar;
};

static void lkpi_pcim_region_release(void *data)
{
	struct lkpi_pcim_region *r = data;

	lkpi_pci_release_region(r->pdev, r->bar);
	kfree(r);
}

void __iomem *lkpi_pcim_iomap_region(struct pci_dev *pdev, int bar, const char *name)
{
	struct lkpi_pcim_region *r;
	void __iomem *addr;
	int rc;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return IOMEM_ERR_PTR(-ENOMEM);
	r->pdev = pdev;
	r->bar = bar;
	rc = lkpi_pci_request_region(pdev, bar, name);
	if (rc) {
		kfree(r);
		return IOMEM_ERR_PTR(rc);
	}
	rc = devm_add_action_or_reset(&pdev->dev, lkpi_pcim_region_release, r);
	if (rc)
		return IOMEM_ERR_PTR(rc);
	addr = lkpi_pcim_iomap(pdev, bar, 0);
	if (!addr) {
		devm_release_action(&pdev->dev, lkpi_pcim_region_release, r);
		return IOMEM_ERR_PTR(-ENOMEM);
	}
	return addr;
}
EXPORT_SYMBOL(lkpi_pcim_iomap_region);

/* ---------------------------------------------- estado, energia, FLR */

int lkpi_pci_save_state(struct pci_dev *dev)
{
	for (int i = 0; i < 16; i++)
		pci_read_config_dword(dev, i * 4, &dev->saved_config_space[i]);
	dev->state_saved = true;
	return 0;
}
EXPORT_SYMBOL(lkpi_pci_save_state);

void lkpi_pci_restore_state(struct pci_dev *dev)
{
	if (!dev->state_saved)
		return;
	/* De arriba abajo: el registro de comando (0x04) se escribe al final. */
	for (int i = 15; i >= 0; i--) {
		u32 cur = cfg32(dev, i * 4);

		if (cur != dev->saved_config_space[i])
			pci_write_config_dword(dev, i * 4, dev->saved_config_space[i]);
	}
	lkpi_pci_msi_restore(dev);
	dev->state_saved = false;
}
EXPORT_SYMBOL(lkpi_pci_restore_state);

int lkpi_pci_set_power_state(struct pci_dev *dev, pci_power_t state)
{
	u16 pmcsr;

	if (state == dev->current_state)
		return 0;
	if (!dev->pm_cap) {
		/* Sin capability PM el dispositivo esta siempre en D0. */
		return state == PCI_D0 ? 0 : -EIO;
	}
	if (state > PCI_D3hot)
		state = PCI_D3hot;	/* D3cold necesita la plataforma (ACPI) */
	pmcsr = cfg16(dev, dev->pm_cap + PCI_PM_CTRL);
	pmcsr = (pmcsr & ~PCI_PM_CTRL_STATE_MASK) | (u16)state;
	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, pmcsr);
	/* Tiempos de la especificacion PM: 10 ms desde/hacia D3hot. */
	if (state == PCI_D3hot || dev->current_state == PCI_D3hot)
		msleep(10);
	dev->current_state = state;
	return 0;
}
EXPORT_SYMBOL(lkpi_pci_set_power_state);

bool lkpi_pci_device_is_present(struct pci_dev *pdev)
{
	return cfg16(pdev, PCI_VENDOR_ID) != 0xffff;
}
EXPORT_SYMBOL(lkpi_pci_device_is_present);

bool lkpi_pci_wait_for_pending_transaction(struct pci_dev *dev)
{
	if (!pci_is_pcie(dev))
		return true;
	for (int i = 0; i < 4; i++) {
		u16 sta;

		if (i)
			msleep((1 << (i - 1)) * 100);
		pcie_capability_read_word(dev, PCI_EXP_DEVSTA, &sta);
		if (!(sta & PCI_EXP_DEVSTA_TRPND))
			return true;
	}
	return false;
}
EXPORT_SYMBOL(lkpi_pci_wait_for_pending_transaction);

bool lkpi_pcie_has_flr(struct pci_dev *dev)
{
	u32 cap;

	if (!pci_is_pcie(dev))
		return false;
	pcie_capability_read_dword(dev, PCI_EXP_DEVCAP, &cap);
	return cap & PCI_EXP_DEVCAP_FLR;
}
EXPORT_SYMBOL(lkpi_pcie_has_flr);

/*
 * Function Level Reset (PCIe). PROVISIONAL: no se ha podido probar (la
 * maquina pc de QEMU no tiene dispositivos PCIe con FLR). Espera las
 * transacciones pendientes, pide el reset, 100 ms y espera hasta 1 s a que
 * el dispositivo responda.
 */
int lkpi_pcie_flr(struct pci_dev *dev)
{
	if (!lkpi_pcie_has_flr(dev))
		return -ENOTTY;
	if (!lkpi_pci_wait_for_pending_transaction(dev))
		pci_err(dev, "FLR con transacciones pendientes\n");
	pcie_capability_set_word(dev, PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_BCR_FLR);
	msleep(100);
	for (int waited = 0; waited < 1000; waited += 10) {
		if (cfg32(dev, PCI_COMMAND) != ~0U)
			return 0;
		msleep(10);
	}
	pci_err(dev, "no responde tras el FLR\n");
	return -ENOTTY;
}
EXPORT_SYMBOL(lkpi_pcie_flr);

int lkpi_pci_reset_function_locked(struct pci_dev *dev)
{
	int rc;

	if (!lkpi_pcie_has_flr(dev))
		return -ENOTTY;		/* reset de bus o por PM: pendiente */
	pci_save_state(dev);
	rc = lkpi_pcie_flr(dev);
	pci_restore_state(dev);
	return rc;
}
EXPORT_SYMBOL(lkpi_pci_reset_function_locked);

int lkpi_pci_reset_function(struct pci_dev *dev)
{
	int rc;

	device_lock(&dev->dev);
	rc = lkpi_pci_reset_function_locked(dev);
	device_unlock(&dev->dev);
	return rc;
}
EXPORT_SYMBOL(lkpi_pci_reset_function);

/* ----------------------------------------------- bus pci de la capa */

static ssize_t vendor_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "0x%04x\n", to_pci_dev(dev)->vendor);
}
static DEVICE_ATTR_RO(vendor);
static ssize_t device_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "0x%04x\n", to_pci_dev(dev)->device);
}
static DEVICE_ATTR_RO(device);
static ssize_t class_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "0x%06x\n", to_pci_dev(dev)->class);
}
static DEVICE_ATTR_RO(class);
static ssize_t irq_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%u\n", to_pci_dev(dev)->irq);
}
static DEVICE_ATTR_RO(irq);
static ssize_t enable_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&to_pci_dev(dev)->enable_cnt));
}
static DEVICE_ATTR_RO(enable);

static struct attribute *lkpi_pci_dev_attrs[] = {
	&dev_attr_vendor.attr, &dev_attr_device.attr, &dev_attr_class.attr,
	&dev_attr_irq.attr, &dev_attr_enable.attr, NULL,
};
ATTRIBUTE_GROUPS(lkpi_pci_dev);

static int lkpi_pci_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct pci_dev *p = container_of_const(dev, struct pci_dev, dev);

	add_uevent_var(env, "PCI_CLASS=%04X", p->class);
	add_uevent_var(env, "PCI_ID=%04X:%04X", p->vendor, p->device);
	add_uevent_var(env, "PCI_SUBSYS_ID=%04X:%04X", p->subsystem_vendor, p->subsystem_device);
	add_uevent_var(env, "PCI_SLOT_NAME=%s", dev_name(dev));
	return add_uevent_var(env, "MODALIAS=pci:v%08Xd%08Xsv%08Xsd%08Xbc%02Xsc%02Xi%02X",
			      p->vendor, p->device, p->subsystem_vendor, p->subsystem_device,
			      (u8)(p->class >> 16), (u8)(p->class >> 8), (u8)p->class);
}

/* La decision de vincular la toma Aurora: aqui nunca hay match automatico. */
static int lkpi_pci_bus_match(struct device *dev, const struct device_driver *drv)
{
	return 0;
}

static int lkpi_pci_bus_probe(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pci_driver *drv = to_pci_driver(dev->driver);
	const struct pci_device_id *id = pci_match_id(drv->id_table, pdev);
	int rc;

	if (!id)
		return -ENODEV;
	pci_dev_get(pdev);
	pdev->driver = drv;
	rc = drv->probe(pdev, id);
	if (rc) {
		pdev->driver = NULL;
		lkpi_pci_free_irq_vectors(pdev);
		pci_dev_put(pdev);
	}
	return rc;
}

static void lkpi_pci_bus_remove(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pci_driver *drv = pdev->driver;

	if (drv && drv->remove)
		drv->remove(pdev);
	/* Lo que el driver no libero (como hace Linux, con aviso). */
	if (pdev->lkpi_irqs) {
		pci_warn(pdev, "el driver no libero sus vectores de IRQ\n");
		lkpi_pci_free_irq_vectors(pdev);
	}
	if (pci_is_enabled(pdev) && !pdev->is_managed)
		pci_warn(pdev, "el driver no llamo a pci_disable_device\n");
	pdev->driver = NULL;
	pci_dev_put(pdev);
}

static void lkpi_pci_bus_shutdown(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	if (pdev->driver && pdev->driver->shutdown)
		pdev->driver->shutdown(pdev);
}

const struct bus_type lkpi_pci_bus_type = {
	.name = "pci",
	.dev_groups = lkpi_pci_dev_groups,
	.match = lkpi_pci_bus_match,
	.uevent = lkpi_pci_uevent,
	.probe = lkpi_pci_bus_probe,
	.remove = lkpi_pci_bus_remove,
	.shutdown = lkpi_pci_bus_shutdown,
};
EXPORT_SYMBOL(lkpi_pci_bus_type);

/* ------------------------------------------- sombra en Aurora */

struct lkpi_pci_shadow {
	struct aurora_driver adrv;
	struct pci_driver *pdrv;
	struct aurora_pci_device_id ids[2];	/* comodin + fin */
	char name[32];
};

static int lkpi_pci_shadow_probe(struct aurora_device *adev)
{
	struct lkpi_pci_shadow *sh = container_of(adev->driver, struct lkpi_pci_shadow, adrv);
	const struct aurora_pci_device *ap = adev->priv;
	struct pci_dev *pdev;
	int rc;

	if (!ap)
		return -ENODEV;
	pdev = lkpi_pci_dev_get_or_create(ap->bus, ap->slot, ap->func);
	if (!pdev)
		return -ENOMEM;
	if (!pci_match_id(sh->pdrv->id_table, pdev)) {
		pci_dev_put(pdev);
		return -ENODEV;
	}
	if (pdev->dev.kobj.state_in_sysfs) {
		/* Ya lo tiene otro driver de la capa (no deberia: Aurora lo veria). */
		pci_dev_put(pdev);
		return -EBUSY;
	}
	lkpi_pci_size_bars(pdev);
	pdev->lkpi_adev = adev;
	rc = device_add(&pdev->dev);
	if (rc) {
		pci_dev_put(pdev);
		return rc;
	}
	rc = device_driver_attach(&sh->pdrv->driver, &pdev->dev);
	if (rc) {
		/* El probe de Linux fallo: Aurora puede ofrecerlo a otro driver, y si
		 * aur.skip se lo habia reservado a Linux, se lanza el .aur omitido (4B). */
		device_del(&pdev->dev);
		pdev->lkpi_adev = NULL;
		pci_dev_put(pdev);
		aurora_aur_bus_launch_skipped(ap->bus, ap->slot, ap->func);
		return -ENODEV;
	}
	/* La referencia de la creacion queda para el vinculo (se suelta en remove). */
	return 0;
}

static void lkpi_pci_shadow_remove(struct aurora_device *adev)
{
	const struct aurora_pci_device *ap = adev->priv;
	struct pci_dev *pdev;

	if (!ap)
		return;
	pdev = lkpi_pci_cache_find(ap->bus, ap->slot, ap->func);
	if (!pdev)
		return;
	if (pdev->lkpi_adev == adev && pdev->dev.kobj.state_in_sysfs) {
		device_release_driver(&pdev->dev);
		device_del(&pdev->dev);
		pdev->lkpi_adev = NULL;
		pci_dev_put(pdev);	/* la de la vinculacion */
	}
	pci_dev_put(pdev);		/* la de lkpi_pci_cache_find */
}

int lkpi_pci_register_driver(struct pci_driver *drv, struct module *owner, const char *mod_name)
{
	struct lkpi_pci_shadow *sh;
	int rc;

	drv->driver.name = drv->name;
	drv->driver.bus = &lkpi_pci_bus_type;
	drv->driver.owner = owner;
	drv->driver.mod_name = mod_name;
	drv->driver.groups = drv->groups;
	drv->driver.dev_groups = drv->dev_groups;
	spin_lock_init(&drv->dynids.lock);
	INIT_LIST_HEAD(&drv->dynids.list);

	sh = kzalloc(sizeof(*sh), GFP_KERNEL);
	if (!sh)
		return -ENOMEM;
	rc = driver_register(&drv->driver);
	if (rc) {
		kfree(sh);
		return rc;
	}
	snprintf(sh->name, sizeof(sh->name), "lkpi:%s", drv->name);
	sh->pdrv = drv;
	sh->ids[0] = (struct aurora_pci_device_id){
		.vendor_id = AURORA_PCI_ANY_ID, .device_id = AURORA_PCI_ANY_ID,
		.subvendor = AURORA_PCI_ANY_ID, .subdevice = AURORA_PCI_ANY_ID,
	};
	sh->adrv.name = sh->name;
	sh->adrv.bus = &aurora_pci_bus;
	sh->adrv.id_table = sh->ids;
	sh->adrv.probe = lkpi_pci_shadow_probe;
	sh->adrv.remove = lkpi_pci_shadow_remove;
	drv->lkpi_adrv = sh;
	/* Aurora prueba ya los dispositivos libres (sincrono). */
	rc = aurora_bus_add_driver(&sh->adrv);
	if (rc) {
		drv->lkpi_adrv = NULL;
		driver_unregister(&drv->driver);
		kfree(sh);
	}
	return rc;
}
EXPORT_SYMBOL(lkpi_pci_register_driver);

void lkpi_pci_unregister_driver(struct pci_driver *drv)
{
	struct lkpi_pci_shadow *sh = drv->lkpi_adrv;

	if (!sh)
		return;
	/* Aurora llama al remove de la sombra en cada dispositivo vinculado. */
	aurora_bus_del_driver(&sh->adrv);
	driver_unregister(&drv->driver);
	drv->lkpi_adrv = NULL;
	kfree(sh);
}
EXPORT_SYMBOL(lkpi_pci_unregister_driver);

/* ------------------------------------------------------------ arranque */

int lkpi_pci_init(void)
{
	int rc = bus_register(&lkpi_pci_bus_type);

	if (rc)
		return rc;
	lkpi_pci_root = root_device_register("pci0000:00");
	if (IS_ERR(lkpi_pci_root)) {
		rc = PTR_ERR(lkpi_pci_root);
		lkpi_pci_root = NULL;
		bus_unregister(&lkpi_pci_bus_type);
	}
	return rc;
}

void lkpi_pci_exit(void)
{
	if (lkpi_pci_root)
		root_device_unregister(lkpi_pci_root);
	lkpi_pci_root = NULL;
	bus_unregister(&lkpi_pci_bus_type);
	for (int i = 0; i < 256; i++) {
		kfree(lkpi_pci_buses[i]);
		lkpi_pci_buses[i] = NULL;
	}
	if (!list_empty(&lkpi_pci_devs))
		pr_warn("linuxkpi: quedan pci_dev con referencias al descargar la capa\n");
}
