// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nucleo del modelo de dispositivos (K4): struct device, atributos,
 * jerarquia en /sys/devices, clases y dev_printk.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sprintf.h>
#include <linux/kstrtox.h>
#include "base.h"
#include "../lkpi_internal.h"
#include "../fs/fs.h"

DEFINE_MUTEX(lkpi_dev_mutex);

struct kobject *lkpi_devices_kobj;
struct kobject *lkpi_bus_kobj;
struct kobject *lkpi_class_kobj;
struct kobject *lkpi_virtual_kobj;

void lkpi_dev_snap_put(struct lkpi_dev_snap *s)
{
	for (int i = 0; i < s->n; i++)
		put_device(s->devs[i]);
	kfree(s->devs);
	s->devs = NULL;
	s->n = 0;
}

/* ------------------------------------------------- ktype de los device */

static ssize_t lkpi_dev_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct device_attribute *da = container_of(attr, struct device_attribute, attr);
	struct device *dev = kobj_to_dev_nonconst(kobj);

	return da->show ? da->show(dev, da, buf) : -EIO;
}

static ssize_t lkpi_dev_attr_store(struct kobject *kobj, struct attribute *attr,
				   const char *buf, size_t count)
{
	struct device_attribute *da = container_of(attr, struct device_attribute, attr);
	struct device *dev = kobj_to_dev_nonconst(kobj);

	return da->store ? da->store(dev, da, buf, count) : -EIO;
}

static const struct sysfs_ops lkpi_dev_sysfs_ops = {
	.show = lkpi_dev_attr_show,
	.store = lkpi_dev_attr_store,
};

static void lkpi_device_release(struct kobject *kobj)
{
	struct device *dev = kobj_to_dev_nonconst(kobj);

	/* Lo que quede en devres se libera aqui, como en Linux. */
	lkpi_devres_release_all(dev);
	if (dev->release)
		dev->release(dev);
	else if (dev->type && dev->type->release)
		dev->type->release(dev);
	else if (dev->class && dev->class->dev_release)
		dev->class->dev_release(dev);
	else
		WARN(1, "device '%s' sin release(): se pierde la memoria\n", dev_name(dev));
}

static const struct kobj_type lkpi_device_ktype = {
	.release = lkpi_device_release,
	.sysfs_ops = &lkpi_dev_sysfs_ops,
};

/* ---------------------------------------------------- uevent y dev */

static ssize_t uevent_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct kobj_uevent_env *env;
	int len = 0;

	env = kzalloc(sizeof(*env), GFP_KERNEL);
	if (!env)
		return -ENOMEM;
	if (MAJOR(dev->devt)) {
		add_uevent_var(env, "MAJOR=%u", MAJOR(dev->devt));
		add_uevent_var(env, "MINOR=%u", MINOR(dev->devt));
		add_uevent_var(env, "DEVNAME=%s", dev_name(dev));
	}
	if (dev->type && dev->type->name)
		add_uevent_var(env, "DEVTYPE=%s", dev->type->name);
	if (dev->driver)
		add_uevent_var(env, "DRIVER=%s", dev->driver->name);
	if (dev->bus && dev->bus->uevent)
		dev->bus->uevent(dev, env);
	if (dev->class && dev->class->dev_uevent)
		dev->class->dev_uevent(dev, env);
	if (dev->type && dev->type->uevent)
		dev->type->uevent(dev, env);
	for (int i = 0; i < env->envp_idx; i++)
		len += sysfs_emit_at(buf, len, "%s\n", env->envp[i]);
	kfree(env);
	return len;
}

/* Escribir en uevent pediria reenviar el evento: todavia no salen (L5). */
static ssize_t uevent_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	return count;
}
static DEVICE_ATTR_RW(uevent);

static ssize_t dev_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u:%u\n", MAJOR(dev->devt), MINOR(dev->devt));
}
static DEVICE_ATTR_RO(dev);

/* --------------------------------------------------------- device */

void lkpi_device_initialize(struct device *dev)
{
	kobject_init(&dev->kobj, &lkpi_device_ktype);
	INIT_LIST_HEAD(&dev->dma_pools);
	mutex_init(&dev->mutex);
	spin_lock_init(&dev->devres_lock);
	INIT_LIST_HEAD(&dev->devres_head);
	INIT_LIST_HEAD(&dev->lkpi_bus_node);
	INIT_LIST_HEAD(&dev->lkpi_drv_node);
	INIT_LIST_HEAD(&dev->lkpi_class_node);
	INIT_LIST_HEAD(&dev->lkpi_children);
	INIT_LIST_HEAD(&dev->lkpi_child_node);
	dev->numa_node = -1;
	dev->power.disable_depth = 1;
	dev->can_match = false;
	dev->dma_coherent = true;
}
EXPORT_SYMBOL(lkpi_device_initialize);

