// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dispositivos de caracteres (K8, L4): regiones de dev_t, cdev, nodos de
 * /dev y misc.
 *
 * Como en Linux, cdev_add() solo apunta el rango de dev_t y el nodo de /dev
 * sale de device_add() de un device con devt (devtmpfs). Aqui ese nodo se
 * registra en el devtmpfs de Aurora con lkpi_file_vops y un origen
 * (lkpi_devnode) en priv: al abrir se busca el cdev del dev_t y se crea el
 * struct file. Los origenes de nodos ya borrados quedan marcados (open da
 * -ENODEV) hasta descargar la capa, por si Aurora aun tenia un lookup suyo.
 *
 * El st_rdev de Aurora usa la codificacion de 32 bits de Linux (new_encode_dev).
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/miscdevice.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sprintf.h>

#include "../aurora.h"
#include "../lkpi_internal.h"
#include "fs.h"

static DEFINE_MUTEX(lkpi_chrdev_mutex);

/* ------------------------------------------------- regiones de dev_t */

struct lkpi_chrdev_region {
	struct list_head list;
	unsigned int major, baseminor, count;
	char name[32];
	struct cdev *cdev;		/* __register_chrdev */
};
static LIST_HEAD(lkpi_chrdev_regions);

static bool lkpi_region_busy(unsigned int major, unsigned int base, unsigned int count)
{
	struct lkpi_chrdev_region *r;

	list_for_each_entry(r, &lkpi_chrdev_regions, list)
		if (r->major == major && base < r->baseminor + r->count &&
		    r->baseminor < base + count)
			return true;
	return false;
}

static bool lkpi_major_used(unsigned int major)
{
	struct lkpi_chrdev_region *r;

	list_for_each_entry(r, &lkpi_chrdev_regions, list)
		if (r->major == major)
			return true;
	return false;
}

/* major 0: dinamico, de 254 a 234 y luego de 511 a 384 (como Linux). */
static struct lkpi_chrdev_region *lkpi_region_add(unsigned int major, unsigned int baseminor,
						  unsigned int count, const char *name)
{
	struct lkpi_chrdev_region *r;

	if (!count || baseminor + count - 1 > MINORMASK || major >= CHRDEV_MAJOR_MAX)
		return ERR_PTR(-EINVAL);
	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return ERR_PTR(-ENOMEM);
	mutex_lock(&lkpi_chrdev_mutex);
	if (!major) {
		unsigned int m;

		for (m = 254; m >= CHRDEV_MAJOR_DYN_END && lkpi_major_used(m); m--)
			;
		if (m < CHRDEV_MAJOR_DYN_END)
			for (m = 511; m >= 384 && lkpi_major_used(m); m--)
				;
		if (m < 384 && (m < CHRDEV_MAJOR_DYN_END || m > 254)) {
			mutex_unlock(&lkpi_chrdev_mutex);
			kfree(r);
			return ERR_PTR(-EBUSY);
		}
		major = m;
	} else if (lkpi_region_busy(major, baseminor, count)) {
		mutex_unlock(&lkpi_chrdev_mutex);
		kfree(r);
		return ERR_PTR(-EBUSY);
	}
	r->major = major;
	r->baseminor = baseminor;
	r->count = count;
	strscpy(r->name, name ? name : "", sizeof(r->name));
	list_add_tail(&r->list, &lkpi_chrdev_regions);
	mutex_unlock(&lkpi_chrdev_mutex);
	return r;
}

static struct lkpi_chrdev_region *lkpi_region_del(unsigned int major, unsigned int baseminor,
						  unsigned int count)
{
	struct lkpi_chrdev_region *r;

	mutex_lock(&lkpi_chrdev_mutex);
	list_for_each_entry(r, &lkpi_chrdev_regions, list)
		if (r->major == major && r->baseminor == baseminor && r->count == count) {
			list_del(&r->list);
			mutex_unlock(&lkpi_chrdev_mutex);
			return r;
		}
	mutex_unlock(&lkpi_chrdev_mutex);
	return NULL;
}

int lkpi_alloc_chrdev_region(dev_t *dev, unsigned int baseminor, unsigned int count,
			     const char *name)
{
	struct lkpi_chrdev_region *r = lkpi_region_add(0, baseminor, count, name);

	if (IS_ERR(r))
		return PTR_ERR(r);
	*dev = MKDEV(r->major, r->baseminor);
	return 0;
}
EXPORT_SYMBOL(lkpi_alloc_chrdev_region);

int lkpi_register_chrdev_region(dev_t from, unsigned int count, const char *name)
{
	struct lkpi_chrdev_region *r = lkpi_region_add(MAJOR(from), MINOR(from), count, name);

	return IS_ERR(r) ? PTR_ERR(r) : 0;
}
EXPORT_SYMBOL(lkpi_register_chrdev_region);

