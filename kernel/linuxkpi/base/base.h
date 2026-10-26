/* SPDX-License-Identifier: GPL-2.0-only */
/* Interno del modelo de dispositivos de la capa (directorio base/). */
#ifndef _LKPI_BASE_H
#define _LKPI_BASE_H

#include <linux/device.h>
#include <linux/notifier.h>

/*
 * Un unico cerrojo para las listas de buses, drivers, dispositivos de cada
 * bus y clases. No se tiene durante probe/remove (que pueden registrar
 * dispositivos o drivers): esos van con device_lock(dev).
 */
extern struct mutex lkpi_dev_mutex;

/* Raices de /sys: "devices", "bus", "class", "devices/virtual". */
extern struct kobject *lkpi_devices_kobj;
extern struct kobject *lkpi_bus_kobj;
extern struct kobject *lkpi_class_kobj;
extern struct kobject *lkpi_virtual_kobj;

struct lkpi_bus_priv {
	struct kobject kobj;		/* bus/<nombre> */
	struct list_head node;
	const struct bus_type *bus;
	struct kobject *devices_kobj;	/* bus/<nombre>/devices */
	struct kobject *drivers_kobj;	/* bus/<nombre>/drivers */
	struct list_head devices;	/* device.lkpi_bus_node */
	struct list_head drivers;	/* driver_private.node */
	struct blocking_notifier_head notifier;
	/* PCI: Aurora ya publica bus/pci/devices/<BDF>; la capa no los enlaza. */
	bool static_devices_dir;
	bool dead;
};

struct driver_private {
	struct kobject kobj;		/* bus/<bus>/drivers/<nombre> */
	struct list_head node;		/* en lkpi_bus_priv.drivers */
	struct list_head devices;	/* device.lkpi_drv_node */
	struct device_driver *driver;
	struct lkpi_bus_priv *bus;
	bool dead;			/* driver_unregister en curso */
};

struct lkpi_bus_priv *lkpi_bus_priv_find(const struct bus_type *bus);
int lkpi_bus_add_device(struct device *dev);
void lkpi_bus_remove_device(struct device *dev);
void lkpi_bus_probe_device(struct device *dev);
void lkpi_bus_notify(struct device *dev, unsigned long action);
void lkpi_deferred_probe_trigger(void);

int lkpi_class_add_device(struct device *dev);
void lkpi_class_remove_device(struct device *dev);
struct kobject *lkpi_class_virtual_dir(const struct class *cls);

int lkpi_sysfs_link_path(const char *dir, const char *name, const char *target_path);
void lkpi_sysfs_unlink_path(const char *dir, const char *name);

/* Copia de una lista de dispositivos con referencia (get_device). */
struct lkpi_dev_snap {
	struct device **devs;
	int n;
};
void lkpi_dev_snap_put(struct lkpi_dev_snap *s);

#endif