int lkpi_dev_set_name(struct device *dev, const char *fmt, ...)
{
	va_list vargs;
	int rc;

	va_start(vargs, fmt);
	rc = kobject_set_name_vargs(&dev->kobj, fmt, vargs);
	va_end(vargs);
	return rc;
}
EXPORT_SYMBOL(lkpi_dev_set_name);

static int lkpi_device_add_attrs(struct device *dev)
{
	const struct class *cls = dev->class;
	const struct device_type *type = dev->type;
	int rc;

	if (cls) {
		rc = device_add_groups(dev, cls->dev_groups);
		if (rc)
			return rc;
	}
	if (type) {
		rc = device_add_groups(dev, type->groups);
		if (rc)
			goto err_class;
	}
	rc = device_add_groups(dev, dev->groups);
	if (rc)
		goto err_type;
	return 0;

err_type:
	if (type)
		device_remove_groups(dev, type->groups);
err_class:
	if (cls)
		device_remove_groups(dev, cls->dev_groups);
	return rc;
}

static void lkpi_device_remove_attrs(struct device *dev)
{
	device_remove_groups(dev, dev->groups);
	if (dev->type)
		device_remove_groups(dev, dev->type->groups);
	if (dev->class)
		device_remove_groups(dev, dev->class->dev_groups);
}

/* Directorio padre en /sys: el del padre, el de la clase o /sys/devices. */
static struct kobject *lkpi_device_parent_kobj(struct device *dev)
{
	if (dev->parent)
		return &dev->parent->kobj;
	if (dev->class)
		return lkpi_class_virtual_dir(dev->class);
	return lkpi_devices_kobj;
}

int lkpi_device_add(struct device *dev)
{
	struct device *parent;
	struct kobject *pkobj;
	int rc;

	dev = get_device(dev);
	if (!dev)
		return -EINVAL;

	if (dev->init_name) {
		rc = dev_set_name(dev, "%s", dev->init_name);
		dev->init_name = NULL;
		if (rc)
			goto err_put;
	}
	if (!dev_name(dev) && dev->bus && dev->bus->dev_name) {
		rc = dev_set_name(dev, "%s%u", dev->bus->dev_name, dev->id);
		if (rc)
			goto err_put;
	}
	if (!dev_name(dev)) {
		rc = -EINVAL;
		goto err_put;
	}

	parent = get_device(dev->parent);
	pkobj = lkpi_device_parent_kobj(dev);
	if (!pkobj) {
		rc = -ENOMEM;
		goto err_parent;
	}
	rc = kobject_add(&dev->kobj, pkobj, "%s", dev_name(dev));
	if (rc)
		goto err_parent;

	rc = device_create_file(dev, &dev_attr_uevent);
	if (rc)
		goto err_kobj;
	if (MAJOR(dev->devt)) {
		rc = device_create_file(dev, &dev_attr_dev);
		if (rc)
			goto err_kobj;
	}
	rc = lkpi_device_add_attrs(dev);
	if (rc)
		goto err_kobj;
	rc = lkpi_bus_add_device(dev);
	if (rc)
		goto err_attrs;
	rc = lkpi_class_add_device(dev);
	if (rc)
		goto err_bus;

	if (parent) {
		mutex_lock(&lkpi_dev_mutex);
		list_add_tail(&dev->lkpi_child_node, &parent->lkpi_children);
		mutex_unlock(&lkpi_dev_mutex);
	}
	if (MAJOR(dev->devt))
		lkpi_devnode_add(dev);	/* /dev/<nombre> (K8, L4) */
	lkpi_bus_notify(dev, BUS_NOTIFY_ADD_DEVICE);
	kobject_uevent(&dev->kobj, KOBJ_ADD);
	lkpi_bus_probe_device(dev);
	put_device(dev);
	return 0;

err_bus:
	lkpi_bus_remove_device(dev);
err_attrs:
	lkpi_device_remove_attrs(dev);
err_kobj:
	kobject_del(&dev->kobj);
err_parent:
	put_device(parent);
err_put:
	put_device(dev);
	return rc;
}
EXPORT_SYMBOL(lkpi_device_add);

