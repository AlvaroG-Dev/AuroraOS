/* SPDX-License-Identifier: GPL-2.0-only */
/* Gancho de "memoria restringida" de virtio: sin CONFIG_VIRTIO_ANCHOR no hay nada. */
#ifndef _LKPI_LINUX_VIRTIO_ANCHOR_H
#define _LKPI_LINUX_VIRTIO_ANCHOR_H

#include <linux/types.h>

struct virtio_device;

bool virtio_require_restricted_mem_acc(struct virtio_device *dev);
extern bool (*virtio_check_mem_acc_cb)(struct virtio_device *dev);

static inline void virtio_set_mem_acc_cb(bool (*func)(struct virtio_device *))
{
	virtio_check_mem_acc_cb = func;
}

#endif
