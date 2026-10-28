// SPDX-License-Identifier: GPL-2.0-only
/*
 * debugfs (K6, L4): arbol en memoria montado en /sys/kernel/debug como un
 * sistema de ficheros de Aurora. Los directorios tienen su propio readdir;
 * los ficheros, lkpi_file_vops con la dentry como origen (lkpi_vnode_src):
 * al abrir se crea el struct file con las fops del driver y el inode de la
 * dentry (i_private = data). Ver linux/debugfs.h.
 */
#define LKPI_DENTRY_PRIVATE 1
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/kstrtox.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "../aurora.h"
#include "../lkpi_internal.h"
#include "fs.h"

#define LKPI_DEBUGFS_MOUNT "/sys/kernel/debug"

struct dentry {
	struct qstr d_name;		/* publico (linux/fs.h), primero */
	struct lkpi_vnode_src src;	/* primero: origen del nodo de fichero */
	char *name;
	struct dentry *parent;
	struct list_head children;
	struct list_head sibling;
	struct list_head all;		/* todas, para liberarlas al descargar */
	umode_t mode;
	bool is_dir;
	bool dead;
	const struct file_operations *fops;
	struct inode *inode;
};

struct inode *lkpi_d_inode(struct dentry *d)
{
	return d ? d->inode : NULL;
}
EXPORT_SYMBOL(lkpi_d_inode);

static DEFINE_MUTEX(lkpi_debugfs_mutex);
static LIST_HEAD(lkpi_debugfs_all);
static struct dentry lkpi_debugfs_root;
static bool lkpi_debugfs_mounted;

/* ------------------------------------------------------ apertura */

static struct file *lkpi_debugfs_open(struct lkpi_vnode_src *src, unsigned int flags)
{
	struct dentry *d = container_of(src, struct dentry, src);
	struct file *f;
	int rc = 0;

	mutex_lock(&lkpi_debugfs_mutex);
	if (d->dead) {
		mutex_unlock(&lkpi_debugfs_mutex);
		return ERR_PTR(-ENOENT);
	}
	ihold(d->inode);
	mutex_unlock(&lkpi_debugfs_mutex);
	f = lkpi_file_alloc(d->fops, flags);
	if (!f) {
		lkpi_iput(d->inode);
		return ERR_PTR(-ENOMEM);
	}
	f->f_inode = d->inode;
	f->f_path.dentry = d;
	f->f_mapping = d->inode->i_mapping;
	if (f->f_op && f->f_op->open)
		rc = f->f_op->open(f->f_inode, f);
	if (rc) {
		lkpi_iput(d->inode);
		lkpi_file_free(f);
		return ERR_PTR(rc);
	}
	if (!f->f_op || !f->f_op->llseek)
		f->f_mode &= ~FMODE_LSEEK;
	return f;
}

/* -------------------------------------------- sistema de ficheros */

static struct dentry *lkpi_debugfs_child(struct dentry *dir, const char *name, size_t len)
{
	struct dentry *c;

	list_for_each_entry(c, &dir->children, sibling)
		if (!c->dead && strlen(c->name) == len && !strncmp(c->name, name, len))
			return c;
	return NULL;
}

static int lkpi_debugfs_readdir(struct aurora_vfs_node *dn, u64 index,
				struct aurora_vfs_dirent *out)
{
	struct dentry *dir = aurora_vfs_node_get_priv(dn), *c;
	u64 i = 0;

	memset(out, 0, sizeof(*out));
	if (!dir || !dir->is_dir)
		return -ENOTDIR;
	mutex_lock(&lkpi_debugfs_mutex);
	list_for_each_entry(c, &dir->children, sibling) {
		if (c->dead)
			continue;
		if (i++ == index) {
			strscpy(out->name, c->name, sizeof(out->name));
			out->type = c->is_dir ? AURORA_VFS_DIRECTORY : AURORA_VFS_FILE;
			out->size = c->is_dir ? 0 : i_size_read(c->inode);
			break;
		}
	}
	mutex_unlock(&lkpi_debugfs_mutex);
	return 0;	/* nombre vacio: fin */
}