int lkpi_device_register(struct device *dev)
{
	device_initialize(dev);
	return device_add(dev);
}
EXPORT_SYMBOL(lkpi_device_register);

void lkpi_device_del(struct device *dev)
{
	struct device *parent = dev->parent;

	if (!dev->kobj.state_in_sysfs) {
		WARN(1, "device_del de '%s' sin anadir\n", dev_name(dev));
		return;
	}
	lkpi_bus_notify(dev, BUS_NOTIFY_DEL_DEVICE);
	if (MAJOR(dev->devt))
		lkpi_devnode_del(dev);
	lkpi_device_release_driver(dev);
	if (parent) {
		mutex_lock(&lkpi_dev_mutex);
		list_del_init(&dev->lkpi_child_node);
		mutex_unlock(&lkpi_dev_mutex);
	}
	lkpi_class_remove_device(dev);
	lkpi_device_remove_attrs(dev);
	lkpi_bus_remove_device(dev);
	kobject_uevent(&dev->kobj, KOBJ_REMOVE);
	kobject_del(&dev->kobj);
	lkpi_bus_notify(dev, BUS_NOTIFY_REMOVED_DEVICE);
	put_device(parent);
}
EXPORT_SYMBOL(lkpi_device_del);

void lkpi_device_unregister(struct device *dev)
{
	device_del(dev);
	put_device(dev);
}
EXPORT_SYMBOL(lkpi_device_unregister);

struct device *lkpi_get_device(struct device *dev)
{
	return dev ? kobj_to_dev_nonconst(kobject_get(&dev->kobj)) : NULL;
}
EXPORT_SYMBOL(lkpi_get_device);

void lkpi_put_device(struct device *dev)
{
	if (dev)
		kobject_put(&dev->kobj);
}
EXPORT_SYMBOL(lkpi_put_device);

int lkpi_device_rename(struct device *dev, const char *new_name)
{
	return kobject_rename(&dev->kobj, new_name);
}
EXPORT_SYMBOL(lkpi_device_rename);

const char *lkpi_dev_driver_string(const struct device *dev)
{
	struct device_driver *drv = READ_ONCE(dev->driver);

	if (drv)
		return drv->name;
	if (dev->bus)
		return dev->bus->name;
	if (dev->class)
		return dev->class->name;
	return "";
}
EXPORT_SYMBOL(lkpi_dev_driver_string);

/* Hijos con referencia, en orden de alta (o inverso). */
static struct lkpi_dev_snap lkpi_children_snap(struct device *parent)
{
	struct lkpi_dev_snap s = { NULL, 0 };
	struct device *d;
	int n = 0;

	mutex_lock(&lkpi_dev_mutex);
	list_for_each_entry(d, &parent->lkpi_children, lkpi_child_node)
		n++;
	s.devs = n ? kmalloc_array(n, sizeof(*s.devs), GFP_KERNEL) : NULL;
	if (s.devs)
		list_for_each_entry(d, &parent->lkpi_children, lkpi_child_node)
			s.devs[s.n++] = get_device(d);
	mutex_unlock(&lkpi_dev_mutex);
	return s;
}

int lkpi_device_for_each_child(struct device *parent, void *data,
			       int (*fn)(struct device *dev, void *data))
{
	struct lkpi_dev_snap s = lkpi_children_snap(parent);
	int rc = 0;

	for (int i = 0; i < s.n && !rc; i++)
		rc = fn(s.devs[i], data);
	lkpi_dev_snap_put(&s);
	return rc;
}
EXPORT_SYMBOL(lkpi_device_for_each_child);

int lkpi_device_for_each_child_reverse(struct device *parent, void *data,
				       int (*fn)(struct device *dev, void *data))
{
	struct lkpi_dev_snap s = lkpi_children_snap(parent);
	int rc = 0;

	for (int i = s.n - 1; i >= 0 && !rc; i--)
		rc = fn(s.devs[i], data);
	lkpi_dev_snap_put(&s);
	return rc;
}
EXPORT_SYMBOL(lkpi_device_for_each_child_reverse);

struct device *lkpi_device_find_child(struct device *parent, const void *data,
				      int (*match)(struct device *dev, const void *data))
{
	struct lkpi_dev_snap s = lkpi_children_snap(parent);
	struct device *found = NULL;

	for (int i = 0; i < s.n && !found; i++)
		if (match(s.devs[i], data))
			found = get_device(s.devs[i]);
	lkpi_dev_snap_put(&s);
	return found;
}
EXPORT_SYMBOL(lkpi_device_find_child);

