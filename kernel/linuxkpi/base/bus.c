// SPDX-License-Identifier: GPL-2.0-only
/*
 * Buses y drivers (K4): bus_register, driver_register, match/probe/remove,
 * probe diferido (-EPROBE_DEFER) y notificadores del bus. Todo sincrono:
 * driver_register() prueba los dispositivos sin driver antes de volver y
 * device_add() prueba los drivers del bus.
 *
 * Las listas se recorren sobre una copia con referencias (lkpi_dev_snap):
 * el probe puede dormir y registrar dispositivos o drivers.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/notifier.h>
#include "base.h"
#include "../lkpi_internal.h"

static LIST_HEAD(lkpi_buses);

struct lkpi_bus_priv *lkpi_bus_priv_find(const struct bus_type *bus)
{
	struct lkpi_bus_priv *bp;

	list_for_each_entry(bp, &lkpi_buses, node)
		if (bp->bus == bus && !bp->dead)
			return bp;
	return NULL;
}

void lkpi_bus_notify(struct device *dev, unsigned long action)
{
	struct lkpi_bus_priv *bp;

	if (!dev->bus)
		return;
	mutex_lock(&lkpi_dev_mutex);
	bp = lkpi_bus_priv_find(dev->bus);
	mutex_unlock(&lkpi_dev_mutex);
	/* El bus no se da de baja con dispositivos dentro: bp sigue vivo. */
	if (bp)
		blocking_notifier_call_chain(&bp->notifier, action, dev);
}

static struct lkpi_dev_snap lkpi_bus_snap(struct lkpi_bus_priv *bp)
{
	struct lkpi_dev_snap s = { NULL, 0 };
	struct device *d;
	int n = 0;

	list_for_each_entry(d, &bp->devices, lkpi_bus_node)
		n++;
	s.devs = n ? kmalloc_array(n, sizeof(*s.devs), GFP_KERNEL) : NULL;
	if (s.devs)
		list_for_each_entry(d, &bp->devices, lkpi_bus_node)
			s.devs[s.n++] = get_device(d);
	return s;
}

/* ----------------------------------------------------- vincular/soltar */

static void lkpi_driver_sysfs_remove(struct device *dev)
{
	struct device_driver *drv = dev->driver;

	if (drv && drv->p)
		sysfs_remove_link(&drv->p->kobj, kobject_name(&dev->kobj));
	sysfs_remove_link(&dev->kobj, "driver");
	if (dev->lkpi_bus_link && dev->bus && drv && drv->p && drv->p->bus->static_devices_dir) {
		/* PCI: el enlace driver tambien en el bus/pci/devices/<BDF> de Aurora. */
		char *dir = kasprintf(GFP_KERNEL, "bus/%s/devices/%s", dev->bus->name, dev_name(dev));

		if (dir)
			lkpi_sysfs_unlink_path(dir, "driver");
		kfree(dir);
	}
}

static void lkpi_driver_sysfs_add(struct device *dev)
{
	struct device_driver *drv = dev->driver;

	sysfs_create_link(&drv->p->kobj, &dev->kobj, kobject_name(&dev->kobj));
	sysfs_create_link(&dev->kobj, &drv->p->kobj, "driver");
	if (dev->lkpi_bus_link && drv->p->bus->static_devices_dir) {
		char *dir = kasprintf(GFP_KERNEL, "bus/%s/devices/%s", dev->bus->name, dev_name(dev));

		if (dir)
			lkpi_sysfs_link_path(dir, "driver", drv->p->kobj.lkpi_path);
		kfree(dir);
	}
}

/*
 * Prueba drv con dev (device_lock tomado, dev sin driver). 1 si queda
 * vinculado, 0 si no, -EPROBE_DEFER si el driver pide reintentar.
 */