static const struct aurora_vfs_ops lkpi_debugfs_dir_vops = {
	.readdir = lkpi_debugfs_readdir,
};

static struct aurora_vfs_node *lkpi_debugfs_lookup_fs(void *fs_priv, const char *path)
{
	struct dentry *d = &lkpi_debugfs_root;
	struct aurora_vfs_node *n;
	const char *p = path;

	mutex_lock(&lkpi_debugfs_mutex);
	while (d && *p) {
		const char *e;

		while (*p == '/')
			p++;
		if (!*p)
			break;
		e = strchrnul(p, '/');
		d = d->is_dir ? lkpi_debugfs_child(d, p, e - p) : NULL;
		p = e;
	}
	if (!d) {
		mutex_unlock(&lkpi_debugfs_mutex);
		return NULL;
	}
	if (d->is_dir)
		n = aurora_vfs_node_alloc(d->name, AURORA_VFS_DIRECTORY, AURORA_S_IFDIR | d->mode,
					  0, &lkpi_debugfs_dir_vops, d, 0);
	else
		n = aurora_vfs_node_alloc(d->name, AURORA_VFS_FILE, AURORA_S_IFREG | d->mode, 0,
					  &lkpi_file_vops, &d->src, i_size_read(d->inode));
	mutex_unlock(&lkpi_debugfs_mutex);
	return n;
}

static const struct aurora_vfs_fs_ops lkpi_debugfs_fs_ops = {
	.lookup = lkpi_debugfs_lookup_fs,
	.name = "debugfs",
};

/* ------------------------------------------------------ entradas */

static struct dentry *lkpi_debugfs_new(const char *name, umode_t mode, struct dentry *parent,
				       bool is_dir)
{
	struct dentry *d;

	if (!lkpi_debugfs_mounted)
		return ERR_PTR(-ENODEV);
	if (!parent)
		parent = &lkpi_debugfs_root;
	if (IS_ERR(parent))
		return parent;
	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return ERR_PTR(-ENOMEM);
	d->name = kstrdup(name, GFP_KERNEL);
	d->inode = lkpi_alloc_anon_inode(NULL);
	if (!d->name || IS_ERR(d->inode)) {
		if (!IS_ERR(d->inode))
			lkpi_iput(d->inode);
		kfree(d->name);
		kfree(d);
		return ERR_PTR(-ENOMEM);
	}
	d->d_name.name = (const unsigned char *)d->name;
	d->d_name.len = strlen(d->name);
	d->src.magic = LKPI_VNODE_MAGIC;
	d->src.open = lkpi_debugfs_open;
	d->mode = mode & 0777;
	d->is_dir = is_dir;
	d->parent = parent;
	INIT_LIST_HEAD(&d->children);
	d->inode->i_mode = (is_dir ? AURORA_S_IFDIR : AURORA_S_IFREG) | d->mode;
	mutex_lock(&lkpi_debugfs_mutex);
	if (parent->dead || !parent->is_dir || lkpi_debugfs_child(parent, name, strlen(name))) {
		mutex_unlock(&lkpi_debugfs_mutex);
		lkpi_iput(d->inode);
		kfree(d->name);
		kfree(d);
		return ERR_PTR(-EEXIST);
	}
	list_add_tail(&d->sibling, &parent->children);
	list_add_tail(&d->all, &lkpi_debugfs_all);
	mutex_unlock(&lkpi_debugfs_mutex);
	return d;
}

struct dentry *lkpi_debugfs_create_dir(const char *name, struct dentry *parent)
{
	return lkpi_debugfs_new(name, 0755, parent, true);
}
EXPORT_SYMBOL(lkpi_debugfs_create_dir);