struct device *lkpi_device_find_child_by_name(struct device *parent, const char *name)
{
	return device_find_child(parent, name, lkpi_device_match_name);
}
EXPORT_SYMBOL(lkpi_device_find_child_by_name);

int lkpi_device_match_name(struct device *dev, const void *name)
{
	return dev_name(dev) && sysfs_streq(dev_name(dev), name);
}
EXPORT_SYMBOL(lkpi_device_match_name);

int lkpi_device_match_devt(struct device *dev, const void *pdevt)
{
	return dev->devt == *(const dev_t *)pdevt;
}
EXPORT_SYMBOL(lkpi_device_match_devt);

int lkpi_device_match_any(struct device *dev, const void *unused)
{
	return 1;
}
EXPORT_SYMBOL(lkpi_device_match_any);

int lkpi_device_create_file(struct device *dev, const struct device_attribute *attr)
{
	if (!dev)
		return -EINVAL;
	WARN((attr->attr.mode & 0444) && !attr->show,
	     "atributo %s legible sin show\n", attr->attr.name);
	WARN((attr->attr.mode & 0222) && !attr->store,
	     "atributo %s escribible sin store\n", attr->attr.name);
	return sysfs_create_file(&dev->kobj, &attr->attr);
}
EXPORT_SYMBOL(lkpi_device_create_file);

void lkpi_device_remove_file(struct device *dev, const struct device_attribute *attr)
{
	if (dev)
		sysfs_remove_file(&dev->kobj, &attr->attr);
}
EXPORT_SYMBOL(lkpi_device_remove_file);

/* Desde su propio store() se interbloquearia (quitar espera a las llamadas). */
bool lkpi_device_remove_file_self(struct device *dev, const struct device_attribute *attr)
{
	WARN(1, "device_remove_file_self(%s) no soportado\n", attr->attr.name);
	return false;
}
EXPORT_SYMBOL(lkpi_device_remove_file_self);

int lkpi_device_create_bin_file(struct device *dev, const struct bin_attribute *attr)
{
	return dev ? sysfs_create_bin_file(&dev->kobj, attr) : -EINVAL;
}
EXPORT_SYMBOL(lkpi_device_create_bin_file);

void lkpi_device_remove_bin_file(struct device *dev, const struct bin_attribute *attr)
{
	if (dev)
		sysfs_remove_bin_file(&dev->kobj, attr);
}
EXPORT_SYMBOL(lkpi_device_remove_bin_file);

int lkpi_device_add_groups(struct device *dev, const struct attribute_group **groups)
{
	return sysfs_create_groups(&dev->kobj, groups);
}
EXPORT_SYMBOL(lkpi_device_add_groups);

void lkpi_device_remove_groups(struct device *dev, const struct attribute_group **groups)
{
	sysfs_remove_groups(&dev->kobj, groups);
}
EXPORT_SYMBOL(lkpi_device_remove_groups);

struct lkpi_groups_devres {
	const struct attribute_group *group;
};

static void lkpi_devm_group_release(struct device *dev, void *res)
{
	struct lkpi_groups_devres *d = res;

	device_remove_group(dev, d->group);
}