static int lkpi_really_probe(struct device *dev, struct device_driver *drv)
{
	int rc;

	if (drv->p->dead)
		return 0;
	dev->driver = drv;
	lkpi_driver_sysfs_add(dev);
	lkpi_bus_notify(dev, BUS_NOTIFY_BIND_DRIVER);
	if (dev->bus->dma_configure) {
		rc = dev->bus->dma_configure(dev);
		if (rc)
			goto fail;
	}
	if (dev->bus->probe)
		rc = dev->bus->probe(dev);
	else if (drv->probe)
		rc = drv->probe(dev);
	else
		rc = 0;
	if (rc)
		goto fail;
	rc = device_add_groups(dev, drv->dev_groups);
	if (rc) {
		dev_err(dev, "no se pudieron crear los atributos del driver: %d\n", rc);
		if (dev->bus->remove)
			dev->bus->remove(dev);
		else if (drv->remove)
			drv->remove(dev);
		goto fail;
	}
	mutex_lock(&lkpi_dev_mutex);
	list_add_tail(&dev->lkpi_drv_node, &drv->p->devices);
	dev->lkpi_deferred = false;
	mutex_unlock(&lkpi_dev_mutex);
	lkpi_bus_notify(dev, BUS_NOTIFY_BOUND_DRIVER);
	kobject_uevent(&dev->kobj, KOBJ_BIND);
	pr_debug("linuxkpi: '%s' vinculado a '%s'\n", dev_name(dev), drv->name);
	return 1;

fail:
	if (dev->bus->dma_cleanup)
		dev->bus->dma_cleanup(dev);
	lkpi_devres_release_all(dev);
	lkpi_driver_sysfs_remove(dev);
	dev->driver = NULL;
	dev_set_drvdata(dev, NULL);
	lkpi_bus_notify(dev, BUS_NOTIFY_DRIVER_NOT_BOUND);
	if (rc == -EPROBE_DEFER) {
		dev->lkpi_deferred = true;
		dev_dbg(dev, "probe de '%s' diferido\n", drv->name);
		return -EPROBE_DEFER;
	}
	if (rc != -ENODEV && rc != -ENXIO)
		pr_warn("linuxkpi: probe de '%s' con '%s' fallo: %d\n", dev_name(dev), drv->name,
			rc);
	return 0;
}

/* match + probe de un par. 1 si queda vinculado. */
static int lkpi_try_bind(struct device *dev, struct device_driver *drv)
{
	int m, rc = 0;

	m = drv->bus->match ? drv->bus->match(dev, drv) : 1;
	if (m == -EPROBE_DEFER) {
		dev->lkpi_deferred = true;
		return 0;
	}
	if (m <= 0)
		return 0;
	if (dev->parent && dev->bus->need_parent_lock)
		device_lock(dev->parent);
	device_lock(dev);
	if (!dev->driver && dev->kobj.state_in_sysfs)
		rc = lkpi_really_probe(dev, drv);
	device_unlock(dev);
	if (dev->parent && dev->bus->need_parent_lock)
		device_unlock(dev->parent);
	return rc == 1;
}

/* Copia de los drivers del bus con referencia en su kobject. */
static int lkpi_drivers_snap(struct lkpi_bus_priv *bp, struct driver_private ***out)
{
	struct driver_private *p, **v;
	int n = 0, i = 0;

	list_for_each_entry(p, &bp->drivers, node)
		n++;
	v = n ? kmalloc_array(n, sizeof(*v), GFP_KERNEL) : NULL;
	if (v)
		list_for_each_entry(p, &bp->drivers, node) {
			kobject_get(&p->kobj);
			v[i++] = p;
		}
	*out = v;
	return v ? i : 0;
}

static int lkpi_device_attach_all(struct device *dev)
{
	struct driver_private **drvs;
	struct lkpi_bus_priv *bp;
	int n, bound = 0;

	mutex_lock(&lkpi_dev_mutex);
	bp = dev->bus ? lkpi_bus_priv_find(dev->bus) : NULL;
	n = bp ? lkpi_drivers_snap(bp, &drvs) : 0;
	mutex_unlock(&lkpi_dev_mutex);
	for (int i = 0; i < n; i++) {
		if (!bound && !READ_ONCE(dev->driver))
			bound = lkpi_try_bind(dev, drvs[i]->driver);
		kobject_put(&drvs[i]->kobj);
	}
	if (n)
		kfree(drvs);
	if (bound)
		lkpi_deferred_probe_trigger();
	return bound;
}

int lkpi_device_attach(struct device *dev)
{
	if (dev->driver)
		return 1;
	return lkpi_device_attach_all(dev);
}
EXPORT_SYMBOL(lkpi_device_attach);

