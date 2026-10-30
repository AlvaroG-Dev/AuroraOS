/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_MEI_AUX_H
#define _LKPI_LINUX_MEI_AUX_H
#include <linux/auxiliary_bus.h>
struct mei_aux_device {
	struct auxiliary_device aux_dev;
	int irq;
	struct resource bar;
	struct resource ext_op_mem;
	bool slow_firmware;
};
#define auxiliary_dev_to_mei_aux_dev(auxiliary_dev) container_of(auxiliary_dev, struct mei_aux_device, aux_dev)
#endif