int lkpi_devm_device_add_group(struct device *dev, const struct attribute_group *grp)
{
	struct lkpi_groups_devres *d;
	int rc;

	d = devres_alloc(lkpi_devm_group_release, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	rc = device_add_group(dev, grp);
	if (rc) {
		devres_free(d);
		return rc;
	}
	d->group = grp;
	devres_add(dev, d);
	return 0;
}
EXPORT_SYMBOL(lkpi_devm_device_add_group);

/* Atributos de dev_ext_attribute (DEVICE_ULONG_ATTR y compania). */
ssize_t lkpi_device_show_ulong(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct dev_ext_attribute *ea = container_of(attr, struct dev_ext_attribute, attr);

	return sysfs_emit(buf, "%lx\n", *(unsigned long *)ea->var);
}
EXPORT_SYMBOL(lkpi_device_show_ulong);

ssize_t lkpi_device_store_ulong(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t size)
{
	struct dev_ext_attribute *ea = container_of(attr, struct dev_ext_attribute, attr);
	int rc = kstrtoul(buf, 0, (unsigned long *)ea->var);

	return rc ? rc : size;
}
EXPORT_SYMBOL(lkpi_device_store_ulong);

ssize_t lkpi_device_show_int(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct dev_ext_attribute *ea = container_of(attr, struct dev_ext_attribute, attr);

	return sysfs_emit(buf, "%d\n", *(int *)ea->var);
}
EXPORT_SYMBOL(lkpi_device_show_int);

ssize_t lkpi_device_store_int(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t size)
{
	struct dev_ext_attribute *ea = container_of(attr, struct dev_ext_attribute, attr);
	int rc = kstrtoint(buf, 0, (int *)ea->var);

	return rc ? rc : size;
}
EXPORT_SYMBOL(lkpi_device_store_int);

ssize_t lkpi_device_show_bool(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct dev_ext_attribute *ea = container_of(attr, struct dev_ext_attribute, attr);

	return sysfs_emit(buf, "%d\n", *(bool *)ea->var);
}
EXPORT_SYMBOL(lkpi_device_show_bool);

ssize_t lkpi_device_store_bool(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t size)
{
	struct dev_ext_attribute *ea = container_of(attr, struct dev_ext_attribute, attr);
	int rc = kstrtobool(buf, (bool *)ea->var);

	return rc ? rc : size;
}
EXPORT_SYMBOL(lkpi_device_store_bool);

/* ------------------------------------------------ dispositivos raiz */

static void lkpi_root_device_release(struct device *dev)
{
	kfree(dev);
}

struct device *lkpi_root_device_register(const char *name)
{
	struct device *root = kzalloc(sizeof(*root), GFP_KERNEL);
	int rc;

	if (!root)
		return ERR_PTR(-ENOMEM);
	device_initialize(root);
	rc = dev_set_name(root, "%s", name);
	if (!rc) {
		root->release = lkpi_root_device_release;
		rc = device_add(root);
	}
	if (rc) {
		put_device(root);
		return ERR_PTR(rc);
	}
	return root;
}
EXPORT_SYMBOL(lkpi_root_device_register);

void lkpi_root_device_unregister(struct device *root)
{
	device_unregister(root);
}
EXPORT_SYMBOL(lkpi_root_device_unregister);

/* ------------------------------------------ device_create (clases) */

static void lkpi_created_device_release(struct device *dev)
{
	kfree(dev);
}

static struct device *lkpi_device_create_vargs(const struct class *cls, struct device *parent,
					       dev_t devt, void *drvdata,
					       const struct attribute_group **groups,
					       const char *fmt, va_list args)
{
	struct device *dev;
	int rc;

	if (IS_ERR_OR_NULL(cls))
		return ERR_PTR(-ENODEV);
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return ERR_PTR(-ENOMEM);
	device_initialize(dev);
	dev->devt = devt;
	dev->class = cls;
	dev->parent = parent;
	dev->groups = groups;
	dev->release = lkpi_created_device_release;
	dev_set_drvdata(dev, drvdata);
	rc = kobject_set_name_vargs(&dev->kobj, fmt, args);
	if (!rc)
		rc = device_add(dev);
	if (rc) {
		put_device(dev);
		return ERR_PTR(rc);
	}
	return dev;
}

struct device *lkpi_device_create(const struct class *cls, struct device *parent, dev_t devt,
				  void *drvdata, const char *fmt, ...)
{
	va_list vargs;
	struct device *dev;

	va_start(vargs, fmt);
	dev = lkpi_device_create_vargs(cls, parent, devt, drvdata, NULL, fmt, vargs);
	va_end(vargs);
	return dev;
}
EXPORT_SYMBOL(lkpi_device_create);

struct device *lkpi_device_create_with_groups(const struct class *cls, struct device *parent,
					      dev_t devt, void *drvdata,
					      const struct attribute_group **groups,
					      const char *fmt, ...)
{
	va_list vargs;
	struct device *dev;

	va_start(vargs, fmt);
	dev = lkpi_device_create_vargs(cls, parent, devt, drvdata, groups, fmt, vargs);
	va_end(vargs);
	return dev;
}
EXPORT_SYMBOL(lkpi_device_create_with_groups);

void lkpi_device_destroy(const struct class *cls, dev_t devt)
{
	struct device *dev = class_find_device_by_devt(cls, devt);

	if (dev) {
		put_device(dev);
		device_unregister(dev);
	}
}
EXPORT_SYMBOL(lkpi_device_destroy);

/* ------------------------------------------------------ device_link */

struct device_link *lkpi_device_link_add(struct device *consumer, struct device *supplier,
					 u32 flags)
{
	struct device_link *link;

	if (!consumer || !supplier || consumer == supplier)
		return NULL;
	link = kzalloc(sizeof(*link), GFP_KERNEL);
	if (!link)
		return NULL;
	link->consumer = get_device(consumer);
	link->supplier = get_device(supplier);
	link->flags = flags;
	dev_dbg(consumer, "device_link con %s (sin orden de probe/energia)\n",
		dev_name(supplier));
	return link;
}
EXPORT_SYMBOL(lkpi_device_link_add);

void lkpi_device_link_del(struct device_link *link)
{
	if (!link)
		return;
	put_device(link->consumer);
	put_device(link->supplier);
	kfree(link);
}
EXPORT_SYMBOL(lkpi_device_link_del);

/* ------------------------------------------------------- dev_printk */

/* El nivel (KERN_*) tiene que ir al principio del formato, no como argumento. */
static void lkpi_dev_vprintk_emit(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	lkpi_vprintk(fmt, args);
	va_end(args);
}

void lkpi_dev_printk(const char *level, const struct device *dev, const char *fmt, ...)
{
	char f[24] = "";
	struct va_format vaf;
	va_list args;
	size_t l = level ? strnlen(level, 4) : 0;

	memcpy(f, level, l);
	strcpy(f + l, dev ? "%s %s: %pV" : "(NULL device *): %pV");
	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (dev)
		lkpi_dev_vprintk_emit(f, dev_driver_string(dev), dev_name(dev), &vaf);
	else
		lkpi_dev_vprintk_emit(f, &vaf);
	va_end(args);
}
EXPORT_SYMBOL(lkpi_dev_printk);

int lkpi_dev_err_probe(const struct device *dev, int err, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (err != -EPROBE_DEFER && err != -ENOMEM)
		dev_err(dev, "error %pe: %pV", ERR_PTR(err), &vaf);
	else
		dev_dbg(dev, "error %pe: %pV", ERR_PTR(err), &vaf);
	va_end(args);
	return err;
}
EXPORT_SYMBOL(lkpi_dev_err_probe);

/* ------------------------------------------------------------ clases */

struct lkpi_class_priv {
	struct kobject kobj;		/* class/<nombre> */
	struct list_head node;
	const struct class *cls;
	struct kobject *virtual_dir;	/* devices/virtual/<nombre> */
	struct list_head devices;	/* device.lkpi_class_node */
	bool dynamic;			/* de class_create: se libera con el */
};

static LIST_HEAD(lkpi_classes);

static struct lkpi_class_priv *lkpi_class_priv_find_locked(const struct class *cls)
{
	struct lkpi_class_priv *cp;

	list_for_each_entry(cp, &lkpi_classes, node)
		if (cp->cls == cls)
			return cp;
	return NULL;
}

static ssize_t lkpi_class_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct lkpi_class_priv *cp = container_of(kobj, struct lkpi_class_priv, kobj);
	struct class_attribute *ca = container_of(attr, struct class_attribute, attr);

	return ca->show ? ca->show(cp->cls, ca, buf) : -EIO;
}