void lkpi_unregister_chrdev_region(dev_t from, unsigned int count)
{
	kfree(lkpi_region_del(MAJOR(from), MINOR(from), count));
}
EXPORT_SYMBOL(lkpi_unregister_chrdev_region);

int lkpi___register_chrdev(unsigned int major, unsigned int baseminor, unsigned int count,
			   const char *name, const struct file_operations *fops)
{
	struct lkpi_chrdev_region *r = lkpi_region_add(major, baseminor, count, name);
	struct cdev *cdev;
	int rc;

	if (IS_ERR(r))
		return PTR_ERR(r);
	cdev = cdev_alloc();
	if (!cdev) {
		rc = -ENOMEM;
		goto err;
	}
	cdev->owner = fops->owner;
	cdev->ops = fops;
	rc = cdev_add(cdev, MKDEV(r->major, baseminor), count);
	if (rc) {
		kobject_put(&cdev->kobj);
		goto err;
	}
	r->cdev = cdev;
	return major ? 0 : r->major;
err:
	kfree(lkpi_region_del(r->major, baseminor, count));
	return rc;
}
EXPORT_SYMBOL(lkpi___register_chrdev);

void lkpi___unregister_chrdev(unsigned int major, unsigned int baseminor, unsigned int count,
			      const char *name)
{
	struct lkpi_chrdev_region *r = lkpi_region_del(major, baseminor, count);

	if (r && r->cdev)
		cdev_del(r->cdev);
	kfree(r);
}
EXPORT_SYMBOL(lkpi___unregister_chrdev);

/* ---------------------------------------------------------------- cdev */

static LIST_HEAD(lkpi_cdevs);

static void lkpi_cdev_release(struct kobject *kobj)
{
	struct cdev *p = container_of(kobj, struct cdev, kobj);

	if (p->__lkpi_dynamic)
		kfree(p);
}

static const struct kobj_type lkpi_cdev_ktype = {
	.release = lkpi_cdev_release,
};

void lkpi_cdev_init(struct cdev *cdev, const struct file_operations *fops)
{
	memset(cdev, 0, sizeof(*cdev));
	INIT_LIST_HEAD(&cdev->list);
	kobject_init(&cdev->kobj, &lkpi_cdev_ktype);
	cdev->ops = fops;
}
EXPORT_SYMBOL(lkpi_cdev_init);

struct cdev *lkpi_cdev_alloc(void)
{
	struct cdev *p = kzalloc(sizeof(*p), GFP_KERNEL);

	if (p) {
		INIT_LIST_HEAD(&p->list);
		kobject_init(&p->kobj, &lkpi_cdev_ktype);
		p->__lkpi_dynamic = true;
	}
	return p;
}
EXPORT_SYMBOL(lkpi_cdev_alloc);

int lkpi_cdev_add(struct cdev *p, dev_t dev, unsigned int count)
{
	if (!count)
		return -EINVAL;
	p->dev = dev;
	p->count = count;
	mutex_lock(&lkpi_chrdev_mutex);
	list_add_tail(&p->list, &lkpi_cdevs);
	mutex_unlock(&lkpi_chrdev_mutex);
	kobject_get(p->kobj.parent);
	return 0;
}
EXPORT_SYMBOL(lkpi_cdev_add);

void lkpi_cdev_del(struct cdev *p)
{
	mutex_lock(&lkpi_chrdev_mutex);
	list_del_init(&p->list);
	mutex_unlock(&lkpi_chrdev_mutex);
	kobject_put(p->kobj.parent);
	kobject_put(&p->kobj);
}
EXPORT_SYMBOL(lkpi_cdev_del);

void lkpi_cdev_set_parent(struct cdev *p, struct kobject *kobj)
{
	p->kobj.parent = kobj;
}
EXPORT_SYMBOL(lkpi_cdev_set_parent);

int lkpi_cdev_device_add(struct cdev *cdev, struct device *dev)
{
	int rc = 0;

	if (dev->devt) {
		lkpi_cdev_set_parent(cdev, &dev->kobj);
		rc = cdev_add(cdev, dev->devt, 1);
		if (rc)
			return rc;
	}
	rc = device_add(dev);
	if (rc && dev->devt)
		cdev_del(cdev);
	return rc;
}
EXPORT_SYMBOL(lkpi_cdev_device_add);

void lkpi_cdev_device_del(struct cdev *cdev, struct device *dev)
{
	device_del(dev);
	if (dev->devt)
		cdev_del(cdev);
}
EXPORT_SYMBOL(lkpi_cdev_device_del);

