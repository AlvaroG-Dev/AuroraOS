/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Tablas de identificacion de dispositivos (MODULE_DEVICE_TABLE). Solo los
 * buses que la capa implementa: PCI, plataforma y virtio (el bus virtio
 * llegara con el nucleo virtio, ver Plan_LinuxKPI.md).
 */
#ifndef _LKPI_LINUX_MOD_DEVICETABLE_H
#define _LKPI_LINUX_MOD_DEVICETABLE_H

#include <linux/types.h>
#include <linux/uuid.h>


typedef unsigned long kernel_ulong_t;

#define PCI_ANY_ID (~0)

struct pci_device_id {
	__u32 vendor, device;		/* o PCI_ANY_ID */
	__u32 subvendor, subdevice;	/* o PCI_ANY_ID */
	__u32 class, class_mask;	/* clase, subclase y prog-if */
	kernel_ulong_t driver_data;
	__u32 override_only;
};

#define PLATFORM_NAME_SIZE 24
struct platform_device_id {
	char name[PLATFORM_NAME_SIZE];
	kernel_ulong_t driver_data;
};

#define I2C_NAME_SIZE 20
#define I2C_MODULE_PREFIX "i2c:"
struct i2c_device_id {
	char name[I2C_NAME_SIZE];
	kernel_ulong_t driver_data;
};

struct of_device_id {
	char name[32];
	char type[32];
	char compatible[128];
	const void *data;
};

struct acpi_device_id {
	__u8 id[16];
	kernel_ulong_t driver_data;
	__u32 cls;
	__u32 cls_msk;
};

#define VIRTIO_DEV_ANY_ID 0xffffffff
struct virtio_device_id {
	__u32 device;
	__u32 vendor;
};

#ifndef MDIO_NAME_SIZE
#define MDIO_NAME_SIZE		32
#define MDIO_MODULE_PREFIX	"mdio:"
#define MDIO_ID_FMT "%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u%u"
#define MDIO_ID_ARGS(_id) \
	((_id)>>31) & 1, ((_id)>>30) & 1, ((_id)>>29) & 1, ((_id)>>28) & 1, \
	((_id)>>27) & 1, ((_id)>>26) & 1, ((_id)>>25) & 1, ((_id)>>24) & 1, \
	((_id)>>23) & 1, ((_id)>>22) & 1, ((_id)>>21) & 1, ((_id)>>20) & 1, \
	((_id)>>19) & 1, ((_id)>>18) & 1, ((_id)>>17) & 1, ((_id)>>16) & 1, \
	((_id)>>15) & 1, ((_id)>>14) & 1, ((_id)>>13) & 1, ((_id)>>12) & 1, \
	((_id)>>11) & 1, ((_id)>>10) & 1, ((_id)>>9) & 1, ((_id)>>8) & 1, \
	((_id)>>7) & 1, ((_id)>>6) & 1, ((_id)>>5) & 1, ((_id)>>4) & 1, \
	((_id)>>3) & 1, ((_id)>>2) & 1, ((_id)>>1) & 1, (_id) & 1
struct mdio_device_id {
	__u32 phy_id;
	__u32 phy_id_mask;
};
#endif

#endif