int lkpi_device_driver_attach(const struct device_driver *drv, struct device *dev)
{
	int bound;

	device_lock(dev);
	bound = !dev->driver && lkpi_really_probe(dev, (struct device_driver *)drv) == 1;
	device_unlock(dev);
	return bound ? 0 : -ENODEV;
}
EXPORT_SYMBOL(lkpi_device_driver_attach);

int lkpi_driver_attach(const struct device_driver *drv)
{
	struct lkpi_dev_snap s;
	int bound = 0;

	mutex_lock(&lkpi_dev_mutex);
	s = lkpi_bus_snap(drv->p->bus);
	mutex_unlock(&lkpi_dev_mutex);
	for (int i = 0; i < s.n; i++)
		if (!READ_ONCE(s.devs[i]->driver))
			bound += lkpi_try_bind(s.devs[i], (struct device_driver *)drv);
	lkpi_dev_snap_put(&s);
	if (bound)
		lkpi_deferred_probe_trigger();
	return 0;
}
EXPORT_SYMBOL(lkpi_driver_attach);

void lkpi_device_release_driver(struct device *dev)
{
	struct device_driver *drv;

	if (dev->parent && dev->bus && dev->bus->need_parent_lock)
		device_lock(dev->parent);
	device_lock(dev);
	drv = dev->driver;
	if (drv) {
		lkpi_bus_notify(dev, BUS_NOTIFY_UNBIND_DRIVER);
		device_remove_groups(dev, drv->dev_groups);
		if (dev->bus && dev->bus->remove)
			dev->bus->remove(dev);
		else if (drv->remove)
			drv->remove(dev);
		if (dev->bus && dev->bus->dma_cleanup)
			dev->bus->dma_cleanup(dev);
		lkpi_devres_release_all(dev);
		lkpi_driver_sysfs_remove(dev);
		mutex_lock(&lkpi_dev_mutex);
		list_del_init(&dev->lkpi_drv_node);
		mutex_unlock(&lkpi_dev_mutex);
		dev->driver = NULL;
		dev_set_drvdata(dev, NULL);
		lkpi_bus_notify(dev, BUS_NOTIFY_UNBOUND_DRIVER);
		kobject_uevent(&dev->kobj, KOBJ_UNBIND);
	}
	device_unlock(dev);
	if (dev->parent && dev->bus && dev->bus->need_parent_lock)
		device_unlock(dev->parent);
}
EXPORT_SYMBOL(lkpi_device_release_driver);

int lkpi_device_reprobe(struct device *dev)
{
	if (dev->driver)
		lkpi_device_release_driver(dev);
	return lkpi_device_attach_all(dev) ? 0 : -ENODEV;
}
EXPORT_SYMBOL(lkpi_device_reprobe);

/* Reintenta los probes diferidos (tras cada vinculacion con exito). */
void lkpi_deferred_probe_trigger(void)
{
	static atomic_t running = ATOMIC_INIT(0);
	struct lkpi_bus_priv *bp;
	struct lkpi_dev_snap s = { NULL, 0 };
	bool again;

	/* Una sola pasada a la vez; si otra lo pide mientras, se repite. */
	if (atomic_inc_return(&running) > 1)
		return;
	do {
		struct device *d;
		int n = 0;

		mutex_lock(&lkpi_dev_mutex);
		list_for_each_entry(bp, &lkpi_buses, node)
			list_for_each_entry(d, &bp->devices, lkpi_bus_node)
				if (d->lkpi_deferred && !d->driver)
					n++;
		s.devs = n ? kmalloc_array(n, sizeof(*s.devs), GFP_KERNEL) : NULL;
		s.n = 0;
		if (s.devs)
			list_for_each_entry(bp, &lkpi_buses, node)
				list_for_each_entry(d, &bp->devices, lkpi_bus_node)
					if (d->lkpi_deferred && !d->driver && s.n < n)
						s.devs[s.n++] = get_device(d);
		mutex_unlock(&lkpi_dev_mutex);
		for (int i = 0; i < s.n; i++) {
			s.devs[i]->lkpi_deferred = false;
			lkpi_device_attach_all(s.devs[i]);
		}
		lkpi_dev_snap_put(&s);
		again = atomic_dec_return(&running) > 0;
		if (again)
			atomic_set(&running, 1);
	} while (again);
}

