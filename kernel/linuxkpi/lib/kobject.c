// SPDX-License-Identifier: GPL-2.0-only
/*
 * kobject, kset y sysfs basico (K4). Un kobject anadido tiene un directorio
 * en el /sys real de Aurora: kobj->lkpi_path, relativo a /sys. Cada
 * atributo es un nodo con callbacks del registro dinamico de Aurora
 * (aurora_sysfs_add_attr) cuyo contexto es un struct lkpi_sysfs_file; los
 * enlaces simbolicos apuntan a rutas absolutas ("/sys/devices/...").
 *
 * Los directorios solo existen mientras tienen algo dentro (asi funciona el
 * registro de Aurora): un kobject sin atributos no se ve en /sys.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sprintf.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include "../aurora.h"
#include "../lkpi_internal.h"

/* Protege las listas lkpi_files de todos los kobject y las de los kset. */
static DEFINE_MUTEX(lkpi_sysfs_mutex);

struct lkpi_sysfs_file {
	struct list_head node;
	struct kobject *kobj;
	const struct attribute *attr;
	const struct bin_attribute *bin;	/* fichero binario: show lee con bin->read(off 0) */
	bool is_link;
	char path[];
};

/* ------------------------------------------------------------- rutas */

/* "dir/nombre" en un buffer kmalloc; NULL si no cabe en el registro. */
static char *lkpi_join(const char *dir, const char *name)
{
	char *p;

	if (dir && *dir)
		p = kasprintf(GFP_KERNEL, "%s/%s", dir, name);
	else
		p = kstrdup(name, GFP_KERNEL);
	if (p && strlen(p) >= AURORA_SYSFS_PATH_MAX) {
		pr_warn("linuxkpi: ruta de sysfs demasiado larga: %s\n", p);
		kfree(p);
		return NULL;
	}
	return p;
}

/* Linux no admite '/' en los nombres de sysfs: se cambia por '!'. */
static void lkpi_sanitize(char *s)
{
	for (; *s; s++)
		if (*s == '/')
			*s = '!';
}

/* -------------------------------------------- callbacks de los ficheros */

static long lkpi_sysfs_show_cb(void *ctx, char *buf, size_t size)
{
	struct lkpi_sysfs_file *f = ctx;
	const struct sysfs_ops *ops = f->kobj->ktype ? f->kobj->ktype->sysfs_ops : NULL;

	if (f->bin) {
		size_t n = min_t(size_t, f->bin->size ? f->bin->size : PAGE_SIZE, size);

		return f->bin->read ? f->bin->read(NULL, f->kobj, f->bin, buf, 0, n) : -EIO;
	}
	if (!ops || !ops->show)
		return -EIO;
	return ops->show(f->kobj, (struct attribute *)f->attr, buf);
}

