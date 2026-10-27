// SPDX-License-Identifier: GPL-2.0-only
/*
 * lkpi_virtio.kur: punto de entrada unico. Los ficheros de drivers/virtio
 * traen cada uno su initcall (renombrado por drv.mk); aqui se encadenan: el
 * bus virtio primero, el driver PCI (que crea los dispositivos) despues.
 */
#include <linux/module.h>
#include <linux/init.h>

int lkpi_virtio_core_init(void);
void lkpi_virtio_core_exit(void);
int lkpi_virtio_pci_init(void);
void lkpi_virtio_pci_exit(void);

static int __init lkpi_virtio_init(void)
{
	int ret = lkpi_virtio_core_init();

	if (ret)
		return ret;
	ret = lkpi_virtio_pci_init();
	if (ret)
		lkpi_virtio_core_exit();
	return ret;
}

static void __exit lkpi_virtio_exit(void)
{
	lkpi_virtio_pci_exit();
	lkpi_virtio_core_exit();
}

module_init(lkpi_virtio_init);
module_exit(lkpi_virtio_exit);
MODULE_DESCRIPTION("Nucleo virtio de Linux sobre LinuxKPI");
MODULE_LICENSE("GPL");