struct dentry *lkpi_debugfs_create_file(const char *name, umode_t mode, struct dentry *parent,
				       void *data, const struct file_operations *fops)
{
	struct dentry *d = lkpi_debugfs_new(name, mode ? mode : 0444, parent, false);

	if (!IS_ERR(d)) {
		d->fops = fops;
		d->inode->i_private = data;
		d->inode->i_fop = fops;
	}
	return d;
}
EXPORT_SYMBOL(lkpi_debugfs_create_file);

void lkpi_debugfs_create_file_size(const char *name, umode_t mode, struct dentry *parent,
				   void *data, const struct file_operations *fops,
				   loff_t file_size)
{
	struct dentry *d = lkpi_debugfs_create_file(name, mode, parent, data, fops);

	if (!IS_ERR(d))
		d->inode->i_size = file_size;
}
EXPORT_SYMBOL(lkpi_debugfs_create_file_size);

static void lkpi_debugfs_kill(struct dentry *d)
{
	struct dentry *c;

	list_for_each_entry(c, &d->children, sibling)
		if (!c->dead)
			lkpi_debugfs_kill(c);
	d->dead = true;
}

void lkpi_debugfs_remove(struct dentry *dentry)
{
	if (IS_ERR_OR_NULL(dentry) || dentry == &lkpi_debugfs_root)
		return;
	mutex_lock(&lkpi_debugfs_mutex);
	lkpi_debugfs_kill(dentry);
	mutex_unlock(&lkpi_debugfs_mutex);
}
EXPORT_SYMBOL(lkpi_debugfs_remove);

struct dentry *lkpi_debugfs_lookup(const char *name, struct dentry *parent)
{
	struct dentry *d;

	if (!parent)
		parent = &lkpi_debugfs_root;
	mutex_lock(&lkpi_debugfs_mutex);
	d = lkpi_debugfs_child(parent, name, strlen(name));
	mutex_unlock(&lkpi_debugfs_mutex);
	return d;
}
EXPORT_SYMBOL(lkpi_debugfs_lookup);

void lkpi_debugfs_lookup_and_remove(const char *name, struct dentry *parent)
{
	lkpi_debugfs_remove(lkpi_debugfs_lookup(name, parent));
}
EXPORT_SYMBOL(lkpi_debugfs_lookup_and_remove);

bool lkpi_debugfs_initialized(void)
{
	return lkpi_debugfs_mounted;
}
EXPORT_SYMBOL(lkpi_debugfs_initialized);

/* --------------------------------------------- atributos simples */

struct lkpi_simple_attr {
	int (*get)(void *, u64 *);
	int (*set)(void *, u64);
	char get_buf[24];
	char set_buf[24];
	void *data;
	const char *fmt;
	struct mutex mutex;
};

int lkpi_simple_attr_open(struct inode *inode, struct file *file, int (*get)(void *, u64 *),
			  int (*set)(void *, u64), const char *fmt)
{
	struct lkpi_simple_attr *attr = kzalloc(sizeof(*attr), GFP_KERNEL);

	if (!attr)
		return -ENOMEM;
	attr->get = get;
	attr->set = set;
	attr->data = inode->i_private;
	attr->fmt = fmt;
	mutex_init(&attr->mutex);
	file->private_data = attr;
	return nonseekable_open(inode, file);
}
EXPORT_SYMBOL(lkpi_simple_attr_open);

int lkpi_simple_attr_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	return 0;
}
EXPORT_SYMBOL(lkpi_simple_attr_release);

ssize_t lkpi_simple_attr_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
	struct lkpi_simple_attr *attr = file->private_data;
	size_t size;
	ssize_t ret;

	if (!attr->get)
		return -EACCES;
	ret = mutex_lock_interruptible(&attr->mutex);
	if (ret)
		return ret;
	if (*ppos && attr->get_buf[0]) {
		size = strlen(attr->get_buf);
	} else {
		u64 val;

		ret = attr->get(attr->data, &val);
		if (ret)
			goto out;
		size = scnprintf(attr->get_buf, sizeof(attr->get_buf), attr->fmt,
				 (unsigned long long)val);
	}
	ret = simple_read_from_buffer(buf, len, ppos, attr->get_buf, size);