static ssize_t lkpi_class_attr_store(struct kobject *kobj, struct attribute *attr,
				     const char *buf, size_t count)
{
	struct lkpi_class_priv *cp = container_of(kobj, struct lkpi_class_priv, kobj);
	struct class_attribute *ca = container_of(attr, struct class_attribute, attr);

	return ca->store ? ca->store(cp->cls, ca, buf, count) : -EIO;
}

static const struct sysfs_ops lkpi_class_sysfs_ops = {
	.show = lkpi_class_attr_show,
	.store = lkpi_class_attr_store,
};

static void lkpi_class_release(struct kobject *kobj)
{
	struct lkpi_class_priv *cp = container_of(kobj, struct lkpi_class_priv, kobj);
	const struct class *cls = cp->cls;

	if (cls->class_release)
		cls->class_release(cls);
	if (cp->dynamic)
		kfree(cls);
	kfree(cp);
}

static const struct kobj_type lkpi_class_ktype = {
	.release = lkpi_class_release,
	.sysfs_ops = &lkpi_class_sysfs_ops,
};

int lkpi_class_register(const struct class *cls)
{
	struct lkpi_class_priv *cp;
	int rc;

	cp = kzalloc(sizeof(*cp), GFP_KERNEL);
	if (!cp)
		return -ENOMEM;
	cp->cls = cls;
	INIT_LIST_HEAD(&cp->devices);
	mutex_lock(&lkpi_dev_mutex);
	if (lkpi_class_priv_find_locked(cls)) {
		mutex_unlock(&lkpi_dev_mutex);
		kfree(cp);
		return -EEXIST;
	}
	list_add_tail(&cp->node, &lkpi_classes);
	mutex_unlock(&lkpi_dev_mutex);

	rc = kobject_init_and_add(&cp->kobj, &lkpi_class_ktype, lkpi_class_kobj, "%s", cls->name);
	if (!rc)
		rc = sysfs_create_groups(&cp->kobj, cls->class_groups);
	if (rc) {
		mutex_lock(&lkpi_dev_mutex);
		list_del(&cp->node);
		mutex_unlock(&lkpi_dev_mutex);
		cp->dynamic = false;
		kobject_put(&cp->kobj);
	}
	return rc;
}
EXPORT_SYMBOL(lkpi_class_register);

