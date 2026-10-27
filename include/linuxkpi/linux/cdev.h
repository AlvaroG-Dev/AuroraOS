/* SPDX-License-Identifier: GPL-2.0-only */
/* Dispositivos de caracteres (K8, L4): cdev_add registra el rango de dev_t;
 * el nodo de /dev lo crea device_add() de un device con devt. */
#ifndef _LKPI_LINUX_CDEV_H
#define _LKPI_LINUX_CDEV_H

#include <linux/kobject.h>
#include <linux/kdev_t.h>
#include <linux/list.h>
#include <linux/fs.h>

struct device;

struct cdev {
	struct kobject kobj;
	struct module *owner;
	const struct file_operations *ops;
	struct list_head list;		/* registro de la capa */
	dev_t dev;
	unsigned int count;
	bool __lkpi_dynamic;
};

void lkpi_cdev_init(struct cdev *cdev, const struct file_operations *fops);
struct cdev *lkpi_cdev_alloc(void);
int lkpi_cdev_add(struct cdev *p, dev_t dev, unsigned int count);
void lkpi_cdev_del(struct cdev *p);
void lkpi_cdev_set_parent(struct cdev *p, struct kobject *kobj);
int lkpi_cdev_device_add(struct cdev *cdev, struct device *dev);
void lkpi_cdev_device_del(struct cdev *cdev, struct device *dev);
#define cdev_init(c, f) lkpi_cdev_init(c, f)
#define cdev_alloc() lkpi_cdev_alloc()
#define cdev_add(c, d, n) lkpi_cdev_add(c, d, n)
#define cdev_del(c) lkpi_cdev_del(c)
#define cdev_set_parent(c, k) lkpi_cdev_set_parent(c, k)
#define cdev_device_add(c, d) lkpi_cdev_device_add(c, d)
#define cdev_device_del(c, d) lkpi_cdev_device_del(c, d)

#endif