out:
	mutex_unlock(&attr->mutex);
	return ret;
}
EXPORT_SYMBOL(lkpi_simple_attr_read);

ssize_t lkpi_simple_attr_write(struct file *file, const char __user *buf, size_t len,
			       loff_t *ppos)
{
	struct lkpi_simple_attr *attr = file->private_data;
	unsigned long long val;
	size_t size;
	ssize_t ret;

	if (!attr->set)
		return -EACCES;
	ret = mutex_lock_interruptible(&attr->mutex);
	if (ret)
		return ret;
	size = min(sizeof(attr->set_buf) - 1, len);
	ret = -EFAULT;
	if (copy_from_user(attr->set_buf, buf, size))
		goto out;
	attr->set_buf[size] = '\0';
	ret = kstrtoull(attr->set_buf, 0, &val);
	if (ret)
		goto out;
	ret = attr->set(attr->data, val);
	if (!ret)
		ret = len;
out:
	mutex_unlock(&attr->mutex);
	return ret;
}
EXPORT_SYMBOL(lkpi_simple_attr_write);

/* ---------------------------------------------------- numeros */

#define LKPI_DEBUGFS_NUM(name, type, fmt) \
static int lkpi_dbg_##name##_get(void *data, u64 *val) \
{ \
	*val = *(type *)data; \
	return 0; \
} \
static int lkpi_dbg_##name##_set(void *data, u64 val) \
{ \
	*(type *)data = (type)val; \
	return 0; \
} \
DEFINE_SIMPLE_ATTRIBUTE(lkpi_fops_##name, lkpi_dbg_##name##_get, lkpi_dbg_##name##_set, fmt); \
DEFINE_SIMPLE_ATTRIBUTE(lkpi_fops_##name##_ro, lkpi_dbg_##name##_get, NULL, fmt); \
void lkpi_debugfs_create_##name(const char *n, umode_t mode, struct dentry *parent, type *value) \
{ \
	lkpi_debugfs_create_file(n, mode, parent, value, \
				 (mode & 0222) ? &lkpi_fops_##name : &lkpi_fops_##name##_ro); \
} \
EXPORT_SYMBOL(lkpi_debugfs_create_##name)

LKPI_DEBUGFS_NUM(u8, u8, "%llu\n");
LKPI_DEBUGFS_NUM(u16, u16, "%llu\n");
LKPI_DEBUGFS_NUM(u32, u32, "%llu\n");
LKPI_DEBUGFS_NUM(u64, u64, "%llu\n");
LKPI_DEBUGFS_NUM(x8, u8, "0x%02llx\n");
LKPI_DEBUGFS_NUM(x16, u16, "0x%04llx\n");
LKPI_DEBUGFS_NUM(x32, u32, "0x%08llx\n");
LKPI_DEBUGFS_NUM(x64, u64, "0x%016llx\n");
LKPI_DEBUGFS_NUM(ulong, unsigned long, "%llu\n");
LKPI_DEBUGFS_NUM(size_t, size_t, "%llu\n");

static int lkpi_dbg_atomic_get(void *data, u64 *val)
{
	*val = atomic_read((atomic_t *)data);
	return 0;
}
static int lkpi_dbg_atomic_set(void *data, u64 val)
{
	atomic_set((atomic_t *)data, (int)val);
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(lkpi_fops_atomic, lkpi_dbg_atomic_get, lkpi_dbg_atomic_set, "%lld\n");

void lkpi_debugfs_create_atomic_t(const char *name, umode_t mode, struct dentry *parent,
				  atomic_t *value)
{
	lkpi_debugfs_create_file(name, mode, parent, value, &lkpi_fops_atomic);
}
EXPORT_SYMBOL(lkpi_debugfs_create_atomic_t);

static ssize_t lkpi_dbg_bool_read(struct file *file, char __user *ubuf, size_t count,
				  loff_t *ppos)
{
	char buf[2] = { *(bool *)file->f_inode->i_private ? 'Y' : 'N', '\n' };

	return simple_read_from_buffer(ubuf, count, ppos, buf, 2);
}

static ssize_t lkpi_dbg_bool_write(struct file *file, const char __user *ubuf, size_t count,
				   loff_t *ppos)
{
	char buf[8] = {};
	bool v;
	int rc;

	if (copy_from_user(buf, ubuf, min(count, sizeof(buf) - 1)))
		return -EFAULT;
	rc = kstrtobool(buf, &v);
	if (rc)
		return rc;
	*(bool *)file->f_inode->i_private = v;
	return count;
}

static const struct file_operations lkpi_fops_bool = {
	.read = lkpi_dbg_bool_read,
	.write = lkpi_dbg_bool_write,
	.llseek = default_llseek,
};

void lkpi_debugfs_create_bool(const char *name, umode_t mode, struct dentry *parent, bool *value)
{
	lkpi_debugfs_create_file(name, mode, parent, value, &lkpi_fops_bool);
}
EXPORT_SYMBOL(lkpi_debugfs_create_bool);

static ssize_t lkpi_dbg_blob_read(struct file *file, char __user *ubuf, size_t count,
				  loff_t *ppos)
{
	struct debugfs_blob_wrapper *blob = file->f_inode->i_private;

	return simple_read_from_buffer(ubuf, count, ppos, blob->data, blob->size);
}

static const struct file_operations lkpi_fops_blob = {
	.read = lkpi_dbg_blob_read,
	.llseek = default_llseek,
};

struct dentry *lkpi_debugfs_create_blob(const char *name, umode_t mode, struct dentry *parent,
				       struct debugfs_blob_wrapper *blob)
{
	struct dentry *d = lkpi_debugfs_create_file(name, mode & 0444, parent, blob,
						    &lkpi_fops_blob);

	if (!IS_ERR(d))
		d->inode->i_size = blob->size;
	return d;
}
EXPORT_SYMBOL(lkpi_debugfs_create_blob);

static int lkpi_dbg_regset32_show(struct seq_file *s, void *data)
{
	struct debugfs_regset32 *regset = s->private;
	int i;

	for (i = 0; i < regset->nregs; i++)
		seq_printf(s, "%s = 0x%08x\n", regset->regs[i].name,
			   readl(regset->base + regset->regs[i].offset));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lkpi_dbg_regset32);

void lkpi_debugfs_create_regset32(const char *name, umode_t mode, struct dentry *parent,
				  struct debugfs_regset32 *regset)
{
	lkpi_debugfs_create_file(name, mode, parent, regset, &lkpi_dbg_regset32_fops);
}
EXPORT_SYMBOL(lkpi_debugfs_create_regset32);

/* ------------------------------------------------- montar/desmontar */

int lkpi_debugfs_init(void)
{
	int rc;

	lkpi_debugfs_root.name = "debug";
	lkpi_debugfs_root.mode = 0700;
	lkpi_debugfs_root.is_dir = true;
	INIT_LIST_HEAD(&lkpi_debugfs_root.children);
	rc = aurora_vfs_mount(LKPI_DEBUGFS_MOUNT, &lkpi_debugfs_fs_ops, NULL);
	if (rc) {
		pr_warn("linuxkpi: no se pudo montar debugfs en %s: %d\n", LKPI_DEBUGFS_MOUNT, rc);
		return 0;	/* sin debugfs: los create_* devuelven -ENODEV */
	}
	lkpi_debugfs_mounted = true;
	return 0;
}

void lkpi_debugfs_exit(void)
{
	struct dentry *d, *n;

	if (lkpi_debugfs_mounted)
		aurora_vfs_umount(LKPI_DEBUGFS_MOUNT);
	lkpi_debugfs_mounted = false;
	list_for_each_entry_safe(d, n, &lkpi_debugfs_all, all) {
		lkpi_iput(d->inode);
		kfree(d->name);
		kfree(d);
	}
	INIT_LIST_HEAD(&lkpi_debugfs_all);
	INIT_LIST_HEAD(&lkpi_debugfs_root.children);
}