void lkpi_driver_deferred_probe_check_state(struct device *dev) { }
EXPORT_SYMBOL(lkpi_driver_deferred_probe_check_state);

void lkpi_wait_for_device_probe(void) { }	/* todo es sincrono */
EXPORT_SYMBOL(lkpi_wait_for_device_probe);

/* ------------------------------------------- dispositivos en el bus */

int lkpi_bus_add_device(struct device *dev)
{
	struct lkpi_bus_priv *bp;
	int rc;

	if (!dev->bus)
		return 0;
	mutex_lock(&lkpi_dev_mutex);
	bp = lkpi_bus_priv_find(dev->bus);
	mutex_unlock(&lkpi_dev_mutex);
	if (!bp) {
		WARN(1, "device '%s' en el bus '%s' sin registrar\n", dev_name(dev),
		     dev->bus->name);
		return -EINVAL;
	}
	rc = device_add_groups(dev, dev->bus->dev_groups);
	if (rc)
		return rc;
	rc = sysfs_create_link(&dev->kobj, &bp->kobj, "subsystem");
	if (!rc && !bp->static_devices_dir)
		rc = sysfs_create_link(bp->devices_kobj, &dev->kobj, dev_name(dev));
	if (rc) {
		device_remove_groups(dev, dev->bus->dev_groups);
		return rc;
	}
	dev->lkpi_bus_link = kstrdup(dev_name(dev), GFP_KERNEL);
	mutex_lock(&lkpi_dev_mutex);
	list_add_tail(&dev->lkpi_bus_node, &bp->devices);
	mutex_unlock(&lkpi_dev_mutex);
	return 0;
}

void lkpi_bus_remove_device(struct device *dev)
{
	struct lkpi_bus_priv *bp;

	if (!dev->bus)
		return;
	mutex_lock(&lkpi_dev_mutex);
	bp = lkpi_bus_priv_find(dev->bus);
	list_del_init(&dev->lkpi_bus_node);
	mutex_unlock(&lkpi_dev_mutex);
	if (!bp)
		return;
	device_remove_groups(dev, dev->bus->dev_groups);
	if (dev->lkpi_bus_link && !bp->static_devices_dir)
		sysfs_remove_link(bp->devices_kobj, dev->lkpi_bus_link);
	sysfs_remove_link(&dev->kobj, "subsystem");
	kfree(dev->lkpi_bus_link);
	dev->lkpi_bus_link = NULL;
}

void lkpi_bus_probe_device(struct device *dev)
{
	if (dev->bus)
		lkpi_device_attach_all(dev);
}

/* ---------------------------------------------------------------- bus */

static ssize_t lkpi_bus_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct lkpi_bus_priv *bp = container_of(kobj, struct lkpi_bus_priv, kobj);
	struct bus_attribute *ba = container_of(attr, struct bus_attribute, attr);

	return ba->show ? ba->show(bp->bus, buf) : -EIO;
}

static ssize_t lkpi_bus_attr_store(struct kobject *kobj, struct attribute *attr,
				   const char *buf, size_t count)
{
	struct lkpi_bus_priv *bp = container_of(kobj, struct lkpi_bus_priv, kobj);
	struct bus_attribute *ba = container_of(attr, struct bus_attribute, attr);

	return ba->store ? ba->store(bp->bus, buf, count) : -EIO;
}

static const struct sysfs_ops lkpi_bus_sysfs_ops = {
	.show = lkpi_bus_attr_show,
	.store = lkpi_bus_attr_store,
};

static void lkpi_bus_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct lkpi_bus_priv, kobj));
}

static const struct kobj_type lkpi_bus_ktype = {
	.release = lkpi_bus_release,
	.sysfs_ops = &lkpi_bus_sysfs_ops,
};

static ssize_t drivers_autoprobe_show(const struct bus_type *bus, char *buf)
{
	return sysfs_emit(buf, "1\n");
}
static BUS_ATTR_RO(drivers_autoprobe);