/* cdev del dev_t con una referencia, o NULL. */
static struct cdev *lkpi_cdev_lookup(dev_t devt)
{
	struct cdev *p;

	mutex_lock(&lkpi_chrdev_mutex);
	list_for_each_entry(p, &lkpi_cdevs, list)
		if (devt >= p->dev && devt < p->dev + p->count) {
			kobject_get(&p->kobj);
			mutex_unlock(&lkpi_chrdev_mutex);
			return p;
		}
	mutex_unlock(&lkpi_chrdev_mutex);
	return NULL;
}

/* -------------------------------------------------------- nodos de /dev */

struct lkpi_devnode {
	struct lkpi_vnode_src src;	/* primero */
	struct list_head list;
	struct device *dev;
	dev_t devt;
	umode_t mode;
	bool dead;
	char path[AURORA_SYSFS_PATH_MAX];
};
static LIST_HEAD(lkpi_devnodes);

static u32 lkpi_encode_dev(dev_t devt)
{
	unsigned int ma = MAJOR(devt), mi = MINOR(devt);

	return (mi & 0xff) | (ma << 8) | ((mi & ~0xffu) << 12);
}

static struct file *lkpi_devnode_open(struct lkpi_vnode_src *src, unsigned int flags)
{
	struct lkpi_devnode *dn = container_of(src, struct lkpi_devnode, src);
	struct cdev *cdev;
	struct file *f;
	int rc = 0;

	if (READ_ONCE(dn->dead))
		return ERR_PTR(-ENODEV);
	cdev = lkpi_cdev_lookup(dn->devt);
	if (!cdev)
		return ERR_PTR(-ENXIO);
	f = lkpi_file_alloc(cdev->ops, flags);
	if (!f) {
		kobject_put(&cdev->kobj);
		return ERR_PTR(-ENOMEM);
	}
	f->f_inode->i_rdev = dn->devt;
	f->f_inode->i_cdev = cdev;
	f->f_inode->i_mode = AURORA_S_IFCHR | dn->mode;
	if (f->f_op && f->f_op->open)
		rc = f->f_op->open(f->f_inode, f);
	if (rc) {
		kobject_put(&cdev->kobj);
		lkpi_file_free(f);
		return ERR_PTR(rc);
	}
	if (!f->f_op || !f->f_op->llseek)
		f->f_mode &= ~FMODE_LSEEK;
	return f;	/* la referencia al cdev la suelta el cierre (fs/file.c) */
}

int lkpi_devnode_add(struct device *dev)
{
	const char *name = NULL;
	char *tmp = NULL;
	umode_t mode = 0;
	struct lkpi_devnode *dn;
	int rc;

	if (dev->type && dev->type->devnode)
		name = tmp = dev->type->devnode(dev, &mode, NULL, NULL);
	if (!name && dev->class && dev->class->devnode)
		name = tmp = dev->class->devnode(dev, &mode);
	if (!name)
		name = dev_name(dev);
	if (!mode)
		mode = 0600;
	dn = kzalloc(sizeof(*dn), GFP_KERNEL);
	if (!dn) {
		kfree(tmp);
		return -ENOMEM;
	}
	dn->src.magic = LKPI_VNODE_MAGIC;
	dn->src.open = lkpi_devnode_open;
	dn->dev = dev;
	dn->devt = dev->devt;
	dn->mode = mode & 0777;
	strscpy(dn->path, name, sizeof(dn->path));
	kfree(tmp);
	rc = aurora_devtmpfs_register(dn->path, lkpi_encode_dev(dn->devt), dn->mode, 0, 0,
				      &lkpi_file_vops, &dn->src);
	if (rc) {
		dev_warn(dev, "no se pudo crear /dev/%s: %d\n", dn->path, rc);
		kfree(dn);
		return rc;
	}
	mutex_lock(&lkpi_chrdev_mutex);
	list_add_tail(&dn->list, &lkpi_devnodes);
	mutex_unlock(&lkpi_chrdev_mutex);
	return 0;
}

void lkpi_devnode_del(struct device *dev)
{
	struct lkpi_devnode *dn;

	mutex_lock(&lkpi_chrdev_mutex);
	list_for_each_entry(dn, &lkpi_devnodes, list)
		if (dn->dev == dev && !dn->dead) {
			dn->dead = true;
			dn->dev = NULL;
			aurora_devtmpfs_unregister(dn->path);
			break;
		}
	mutex_unlock(&lkpi_chrdev_mutex);
}

/* ---------------------------------------------------------------- misc */

static LIST_HEAD(lkpi_misc_list);
static DEFINE_MUTEX(lkpi_misc_mutex);
static DEFINE_IDA(lkpi_misc_minors);
static struct cdev lkpi_misc_cdev;