static long lkpi_sysfs_store_cb(void *ctx, const char *buf, size_t len)
{
	struct lkpi_sysfs_file *f = ctx;
	const struct sysfs_ops *ops = f->kobj->ktype ? f->kobj->ktype->sysfs_ops : NULL;
	char *kbuf;
	long rc;

	if (!ops || !ops->store)
		return -EIO;
	if (len >= PAGE_SIZE)
		len = PAGE_SIZE - 1;
	/* store() de Linux recibe un buffer terminado en NUL. */
	kbuf = kmalloc(len + 1, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	memcpy(kbuf, buf, len);
	kbuf[len] = '\0';
	rc = ops->store(f->kobj, (struct attribute *)f->attr, kbuf, len);
	kfree(kbuf);
	return rc;
}

static const struct aurora_sysfs_attr_ops lkpi_sysfs_attr_ops = {
	.show = lkpi_sysfs_show_cb,
	.store = lkpi_sysfs_store_cb,
};

static int lkpi_sysfs_add_bin(struct kobject *kobj, const char *subdir,
			      const struct attribute *attr, umode_t mode,
			      const struct bin_attribute *bin);
static int lkpi_sysfs_add(struct kobject *kobj, const char *subdir,
			  const struct attribute *attr, umode_t mode)
{
	return lkpi_sysfs_add_bin(kobj, subdir, attr, mode, NULL);
}

static int lkpi_sysfs_add_bin(struct kobject *kobj, const char *subdir,
			      const struct attribute *attr, umode_t mode,
			      const struct bin_attribute *bin)
{
	struct lkpi_sysfs_file *f;
	char *dir, *path;
	int rc;

	if (!kobj->state_in_sysfs || !kobj->lkpi_path)
		return -ENOENT;
	dir = subdir ? lkpi_join(kobj->lkpi_path, subdir) : kstrdup(kobj->lkpi_path, GFP_KERNEL);
	if (!dir)
		return -ENOMEM;
	path = lkpi_join(dir, attr->name);
	kfree(dir);
	if (!path)
		return -ENAMETOOLONG;
	f = kzalloc(sizeof(*f) + strlen(path) + 1, GFP_KERNEL);
	if (!f) {
		kfree(path);
		return -ENOMEM;
	}
	f->kobj = kobj;
	f->attr = attr;
	f->bin = bin;
	strcpy(f->path, path);
	kfree(path);
	rc = aurora_sysfs_add_attr(f->path, &lkpi_sysfs_attr_ops, f, mode & 0777);
	if (rc) {
		if (rc == -EEXIST)
			pr_warn("linuxkpi: sysfs: %s ya existe\n", f->path);
		kfree(f);
		return rc;
	}
	mutex_lock(&lkpi_sysfs_mutex);
	list_add_tail(&f->node, &kobj->lkpi_files);
	mutex_unlock(&lkpi_sysfs_mutex);
	return 0;
}

/* Quita del registro el fichero `subdir/name` de kobj (con su contexto). */
static void lkpi_sysfs_del(struct kobject *kobj, const char *subdir, const char *name)
{
	struct lkpi_sysfs_file *f, *found = NULL;
	char *dir, *path;

	if (!kobj->lkpi_path)
		return;
	dir = subdir ? lkpi_join(kobj->lkpi_path, subdir) : kstrdup(kobj->lkpi_path, GFP_KERNEL);
	path = dir ? lkpi_join(dir, name) : NULL;
	kfree(dir);
	if (!path)
		return;
	mutex_lock(&lkpi_sysfs_mutex);
	list_for_each_entry(f, &kobj->lkpi_files, node) {
		if (!strcmp(f->path, path)) {
			list_del(&f->node);
			found = f;
			break;
		}
	}
	mutex_unlock(&lkpi_sysfs_mutex);
	/* Quitarlo espera a las llamadas en curso: despues se puede liberar. */
	aurora_sysfs_remove(path);
	kfree(path);
	kfree(found);
}

int lkpi_sysfs_create_file(struct kobject *kobj, const struct attribute *attr)
{
	if (!kobj || !attr)
		return -EINVAL;
	return lkpi_sysfs_add(kobj, NULL, attr, attr->mode);
}
EXPORT_SYMBOL(lkpi_sysfs_create_file);

void lkpi_sysfs_remove_file(struct kobject *kobj, const struct attribute *attr)
{
	if (kobj && attr)
		lkpi_sysfs_del(kobj, NULL, attr->name);
}
EXPORT_SYMBOL(lkpi_sysfs_remove_file);

int lkpi_sysfs_create_files(struct kobject *kobj, const struct attribute *const *attrs)
{
	int rc = 0, i;

	for (i = 0; attrs[i] && !rc; i++)
		rc = lkpi_sysfs_create_file(kobj, attrs[i]);
	if (rc)
		while (--i >= 0)
			lkpi_sysfs_remove_file(kobj, attrs[i]);
	return rc;
}
EXPORT_SYMBOL(lkpi_sysfs_create_files);

void lkpi_sysfs_remove_files(struct kobject *kobj, const struct attribute *const *attrs)
{
	for (int i = 0; attrs[i]; i++)
		lkpi_sysfs_remove_file(kobj, attrs[i]);
}
EXPORT_SYMBOL(lkpi_sysfs_remove_files);

/* Los binarios (EDID, ROM, config...) no se publican todavia (L4/L5). */
int lkpi_sysfs_create_bin_file(struct kobject *kobj, const struct bin_attribute *attr)
{
	/* Solo lectura, hasta PAGE_SIZE (config de PCI: 256 bytes). */
	if (!kobj || !attr)
		return -EINVAL;
	if (!attr->read)
		return 0;
	return lkpi_sysfs_add_bin(kobj, NULL, &attr->attr, attr->attr.mode ? attr->attr.mode : 0444,
				  attr);
}
EXPORT_SYMBOL(lkpi_sysfs_create_bin_file);

void lkpi_sysfs_remove_bin_file(struct kobject *kobj, const struct bin_attribute *attr)
{
	if (kobj && attr && attr->read)
		lkpi_sysfs_del(kobj, NULL, attr->attr.name);
}
EXPORT_SYMBOL(lkpi_sysfs_remove_bin_file);

static void lkpi_group_remove_files(struct kobject *kobj, const struct attribute_group *grp,
				    int upto)
{
	for (int i = 0; grp->attrs && grp->attrs[i] && (upto < 0 || i < upto); i++)
		lkpi_sysfs_del(kobj, grp->name, grp->attrs[i]->name);
}

static int lkpi_group_create(struct kobject *kobj, const struct attribute_group *grp, bool update)
{
	int i, rc = 0;

	if (!kobj || !grp)
		return -EINVAL;
	for (i = 0; grp->attrs && grp->attrs[i]; i++) {
		struct attribute *a = grp->attrs[i];
		umode_t mode = a->mode;

		if (update)
			lkpi_sysfs_del(kobj, grp->name, a->name);
		if (grp->is_visible) {
			mode = grp->is_visible(kobj, a, i);
			if (!mode)
				continue;
		}
		rc = lkpi_sysfs_add(kobj, grp->name, a, mode);
		if (rc)
			break;
	}
	if (rc)
		lkpi_group_remove_files(kobj, grp, i);
	return rc;
}

int lkpi_sysfs_create_group(struct kobject *kobj, const struct attribute_group *grp)
{
	return lkpi_group_create(kobj, grp, false);
}
EXPORT_SYMBOL(lkpi_sysfs_create_group);

int lkpi_sysfs_update_group(struct kobject *kobj, const struct attribute_group *grp)
{
	return lkpi_group_create(kobj, grp, true);
}
EXPORT_SYMBOL(lkpi_sysfs_update_group);

void lkpi_sysfs_remove_group(struct kobject *kobj, const struct attribute_group *grp)
{
	if (kobj && grp)
		lkpi_group_remove_files(kobj, grp, -1);
}
EXPORT_SYMBOL(lkpi_sysfs_remove_group);

int lkpi_sysfs_create_groups(struct kobject *kobj, const struct attribute_group **groups)
{
	int i, rc = 0;

	if (!groups)
		return 0;
	for (i = 0; groups[i] && !rc; i++)
		rc = lkpi_sysfs_create_group(kobj, groups[i]);
	if (rc)
		while (--i >= 0)
			lkpi_sysfs_remove_group(kobj, groups[i]);
	return rc;
}
EXPORT_SYMBOL(lkpi_sysfs_create_groups);

void lkpi_sysfs_remove_groups(struct kobject *kobj, const struct attribute_group **groups)
{
	if (!groups)
		return;
	for (int i = 0; groups[i]; i++)
		lkpi_sysfs_remove_group(kobj, groups[i]);
}
EXPORT_SYMBOL(lkpi_sysfs_remove_groups);

/* Crea /sys/<dir>/<name> -> /sys/<target_path>. Para la capa (dir libre). */
int lkpi_sysfs_link_path(const char *dir, const char *name, const char *target_path)
{
	char *path = lkpi_join(dir, name);
	char *target = kasprintf(GFP_KERNEL, "/sys/%s", target_path);
	int rc = -ENOMEM;

	if (path && target)
		rc = aurora_sysfs_add_symlink(path, target);
	kfree(path);
	kfree(target);
	return rc;
}

void lkpi_sysfs_unlink_path(const char *dir, const char *name)
{
	char *path = lkpi_join(dir, name);

	if (path)
		aurora_sysfs_remove(path);
	kfree(path);
}

int lkpi_sysfs_create_link(struct kobject *kobj, struct kobject *target, const char *name)
{
	if (!kobj || !target || !kobj->lkpi_path || !target->lkpi_path)
		return -ENOENT;
	return lkpi_sysfs_link_path(kobj->lkpi_path, name, target->lkpi_path);
}
EXPORT_SYMBOL(lkpi_sysfs_create_link);

void lkpi_sysfs_remove_link(struct kobject *kobj, const char *name)
{
	if (kobj && kobj->lkpi_path)
		lkpi_sysfs_unlink_path(kobj->lkpi_path, name);
}
EXPORT_SYMBOL(lkpi_sysfs_remove_link);

int lkpi_sysfs_emit(char *buf, const char *fmt, ...)
{
	va_list args;
	int len;

	/* Linux exige ademas un buffer alineado a pagina; el de Aurora
	 * (sysfs_dyn) mide PAGE_SIZE pero no esta alineado. */
	if (WARN(!buf, "sysfs_emit: buffer NULL\n"))
		return 0;
	va_start(args, fmt);
	len = vscnprintf(buf, PAGE_SIZE, fmt, args);
	va_end(args);
	return len;
}
EXPORT_SYMBOL(lkpi_sysfs_emit);

int lkpi_sysfs_emit_at(char *buf, int at, const char *fmt, ...)
{
	va_list args;
	int len;

	if (WARN(!buf || at < 0 || at >= PAGE_SIZE, "sysfs_emit_at: posicion invalida\n"))
		return 0;
	va_start(args, fmt);
	len = vscnprintf(buf + at, PAGE_SIZE - at, fmt, args);
	va_end(args);
	return len;
}
EXPORT_SYMBOL(lkpi_sysfs_emit_at);

/* ------------------------------------------------------------ kobject */

void lkpi_kobject_init(struct kobject *kobj, const struct kobj_type *ktype)
{
	if (!kobj)
		return;
	WARN(kobj->state_initialized, "kobject %p ya inicializado\n", kobj);
	kref_init(&kobj->kref);
	INIT_LIST_HEAD(&kobj->entry);
	INIT_LIST_HEAD(&kobj->lkpi_files);
	kobj->ktype = ktype;
	kobj->state_in_sysfs = 0;
	kobj->state_add_uevent_sent = 0;
	kobj->state_remove_uevent_sent = 0;
	kobj->state_initialized = 1;
	kobj->lkpi_path = NULL;
}
EXPORT_SYMBOL(lkpi_kobject_init);

int lkpi_kobject_set_name_vargs(struct kobject *kobj, const char *fmt, va_list vargs)
{
	char *s;

	if (kobj->name && !fmt)
		return 0;
	s = kvasprintf(GFP_KERNEL, fmt, vargs);
	if (!s)
		return -ENOMEM;
	lkpi_sanitize(s);
	kfree(kobj->name);
	kobj->name = s;
	return 0;
}
EXPORT_SYMBOL(lkpi_kobject_set_name_vargs);

int lkpi_kobject_set_name(struct kobject *kobj, const char *fmt, ...)
{
	va_list vargs;
	int rc;

	va_start(vargs, fmt);
	rc = lkpi_kobject_set_name_vargs(kobj, fmt, vargs);
	va_end(vargs);
	return rc;
}
EXPORT_SYMBOL(lkpi_kobject_set_name);

static int lkpi_kobject_add_internal(struct kobject *kobj)
{
	struct kobject *parent;
	char *path;
	int rc;

	if (!kobj->name || !kobj->name[0]) {
		WARN(1, "kobject %p sin nombre\n", kobj);
		return -EINVAL;
	}
	parent = kobject_get(kobj->parent);
	if (kobj->kset) {
		if (!parent)
			parent = kobject_get(&kobj->kset->kobj);
		spin_lock(&kobj->kset->list_lock);
		list_add_tail(&kobj->entry, &kobj->kset->list);
		spin_unlock(&kobj->kset->list_lock);
	}
	kobj->parent = parent;

	path = lkpi_join(parent && parent->lkpi_path ? parent->lkpi_path : NULL, kobj->name);
	if (!path) {
		rc = -ENOMEM;
		goto err;
	}
	kobj->lkpi_path = path;
	kobj->state_in_sysfs = 1;
	if (kobj->ktype && kobj->ktype->default_groups) {
		rc = lkpi_sysfs_create_groups(kobj, kobj->ktype->default_groups);
		if (rc)
			goto err_path;
	}
	return 0;

err_path:
	kobj->state_in_sysfs = 0;
	kobj->lkpi_path = NULL;
	kfree(path);
err:
	if (kobj->kset) {
		spin_lock(&kobj->kset->list_lock);
		list_del_init(&kobj->entry);
		spin_unlock(&kobj->kset->list_lock);
	}
	kobject_put(parent);
	kobj->parent = NULL;
	return rc;
}

static int lkpi_kobject_add_varg(struct kobject *kobj, struct kobject *parent,
				 const char *fmt, va_list vargs)
{
	int rc = lkpi_kobject_set_name_vargs(kobj, fmt, vargs);

	if (rc)
		return rc;
	kobj->parent = parent;
	return lkpi_kobject_add_internal(kobj);
}

int lkpi_kobject_add(struct kobject *kobj, struct kobject *parent, const char *fmt, ...)
{
	va_list args;
	int rc;

	if (!kobj)
		return -EINVAL;
	if (!kobj->state_initialized) {
		WARN(1, "kobject_add de un kobject sin inicializar\n");
		return -EINVAL;
	}
	va_start(args, fmt);
	rc = lkpi_kobject_add_varg(kobj, parent, fmt, args);
	va_end(args);
	return rc;
}
EXPORT_SYMBOL(lkpi_kobject_add);

int lkpi_kobject_init_and_add(struct kobject *kobj, const struct kobj_type *ktype,
			      struct kobject *parent, const char *fmt, ...)
{
	va_list args;
	int rc;

	lkpi_kobject_init(kobj, ktype);
	va_start(args, fmt);
	rc = lkpi_kobject_add_varg(kobj, parent, fmt, args);
	va_end(args);
	return rc;
}
EXPORT_SYMBOL(lkpi_kobject_init_and_add);

void lkpi_kobject_del(struct kobject *kobj)
{
	struct lkpi_sysfs_file *f, *tmp;
	struct kobject *parent;
	LIST_HEAD(files);

	if (!kobj || !kobj->state_in_sysfs)
		return;
	/* Quitar todo lo que haya bajo su directorio y, despues, los contextos. */
	aurora_sysfs_remove_tree(kobj->lkpi_path);
	mutex_lock(&lkpi_sysfs_mutex);
	list_splice_init(&kobj->lkpi_files, &files);
	mutex_unlock(&lkpi_sysfs_mutex);
	list_for_each_entry_safe(f, tmp, &files, node)
		kfree(f);
	kobj->state_in_sysfs = 0;
	kfree(kobj->lkpi_path);
	kobj->lkpi_path = NULL;

	if (kobj->kset) {
		spin_lock(&kobj->kset->list_lock);
		list_del_init(&kobj->entry);
		spin_unlock(&kobj->kset->list_lock);
	}
	parent = kobj->parent;
	kobj->parent = NULL;
	kobject_put(parent);
}
EXPORT_SYMBOL(lkpi_kobject_del);

int lkpi_kobject_rename(struct kobject *kobj, const char *new_name)
{
	struct kobject *parent;
	int rc;

	if (!kobj->state_in_sysfs)
		return lkpi_kobject_set_name(kobj, "%s", new_name);
	/* Sencillo: quitar y volver a anadir (los ficheros se recrean con los grupos). */
	parent = kobject_get(kobj->parent);
	lkpi_kobject_del(kobj);
	rc = lkpi_kobject_set_name(kobj, "%s", new_name);
	if (!rc) {
		kobj->parent = parent;
		rc = lkpi_kobject_add_internal(kobj);
	}
	kobject_put(parent);
	return rc;
}
EXPORT_SYMBOL(lkpi_kobject_rename);

struct kobject *lkpi_kobject_get(struct kobject *kobj)
{
	if (kobj) {
		WARN(!kobj->state_initialized, "kobject_get de un kobject sin inicializar\n");
		kref_get(&kobj->kref);
	}
	return kobj;
}
EXPORT_SYMBOL(lkpi_kobject_get);

struct kobject *lkpi_kobject_get_unless_zero(struct kobject *kobj)
{
	if (!kobj)
		return NULL;
	return kref_get_unless_zero(&kobj->kref) ? kobj : NULL;
}
EXPORT_SYMBOL(lkpi_kobject_get_unless_zero);

static void lkpi_kobject_release(struct kref *kref)
{
	struct kobject *kobj = container_of(kref, struct kobject, kref);
	const struct kobj_type *t = kobj->ktype;
	const char *name = kobj->name;

	if (kobj->state_in_sysfs)
		lkpi_kobject_del(kobj);
	if (t && t->release)
		t->release(kobj);
	else
		pr_err("linuxkpi: kobject '%s' sin release(): se pierde la memoria\n",
		       name ? name : "?");
	kfree(name);
}

void lkpi_kobject_put(struct kobject *kobj)
{
	if (!kobj)
		return;
	if (!kobj->state_initialized) {
		WARN(1, "kobject_put de un kobject sin inicializar\n");
		return;
	}
	kref_put(&kobj->kref, lkpi_kobject_release);
}
EXPORT_SYMBOL(lkpi_kobject_put);

static void lkpi_dynamic_kobj_release(struct kobject *kobj)
{
	kfree(kobj);
}

static const struct kobj_type lkpi_dynamic_kobj_ktype = {
	.release = lkpi_dynamic_kobj_release,
	.sysfs_ops = &lkpi_kobj_sysfs_ops,
};

struct kobject *lkpi_kobject_create_and_add(const char *name, struct kobject *parent)
{
	struct kobject *kobj = kzalloc(sizeof(*kobj), GFP_KERNEL);

	if (!kobj)
		return NULL;
	lkpi_kobject_init(kobj, &lkpi_dynamic_kobj_ktype);
	if (lkpi_kobject_add(kobj, parent, "%s", name)) {
		kobject_put(kobj);
		return NULL;
	}
	return kobj;
}
EXPORT_SYMBOL(lkpi_kobject_create_and_add);

char *lkpi_kobject_get_path(const struct kobject *kobj, gfp_t gfp)
{
	if (kobj->lkpi_path)
		return kasprintf(gfp, "/%s", kobj->lkpi_path);
	return kasprintf(gfp, "/%s", kobj->name ? kobj->name : "");
}
EXPORT_SYMBOL(lkpi_kobject_get_path);

/* Los eventos de dispositivos salen por netlink (base/core.c); el resto no */
int lkpi_kobject_uevent_env(struct kobject *kobj, enum kobject_action action, char *envp[])
{
	if (action == KOBJ_ADD)
		kobj->state_add_uevent_sent = 1;
	else if (action == KOBJ_REMOVE)
		kobj->state_remove_uevent_sent = 1;
	return lkpi_device_uevent_send(kobj, action, envp);
}
EXPORT_SYMBOL(lkpi_kobject_uevent_env);

int lkpi_kobject_uevent(struct kobject *kobj, enum kobject_action action)
{
	return lkpi_kobject_uevent_env(kobj, action, NULL);
}
EXPORT_SYMBOL(lkpi_kobject_uevent);

int lkpi_add_uevent_var(struct kobj_uevent_env *env, const char *format, ...)
{
	va_list args;
	int len;

	if (env->envp_idx >= ARRAY_SIZE(env->envp)) {
		WARN(1, "add_uevent_var: demasiadas variables\n");
		return -ENOMEM;
	}
	va_start(args, format);
	len = vsnprintf(&env->buf[env->buflen], sizeof(env->buf) - env->buflen, format, args);
	va_end(args);
	if (len >= (int)(sizeof(env->buf) - env->buflen)) {
		WARN(1, "add_uevent_var: buffer lleno\n");
		return -ENOMEM;
	}
	env->envp[env->envp_idx++] = &env->buf[env->buflen];
	env->buflen += len + 1;
	return 0;
}
EXPORT_SYMBOL(lkpi_add_uevent_var);

const struct kobj_type *lkpi_get_ktype(const struct kobject *kobj)
{
	return kobj->ktype;
}
EXPORT_SYMBOL(lkpi_get_ktype);

/* ---------------------------------------------------- kobj_attribute */

static ssize_t lkpi_kobj_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct kobj_attribute *ka = container_of(attr, struct kobj_attribute, attr);

	return ka->show ? ka->show(kobj, ka, buf) : -EIO;
}