int lkpi_bus_register(const struct bus_type *bus)
{
	struct lkpi_bus_priv *bp;
	int rc;

	bp = kzalloc(sizeof(*bp), GFP_KERNEL);
	if (!bp)
		return -ENOMEM;
	bp->bus = bus;
	INIT_LIST_HEAD(&bp->devices);
	INIT_LIST_HEAD(&bp->drivers);
	BLOCKING_INIT_NOTIFIER_HEAD(&bp->notifier);
	/* Aurora ya tiene /sys/bus/pci/devices: la capa no lo duplica. */
	bp->static_devices_dir = !strcmp(bus->name, "pci");

	rc = kobject_init_and_add(&bp->kobj, &lkpi_bus_ktype, lkpi_bus_kobj, "%s", bus->name);
	if (rc) {
		kobject_put(&bp->kobj);
		return rc;
	}
	bp->devices_kobj = kobject_create_and_add("devices", &bp->kobj);
	bp->drivers_kobj = kobject_create_and_add("drivers", &bp->kobj);
	if (!bp->devices_kobj || !bp->drivers_kobj) {
		rc = -ENOMEM;
		goto err;
	}
	rc = sysfs_create_file(&bp->kobj, &bus_attr_drivers_autoprobe.attr);
	if (!rc)
		rc = sysfs_create_groups(&bp->kobj, bus->bus_groups);
	if (rc)
		goto err;

	mutex_lock(&lkpi_dev_mutex);
	if (lkpi_bus_priv_find(bus)) {
		mutex_unlock(&lkpi_dev_mutex);
		rc = -EEXIST;
		goto err;
	}
	list_add_tail(&bp->node, &lkpi_buses);
	mutex_unlock(&lkpi_dev_mutex);
	pr_debug("linuxkpi: bus '%s' registrado\n", bus->name);
	return 0;

err:
	kobject_put(bp->drivers_kobj);
	kobject_put(bp->devices_kobj);
	kobject_del(&bp->kobj);
	kobject_put(&bp->kobj);
	return rc;
}
EXPORT_SYMBOL(lkpi_bus_register);

void lkpi_bus_unregister(const struct bus_type *bus)
{
	struct lkpi_bus_priv *bp;

	mutex_lock(&lkpi_dev_mutex);
	bp = lkpi_bus_priv_find(bus);
	if (bp) {
		bp->dead = true;
		list_del(&bp->node);
	}
	mutex_unlock(&lkpi_dev_mutex);
	if (!bp)
		return;
	WARN(!list_empty(&bp->devices) || !list_empty(&bp->drivers),
	     "bus_unregister(%s) con dispositivos o drivers\n", bus->name);
	sysfs_remove_groups(&bp->kobj, bus->bus_groups);
	kobject_put(bp->drivers_kobj);
	kobject_put(bp->devices_kobj);
	kobject_del(&bp->kobj);
	kobject_put(&bp->kobj);
}
EXPORT_SYMBOL(lkpi_bus_unregister);

int lkpi_bus_rescan_devices(const struct bus_type *bus)
{
	struct lkpi_bus_priv *bp;
	struct lkpi_dev_snap s = { NULL, 0 };

	mutex_lock(&lkpi_dev_mutex);
	bp = lkpi_bus_priv_find(bus);
	if (bp)
		s = lkpi_bus_snap(bp);
	mutex_unlock(&lkpi_dev_mutex);
	for (int i = 0; i < s.n; i++)
		if (!s.devs[i]->driver)
			lkpi_device_attach_all(s.devs[i]);
	lkpi_dev_snap_put(&s);
	return 0;
}
EXPORT_SYMBOL(lkpi_bus_rescan_devices);

int lkpi_bus_for_each_dev(const struct bus_type *bus, struct device *start, void *data,
			  int (*fn)(struct device *dev, void *data))
{
	struct lkpi_bus_priv *bp;
	struct lkpi_dev_snap s = { NULL, 0 };
	bool started = !start;
	int rc = 0;

	mutex_lock(&lkpi_dev_mutex);
	bp = lkpi_bus_priv_find(bus);
	if (bp)
		s = lkpi_bus_snap(bp);
	mutex_unlock(&lkpi_dev_mutex);
	if (!bp)
		return -EINVAL;
	for (int i = 0; i < s.n && !rc; i++) {
		if (!started) {
			started = s.devs[i] == start;
			continue;
		}
		rc = fn(s.devs[i], data);
	}
	lkpi_dev_snap_put(&s);
	return rc;
}
EXPORT_SYMBOL(lkpi_bus_for_each_dev);