/* Devuelve la clase: la quita de la lista, pero el kobject la mantiene viva. */
void lkpi_class_unregister(const struct class *cls)
{
	struct lkpi_class_priv *cp;

	mutex_lock(&lkpi_dev_mutex);
	cp = lkpi_class_priv_find_locked(cls);
	if (cp)
		list_del(&cp->node);
	mutex_unlock(&lkpi_dev_mutex);
	if (!cp)
		return;
	WARN(!list_empty(&cp->devices), "class_unregister(%s) con dispositivos\n", cls->name);
	sysfs_remove_groups(&cp->kobj, cls->class_groups);
	if (cp->virtual_dir)
		kobject_put(cp->virtual_dir);
	kobject_del(&cp->kobj);
	kobject_put(&cp->kobj);
}
EXPORT_SYMBOL(lkpi_class_unregister);

struct class *lkpi_class_create(const char *name)
{
	struct lkpi_class_priv *cp;
	struct class *cls;
	int rc;

	cls = kzalloc(sizeof(*cls), GFP_KERNEL);
	if (!cls)
		return ERR_PTR(-ENOMEM);
	cls->name = name;
	rc = class_register(cls);
	if (rc) {
		kfree(cls);
		return ERR_PTR(rc);
	}
	mutex_lock(&lkpi_dev_mutex);
	cp = lkpi_class_priv_find_locked(cls);
	if (cp)
		cp->dynamic = true;
	mutex_unlock(&lkpi_dev_mutex);
	return cls;
}
EXPORT_SYMBOL(lkpi_class_create);

void lkpi_class_destroy(const struct class *cls)
{
	if (!IS_ERR_OR_NULL(cls))
		class_unregister(cls);
}
EXPORT_SYMBOL(lkpi_class_destroy);

bool lkpi_class_is_registered(const struct class *cls)
{
	bool r;

	mutex_lock(&lkpi_dev_mutex);
	r = lkpi_class_priv_find_locked(cls) != NULL;
	mutex_unlock(&lkpi_dev_mutex);
	return r;
}
EXPORT_SYMBOL(lkpi_class_is_registered);

struct kobject *lkpi_class_virtual_dir(const struct class *cls)
{
	struct lkpi_class_priv *cp;
	struct kobject *dir = NULL;

	mutex_lock(&lkpi_dev_mutex);
	cp = lkpi_class_priv_find_locked(cls);
	if (cp && !cp->virtual_dir)
		cp->virtual_dir = kobject_create_and_add(cls->name, lkpi_virtual_kobj);
	if (cp)
		dir = cp->virtual_dir;
	mutex_unlock(&lkpi_dev_mutex);
	return dir;
}

int lkpi_class_add_device(struct device *dev)
{
	struct lkpi_class_priv *cp;
	int rc;

	if (!dev->class)
		return 0;
	mutex_lock(&lkpi_dev_mutex);
	cp = lkpi_class_priv_find_locked(dev->class);
	if (cp)
		list_add_tail(&dev->lkpi_class_node, &cp->devices);
	mutex_unlock(&lkpi_dev_mutex);
	if (!cp) {
		WARN(1, "device '%s' de una clase no registrada\n", dev_name(dev));
		return -EINVAL;
	}
	rc = sysfs_create_link(&dev->kobj, &cp->kobj, "subsystem");
	if (!rc)
		rc = sysfs_create_link(&cp->kobj, &dev->kobj, dev_name(dev));
	if (!rc)
		dev->lkpi_class_link = kasprintf(GFP_KERNEL, "%s", dev_name(dev));
	if (rc) {
		mutex_lock(&lkpi_dev_mutex);
		list_del_init(&dev->lkpi_class_node);
		mutex_unlock(&lkpi_dev_mutex);
	}
	return rc;
}