static ssize_t lkpi_kobj_attr_store(struct kobject *kobj, struct attribute *attr,
				    const char *buf, size_t count)
{
	struct kobj_attribute *ka = container_of(attr, struct kobj_attribute, attr);

	return ka->store ? ka->store(kobj, ka, buf, count) : -EIO;
}

const struct sysfs_ops lkpi_kobj_sysfs_ops = {
	.show = lkpi_kobj_attr_show,
	.store = lkpi_kobj_attr_store,
};
EXPORT_SYMBOL(lkpi_kobj_sysfs_ops);

/* --------------------------------------------------------------- kset */

void lkpi_kset_init(struct kset *k)
{
	lkpi_kobject_init(&k->kobj, k->kobj.ktype);
	INIT_LIST_HEAD(&k->list);
	spin_lock_init(&k->list_lock);
}
EXPORT_SYMBOL(lkpi_kset_init);

int lkpi_kset_register(struct kset *k)
{
	if (!k)
		return -EINVAL;
	lkpi_kset_init(k);
	return lkpi_kobject_add_internal(&k->kobj);
}
EXPORT_SYMBOL(lkpi_kset_register);

void lkpi_kset_unregister(struct kset *k)
{
	if (!k)
		return;
	lkpi_kobject_del(&k->kobj);
	kobject_put(&k->kobj);
}
EXPORT_SYMBOL(lkpi_kset_unregister);