struct device *lkpi_bus_find_device(const struct bus_type *bus, struct device *start,
				    const void *data,
				    int (*match)(struct device *dev, const void *data))
{
	struct lkpi_bus_priv *bp;
	struct lkpi_dev_snap s = { NULL, 0 };
	struct device *found = NULL;
	bool started = !start;

	mutex_lock(&lkpi_dev_mutex);
	bp = lkpi_bus_priv_find(bus);
	if (bp)
		s = lkpi_bus_snap(bp);
	mutex_unlock(&lkpi_dev_mutex);
	for (int i = 0; i < s.n && !found; i++) {
		if (!started) {
			started = s.devs[i] == start;
			continue;
		}
		if (match(s.devs[i], data))
			found = get_device(s.devs[i]);
	}
	lkpi_dev_snap_put(&s);
	return found;
}
EXPORT_SYMBOL(lkpi_bus_find_device);

int lkpi_bus_for_each_drv(const struct bus_type *bus, struct device_driver *start, void *data,
			  int (*fn)(struct device_driver *drv, void *data))
{
	struct driver_private **drvs;
	struct lkpi_bus_priv *bp;
	bool started = !start;
	int n, rc = 0;

	mutex_lock(&lkpi_dev_mutex);
	bp = lkpi_bus_priv_find(bus);
	n = bp ? lkpi_drivers_snap(bp, &drvs) : 0;
	mutex_unlock(&lkpi_dev_mutex);
	if (!bp)
		return -EINVAL;
	for (int i = 0; i < n; i++) {
		if (!rc) {
			if (!started)
				started = drvs[i]->driver == start;
			else
				rc = fn(drvs[i]->driver, data);
		}
		kobject_put(&drvs[i]->kobj);
	}
	if (n)
		kfree(drvs);
	return rc;
}
EXPORT_SYMBOL(lkpi_bus_for_each_drv);

int lkpi_bus_register_notifier(const struct bus_type *bus, struct notifier_block *nb)
{
	struct lkpi_bus_priv *bp;

	mutex_lock(&lkpi_dev_mutex);
	bp = lkpi_bus_priv_find(bus);
	mutex_unlock(&lkpi_dev_mutex);
	return bp ? blocking_notifier_chain_register(&bp->notifier, nb) : -EINVAL;
}
EXPORT_SYMBOL(lkpi_bus_register_notifier);

int lkpi_bus_unregister_notifier(const struct bus_type *bus, struct notifier_block *nb)
{
	struct lkpi_bus_priv *bp;

	mutex_lock(&lkpi_dev_mutex);
	bp = lkpi_bus_priv_find(bus);
	mutex_unlock(&lkpi_dev_mutex);
	return bp ? blocking_notifier_chain_unregister(&bp->notifier, nb) : -EINVAL;
}
EXPORT_SYMBOL(lkpi_bus_unregister_notifier);

int lkpi_bus_create_file(const struct bus_type *bus, struct bus_attribute *attr)
{
	struct lkpi_bus_priv *bp;

	mutex_lock(&lkpi_dev_mutex);
	bp = lkpi_bus_priv_find(bus);
	mutex_unlock(&lkpi_dev_mutex);
	return bp ? sysfs_create_file(&bp->kobj, &attr->attr) : -EINVAL;
}
EXPORT_SYMBOL(lkpi_bus_create_file);

void lkpi_bus_remove_file(const struct bus_type *bus, struct bus_attribute *attr)
{
	struct lkpi_bus_priv *bp;

	mutex_lock(&lkpi_dev_mutex);
	bp = lkpi_bus_priv_find(bus);
	mutex_unlock(&lkpi_dev_mutex);
	if (bp)
		sysfs_remove_file(&bp->kobj, &attr->attr);
}
EXPORT_SYMBOL(lkpi_bus_remove_file);

/* ------------------------------------------------------------ drivers */

static ssize_t lkpi_drv_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct driver_private *p = container_of(kobj, struct driver_private, kobj);
	struct driver_attribute *da = container_of(attr, struct driver_attribute, attr);

	return da->show ? da->show(p->driver, buf) : -EIO;
}

