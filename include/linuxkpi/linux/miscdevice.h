/* SPDX-License-Identifier: GPL-2.0-only */
/* misc_register (K8, L4): major 10, /dev/<name>, clase "misc". En open,
 * file->private_data apunta a la miscdevice, como en Linux. */
#ifndef _LKPI_LINUX_MISCDEVICE_H
#define _LKPI_LINUX_MISCDEVICE_H

#include <linux/major.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/device.h>

#define MISC_DYNAMIC_MINOR 255

struct miscdevice {
	int minor;
	const char *name;
	const struct file_operations *fops;
	struct list_head list;
	struct device *parent;
	struct device *this_device;
	const struct attribute_group **groups;
	const char *nodename;
	umode_t mode;
};

int lkpi_misc_register(struct miscdevice *misc);
void lkpi_misc_deregister(struct miscdevice *misc);
#define misc_register(m) lkpi_misc_register(m)
#define misc_deregister(m) lkpi_misc_deregister(m)
#define module_misc_device(__misc_device) \
	module_driver(__misc_device, misc_register, misc_deregister)

#endif