static char *lkpi_misc_devnode(const struct device *dev, umode_t *mode)
{
	struct miscdevice *c = dev_get_drvdata(dev);

	if (mode && c->mode)
		*mode = c->mode;
	if (c->nodename)
		return kstrdup(c->nodename, GFP_KERNEL);
	return NULL;
}

static const struct class lkpi_misc_class = {
	.name = "misc",
	.devnode = lkpi_misc_devnode,
};

static int lkpi_misc_open(struct inode *inode, struct file *file)
{
	int minor = iminor(inode);
	struct miscdevice *c = NULL, *it;
	const struct file_operations *new_fops = NULL;
	int rc = -ENODEV;

	mutex_lock(&lkpi_misc_mutex);
	list_for_each_entry(it, &lkpi_misc_list, list)
		if (it->minor == minor) {
			c = it;
			new_fops = it->fops;
			break;
		}
	if (!new_fops)
		goto out;
	/* Como Linux: private_data = la miscdevice, y las fops del driver. */
	file->private_data = c;
	file->f_op = new_fops;
	rc = 0;
	if (file->f_op->open)
		rc = file->f_op->open(inode, file);
out:
	mutex_unlock(&lkpi_misc_mutex);
	return rc;
}

static const struct file_operations lkpi_misc_fops = {
	.open = lkpi_misc_open,
	.llseek = lkpi_noop_llseek,
};

int lkpi_misc_register(struct miscdevice *misc)
{
	dev_t dev;
	int rc = 0;

	INIT_LIST_HEAD(&misc->list);
	mutex_lock(&lkpi_misc_mutex);
	if (misc->minor == MISC_DYNAMIC_MINOR) {
		int i = ida_alloc_range(&lkpi_misc_minors, 128, 254, GFP_KERNEL);

		if (i < 0) {
			rc = -EBUSY;
			goto out;
		}
		misc->minor = i;
	} else {
		struct miscdevice *c;

		list_for_each_entry(c, &lkpi_misc_list, list)
			if (c->minor == misc->minor) {
				rc = -EBUSY;
				goto out;
			}
	}
	dev = MKDEV(MISC_MAJOR, misc->minor);
	misc->this_device = device_create_with_groups(&lkpi_misc_class, misc->parent, dev, misc,
						      misc->groups, "%s", misc->name);
	if (IS_ERR(misc->this_device)) {
		rc = PTR_ERR(misc->this_device);
		if (misc->minor >= 128 && misc->minor <= 254)
			ida_free(&lkpi_misc_minors, misc->minor);
		misc->this_device = NULL;
		goto out;
	}
	list_add(&misc->list, &lkpi_misc_list);
out:
	mutex_unlock(&lkpi_misc_mutex);
	return rc;
}
EXPORT_SYMBOL(lkpi_misc_register);

void lkpi_misc_deregister(struct miscdevice *misc)
{
	mutex_lock(&lkpi_misc_mutex);
	list_del(&misc->list);
	device_destroy(&lkpi_misc_class, MKDEV(MISC_MAJOR, misc->minor));
	if (misc->minor >= 128 && misc->minor <= 254)
		ida_free(&lkpi_misc_minors, misc->minor);
	mutex_unlock(&lkpi_misc_mutex);
}
EXPORT_SYMBOL(lkpi_misc_deregister);

int lkpi_chrdev_init(void)
{
	int rc = class_register(&lkpi_misc_class);

	if (rc)
		return rc;
	rc = lkpi_register_chrdev_region(MKDEV(MISC_MAJOR, 0), 256, "misc");
	if (rc)
		goto err_class;
	cdev_init(&lkpi_misc_cdev, &lkpi_misc_fops);
	rc = cdev_add(&lkpi_misc_cdev, MKDEV(MISC_MAJOR, 0), 256);
	if (rc)
		goto err_region;
	return 0;
err_region:
	lkpi_unregister_chrdev_region(MKDEV(MISC_MAJOR, 0), 256);
err_class:
	class_unregister(&lkpi_misc_class);
	return rc;
}

void lkpi_chrdev_exit(void)
{
	struct lkpi_devnode *dn, *n;

	cdev_del(&lkpi_misc_cdev);
	lkpi_unregister_chrdev_region(MKDEV(MISC_MAJOR, 0), 256);
	class_unregister(&lkpi_misc_class);
	list_for_each_entry_safe(dn, n, &lkpi_devnodes, list) {
		if (!dn->dead)
			aurora_devtmpfs_unregister(dn->path);
		list_del(&dn->list);
		kfree(dn);
	}
}