static void lkpi_kset_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct kset, kobj));
}

static const struct kobj_type lkpi_kset_ktype = {
	.sysfs_ops = &lkpi_kobj_sysfs_ops,
	.release = lkpi_kset_release,
};

struct kset *lkpi_kset_create_and_add(const char *name, const struct kset_uevent_ops *u,
				      struct kobject *parent_kobj)
{
	struct kset *k = kzalloc(sizeof(*k), GFP_KERNEL);

	if (!k)
		return NULL;
	if (lkpi_kobject_set_name(&k->kobj, "%s", name)) {
		kfree(k);
		return NULL;
	}
	k->uevent_ops = u;
	k->kobj.parent = parent_kobj;
	k->kobj.ktype = &lkpi_kset_ktype;
	k->kobj.kset = NULL;
	if (lkpi_kset_register(k)) {
		kobject_put(&k->kobj);
		return NULL;
	}
	return k;
}
EXPORT_SYMBOL(lkpi_kset_create_and_add);

struct kobject *lkpi_kset_find_obj(struct kset *kset, const char *name)
{
	struct kobject *k, *ret = NULL;

	spin_lock(&kset->list_lock);
	list_for_each_entry(k, &kset->list, entry) {
		if (kobject_name(k) && !strcmp(kobject_name(k), name)) {
			ret = kobject_get_unless_zero(k);
			break;
		}
	}
	spin_unlock(&kset->list_lock);
	return ret;
}
EXPORT_SYMBOL(lkpi_kset_find_obj);

/* ----------------------------------------------------------- arranque */

struct kobject *lkpi_kernel_kobj;
EXPORT_SYMBOL(lkpi_kernel_kobj);

int lkpi_kobject_init_layer(void)
{
	lkpi_kernel_kobj = lkpi_kobject_create_and_add("kernel", NULL);
	return lkpi_kernel_kobj ? 0 : -ENOMEM;
}

void lkpi_kobject_exit_layer(void)
{
	kobject_put(lkpi_kernel_kobj);
	lkpi_kernel_kobj = NULL;
}