void lkpi_class_remove_device(struct device *dev)
{
	struct lkpi_class_priv *cp;

	if (!dev->class)
		return;
	mutex_lock(&lkpi_dev_mutex);
	cp = lkpi_class_priv_find_locked(dev->class);
	list_del_init(&dev->lkpi_class_node);
	mutex_unlock(&lkpi_dev_mutex);
	if (cp && dev->lkpi_class_link)
		sysfs_remove_link(&cp->kobj, dev->lkpi_class_link);
	kfree(dev->lkpi_class_link);
	dev->lkpi_class_link = NULL;
}

static struct lkpi_dev_snap lkpi_class_snap(const struct class *cls)
{
	struct lkpi_dev_snap s = { NULL, 0 };
	struct lkpi_class_priv *cp;
	struct device *d;
	int n = 0;

	mutex_lock(&lkpi_dev_mutex);
	cp = lkpi_class_priv_find_locked(cls);
	if (cp) {
		list_for_each_entry(d, &cp->devices, lkpi_class_node)
			n++;
		s.devs = n ? kmalloc_array(n, sizeof(*s.devs), GFP_KERNEL) : NULL;
		if (s.devs)
			list_for_each_entry(d, &cp->devices, lkpi_class_node)
				s.devs[s.n++] = get_device(d);
	}
	mutex_unlock(&lkpi_dev_mutex);
	return s;
}

int lkpi_class_for_each_device(const struct class *cls, const struct device *start, void *data,
			       int (*fn)(struct device *dev, void *data))
{
	struct lkpi_dev_snap s = lkpi_class_snap(cls);
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
EXPORT_SYMBOL(lkpi_class_for_each_device);

struct device *lkpi_class_find_device(const struct class *cls, const struct device *start,
				      const void *data,
				      int (*match)(struct device *, const void *))
{
	struct lkpi_dev_snap s = lkpi_class_snap(cls);
	bool started = !start;
	struct device *found = NULL;

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
EXPORT_SYMBOL(lkpi_class_find_device);

int lkpi_class_create_file_ns(const struct class *cls, const struct class_attribute *attr,
			      const void *ns)
{
	struct lkpi_class_priv *cp;

	mutex_lock(&lkpi_dev_mutex);
	cp = lkpi_class_priv_find_locked(cls);
	mutex_unlock(&lkpi_dev_mutex);
	return cp ? sysfs_create_file(&cp->kobj, &attr->attr) : -EINVAL;
}
EXPORT_SYMBOL(lkpi_class_create_file_ns);

void lkpi_class_remove_file_ns(const struct class *cls, const struct class_attribute *attr,
			       const void *ns)
{
	struct lkpi_class_priv *cp;

	mutex_lock(&lkpi_dev_mutex);
	cp = lkpi_class_priv_find_locked(cls);
	mutex_unlock(&lkpi_dev_mutex);
	if (cp)
		sysfs_remove_file(&cp->kobj, &attr->attr);
}
EXPORT_SYMBOL(lkpi_class_remove_file_ns);

/* ----------------------------------------------------------- arranque */

int lkpi_device_core_init(void)
{
	int rc = lkpi_kobject_init_layer();

	if (rc)
		return rc;
	lkpi_devices_kobj = kobject_create_and_add("devices", NULL);
	lkpi_bus_kobj = kobject_create_and_add("bus", NULL);
	lkpi_class_kobj = kobject_create_and_add("class", NULL);
	lkpi_virtual_kobj = lkpi_devices_kobj ?
		kobject_create_and_add("virtual", lkpi_devices_kobj) : NULL;
	if (!lkpi_devices_kobj || !lkpi_bus_kobj || !lkpi_class_kobj || !lkpi_virtual_kobj) {
		lkpi_device_core_exit();
		return -ENOMEM;
	}
	return 0;
}

void lkpi_device_core_exit(void)
{
	kobject_put(lkpi_virtual_kobj);
	kobject_put(lkpi_class_kobj);
	kobject_put(lkpi_bus_kobj);
	kobject_put(lkpi_devices_kobj);
	lkpi_virtual_kobj = lkpi_class_kobj = lkpi_bus_kobj = lkpi_devices_kobj = NULL;
	lkpi_kobject_exit_layer();
}

ssize_t lkpi_show_class_attr_string(const struct class *class, const struct class_attribute *attr,
				    char *buf)
{
	const struct class_attribute_string *cs = container_of(attr, struct class_attribute_string, attr);

	return sysfs_emit(buf, "%s\n", cs->str);
}
EXPORT_SYMBOL(lkpi_show_class_attr_string);