static ssize_t lkpi_drv_attr_store(struct kobject *kobj, struct attribute *attr,
				   const char *buf, size_t count)
{
	struct driver_private *p = container_of(kobj, struct driver_private, kobj);
	struct driver_attribute *da = container_of(attr, struct driver_attribute, attr);

	return da->store ? da->store(p->driver, buf, count) : -EIO;
}

static const struct sysfs_ops lkpi_drv_sysfs_ops = {
	.show = lkpi_drv_attr_show,
	.store = lkpi_drv_attr_store,
};

static void lkpi_driver_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct driver_private, kobj));
}

static const struct kobj_type lkpi_driver_ktype = {
	.release = lkpi_driver_release,
	.sysfs_ops = &lkpi_drv_sysfs_ops,
};

static ssize_t uevent_store(struct device_driver *drv, const char *buf, size_t count)
{
	return count;
}
static DRIVER_ATTR_WO(uevent);

static ssize_t unbind_store(struct device_driver *drv, const char *buf, size_t count)
{
	struct device *dev = bus_find_device_by_name(drv->bus, NULL, buf);
	int rc = -ENODEV;

	if (dev && dev->driver == drv) {
		lkpi_device_release_driver(dev);
		rc = count;
	}
	put_device(dev);
	return rc;
}
static DRIVER_ATTR_WO(unbind);

static ssize_t bind_store(struct device_driver *drv, const char *buf, size_t count)
{
	struct device *dev = bus_find_device_by_name(drv->bus, NULL, buf);
	int rc = -ENODEV;

	if (dev && !dev->driver && lkpi_try_bind(dev, drv))
		rc = count;
	put_device(dev);
	return rc;
}
static DRIVER_ATTR_WO(bind);

struct device_driver *lkpi_driver_find(const char *name, const struct bus_type *bus)
{
	struct lkpi_bus_priv *bp;
	struct driver_private *p;
	struct device_driver *found = NULL;

	mutex_lock(&lkpi_dev_mutex);
	bp = lkpi_bus_priv_find(bus);
	if (bp)
		list_for_each_entry(p, &bp->drivers, node)
			if (!p->dead && !strcmp(p->driver->name, name)) {
				found = p->driver;
				break;
			}
	mutex_unlock(&lkpi_dev_mutex);
	return found;
}
EXPORT_SYMBOL(lkpi_driver_find);

int lkpi_driver_register(struct device_driver *drv)
{
	struct driver_private *p;
	struct lkpi_bus_priv *bp;
	int rc;

	mutex_lock(&lkpi_dev_mutex);
	bp = drv->bus ? lkpi_bus_priv_find(drv->bus) : NULL;
	mutex_unlock(&lkpi_dev_mutex);
	if (!bp) {
		pr_err("linuxkpi: driver '%s': bus '%s' sin registrar\n", drv->name,
		       drv->bus ? drv->bus->name : "(ninguno)");
		return -EINVAL;
	}
	if (lkpi_driver_find(drv->name, drv->bus)) {
		pr_err("linuxkpi: driver '%s' ya registrado en '%s'\n", drv->name, drv->bus->name);
		return -EBUSY;
	}
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	INIT_LIST_HEAD(&p->devices);
	p->driver = drv;
	p->bus = bp;
	drv->p = p;
	rc = kobject_init_and_add(&p->kobj, &lkpi_driver_ktype, bp->drivers_kobj, "%s", drv->name);
	if (rc)
		goto err_put;
	rc = sysfs_create_file(&p->kobj, &driver_attr_uevent.attr);
	if (!rc && !drv->suppress_bind_attrs) {
		rc = sysfs_create_file(&p->kobj, &driver_attr_bind.attr);
		if (!rc)
			rc = sysfs_create_file(&p->kobj, &driver_attr_unbind.attr);
	}
	if (!rc)
		rc = sysfs_create_groups(&p->kobj, drv->bus->drv_groups);
	if (!rc)
		rc = sysfs_create_groups(&p->kobj, drv->groups);
	if (rc)
		goto err_del;
	mutex_lock(&lkpi_dev_mutex);
	list_add_tail(&p->node, &bp->drivers);
	mutex_unlock(&lkpi_dev_mutex);
	lkpi_driver_attach(drv);
	return 0;

err_del:
	kobject_del(&p->kobj);
err_put:
	kobject_put(&p->kobj);
	drv->p = NULL;
	return rc;
}
EXPORT_SYMBOL(lkpi_driver_register);

