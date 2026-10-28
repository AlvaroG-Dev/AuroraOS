// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCI (L7): lo que i915 pide ademas de pci.c: configuracion por bus/devfn
 * (puente del host), pci_dev_present() y el puerto raiz.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/pci.h>
#include "../aurora.h"
#include "../lkpi_internal.h"

static bool cfg_ok(int where, int size)
{
	return where >= 0 && where + size <= PCI_CFG_SPACE_SIZE && !(where & (size - 1));
}

int lkpi_pci_bus_read_config_byte(struct pci_bus *bus, unsigned int devfn, int where, u8 *val)
{
	if (!cfg_ok(where, 1)) {
		*val = 0xff;
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}
	*val = aurora_pci_read_config_byte(bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), where);
	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL(lkpi_pci_bus_read_config_byte);

int lkpi_pci_bus_read_config_word(struct pci_bus *bus, unsigned int devfn, int where, u16 *val)
{
	if (!cfg_ok(where, 2)) {
		*val = 0xffff;
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}
	*val = aurora_pci_read_config_word(bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), where);
	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL(lkpi_pci_bus_read_config_word);

int lkpi_pci_bus_read_config_dword(struct pci_bus *bus, unsigned int devfn, int where, u32 *val)
{
	if (!cfg_ok(where, 4)) {
		*val = 0xffffffff;
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}
	*val = aurora_pci_read_config_dword(bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), where);
	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL(lkpi_pci_bus_read_config_dword);

int lkpi_pci_bus_write_config_byte(struct pci_bus *bus, unsigned int devfn, int where, u8 val)
{
	if (!cfg_ok(where, 1))
		return PCIBIOS_BAD_REGISTER_NUMBER;
	aurora_pci_write_config_byte(bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), where, val);
	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL(lkpi_pci_bus_write_config_byte);

/* Algun dispositivo del bus casa con la tabla (vendor/device/subsistema/clase con mascara). */
int lkpi_pci_dev_present(const struct pci_device_id *ids)
{
	const struct aurora_pci_device *devs;
	int count = 0;

	devs = aurora_pci_get_devices(&count);
	for (int i = 0; devs && i < count; i++) {
		const struct aurora_pci_device *d = &devs[i];

		for (const struct pci_device_id *id = ids; id->vendor || id->subvendor || id->class_mask; id++) {
			u32 class = (d->class_id << 16) | (d->subclass_id << 8) | d->prog_if;

			if ((id->vendor == PCI_ANY_ID || id->vendor == d->vendor_id) &&
			    (id->device == PCI_ANY_ID || id->device == d->device_id) &&
			    (id->subvendor == PCI_ANY_ID || id->subvendor == d->subsystem_vendor_id) &&
			    (id->subdevice == PCI_ANY_ID || id->subdevice == d->subsystem_id) &&
			    !((id->class ^ class) & id->class_mask))
				return 1;
		}
	}
	return 0;
}
EXPORT_SYMBOL(lkpi_pci_dev_present);

struct pci_dev *lkpi_pcie_find_root_port(struct pci_dev *dev)
{
	while (dev) {
		struct pci_dev *bridge = dev->bus ? dev->bus->self : NULL;

		if (pci_is_pcie(dev) && pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT)
			return dev;
		if (!bridge)
			return NULL;
		dev = bridge;
	}
	return NULL;
}
EXPORT_SYMBOL(lkpi_pcie_find_root_port);

/* Sin reasignacion de ventanas: el MCHBAR de Gen2-4 no se reserva (resource_alignf de pci_bus_alloc_resource) */
resource_size_t lkpi_pcibios_align_resource(void *data, const struct resource *res, resource_size_t size,
					     resource_size_t align)
{
	return res->start;
}
EXPORT_SYMBOL(lkpi_pcibios_align_resource);