void lkpi_driver_unregister(struct device_driver *drv)
{
	struct driver_private *p = drv->p;
	struct lkpi_dev_snap s = { NULL, 0 };
	struct device *d;
	int n = 0;

	if (!p) {
		WARN(1, "driver_unregister(%s) sin registrar\n", drv->name);
		return;
	}
	mutex_lock(&lkpi_dev_mutex);
	p->dead = true;
	list_del(&p->node);
	list_for_each_entry(d, &p->devices, lkpi_drv_node)
		n++;
	s.devs = n ? kmalloc_array(n, sizeof(*s.devs), GFP_KERNEL) : NULL;
	if (s.devs)
		list_for_each_entry(d, &p->devices, lkpi_drv_node)
			s.devs[s.n++] = get_device(d);
	mutex_unlock(&lkpi_dev_mutex);
	/* Soltar en orden inverso de vinculacion, como Linux. */
	for (int i = s.n - 1; i >= 0; i--)
		if (s.devs[i]->driver == drv)
			lkpi_device_release_driver(s.devs[i]);
	lkpi_dev_snap_put(&s);
	sysfs_remove_groups(&p->kobj, drv->groups);
	sysfs_remove_groups(&p->kobj, drv->bus->drv_groups);
	kobject_del(&p->kobj);
	kobject_put(&p->kobj);
	drv->p = NULL;
}
EXPORT_SYMBOL(lkpi_driver_unregister);

static struct lkpi_dev_snap lkpi_driver_snap(const struct device_driver *drv)
{
	struct lkpi_dev_snap s = { NULL, 0 };
	struct device *d;
	int n = 0;

	mutex_lock(&lkpi_dev_mutex);
	if (drv->p) {
		list_for_each_entry(d, &drv->p->devices, lkpi_drv_node)
			n++;
		s.devs = n ? kmalloc_array(n, sizeof(*s.devs), GFP_KERNEL) : NULL;
		if (s.devs)
			list_for_each_entry(d, &drv->p->devices, lkpi_drv_node)
				s.devs[s.n++] = get_device(d);
	}
	mutex_unlock(&lkpi_dev_mutex);
	return s;
}

int lkpi_driver_for_each_device(struct device_driver *drv, struct device *start, void *data,
				int (*fn)(struct device *dev, void *))
{
	struct lkpi_dev_snap s = lkpi_driver_snap(drv);
	bool started = !start;
	int rc = 0;

	for (int i = 0; i < s.n && !rc; i++) {
		if (!started) {
			started = s.devs[i] == start;
			continue;
		}
		rc = fn(s.devs[i], data);
	}
	lkpi_dev_snap_put(&s);
	return rc;
}
EXPORT_SYMBOL(lkpi_driver_for_each_device);

struct device *lkpi_driver_find_device(const struct device_driver *drv, struct device *start,
				       const void *data,
				       int (*match)(struct device *dev, const void *data))
{
	struct lkpi_dev_snap s = lkpi_driver_snap(drv);
	struct device *found = NULL;
	bool started = !start;

	for (int i = 0; i < s.n && !found; i++) {
		if (!started) {
			started = s.devs[i] == start;
			continue;
		}
		if (match(s.devs[i], data))
			found = get_device(s.devs[i]);
	}
	lkpi_dev_snap_put(&s);
	return found;
}
EXPORT_SYMBOL(lkpi_driver_find_device);

int lkpi_driver_create_file(const struct device_driver *drv, const struct driver_attribute *attr)
{
	return drv->p ? sysfs_create_file(&drv->p->kobj, &attr->attr) : -EINVAL;
}
EXPORT_SYMBOL(lkpi_driver_create_file);

void lkpi_driver_remove_file(const struct device_driver *drv, const struct driver_attribute *attr)
{
	if (drv->p)
		sysfs_remove_file(&drv->p->kobj, &attr->attr);
}
EXPORT_SYMBOL(lkpi_driver_remove_file);
