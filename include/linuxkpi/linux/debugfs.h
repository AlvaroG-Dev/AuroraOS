/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * debugfs (K6, L4): arbol en memoria de la capa, montado en
 * /sys/kernel/debug. Los ficheros usan el mismo puente de file_operations
 * que /dev (cat, echo... funcionan). Las entradas borradas no se liberan
 * hasta descargar la capa (puede quedar un lookup de Aurora apuntandolas).
 */
#ifndef _LKPI_LINUX_DEBUGFS_H
#define _LKPI_LINUX_DEBUGFS_H

#include <linux/fs.h>
#include <linux/seq_file.h>
#include <linux/types.h>

struct dentry;
struct device;

struct debugfs_blob_wrapper {
	void *data;
	unsigned long size;
};
struct debugfs_reg32 {
	char *name;
	unsigned long offset;
};
struct debugfs_regset32 {
	const struct debugfs_reg32 *regs;
	int nregs;
	void __iomem *base;
	struct device *dev;
};

struct dentry *lkpi_debugfs_create_dir(const char *name, struct dentry *parent);
struct dentry *lkpi_debugfs_create_file(const char *name, umode_t mode, struct dentry *parent,
				       void *data, const struct file_operations *fops);
void lkpi_debugfs_create_file_size(const char *name, umode_t mode, struct dentry *parent,
				   void *data, const struct file_operations *fops,
				   loff_t file_size);
void lkpi_debugfs_remove(struct dentry *dentry);
struct dentry *lkpi_debugfs_lookup(const char *name, struct dentry *parent);
void lkpi_debugfs_lookup_and_remove(const char *name, struct dentry *parent);
void lkpi_debugfs_create_u8(const char *name, umode_t mode, struct dentry *parent, u8 *value);
void lkpi_debugfs_create_u16(const char *name, umode_t mode, struct dentry *parent, u16 *value);
void lkpi_debugfs_create_u32(const char *name, umode_t mode, struct dentry *parent, u32 *value);
void lkpi_debugfs_create_u64(const char *name, umode_t mode, struct dentry *parent, u64 *value);
void lkpi_debugfs_create_x8(const char *name, umode_t mode, struct dentry *parent, u8 *value);
void lkpi_debugfs_create_x16(const char *name, umode_t mode, struct dentry *parent, u16 *value);
void lkpi_debugfs_create_x32(const char *name, umode_t mode, struct dentry *parent, u32 *value);
void lkpi_debugfs_create_x64(const char *name, umode_t mode, struct dentry *parent, u64 *value);
void lkpi_debugfs_create_ulong(const char *name, umode_t mode, struct dentry *parent,
			       unsigned long *value);
void lkpi_debugfs_create_size_t(const char *name, umode_t mode, struct dentry *parent,
				size_t *value);
void lkpi_debugfs_create_atomic_t(const char *name, umode_t mode, struct dentry *parent,
				  atomic_t *value);
void lkpi_debugfs_create_bool(const char *name, umode_t mode, struct dentry *parent, bool *value);
struct dentry *lkpi_debugfs_create_blob(const char *name, umode_t mode, struct dentry *parent,
				       struct debugfs_blob_wrapper *blob);
void lkpi_debugfs_create_regset32(const char *name, umode_t mode, struct dentry *parent,
				  struct debugfs_regset32 *regset);
bool lkpi_debugfs_initialized(void);

#define debugfs_create_dir(n, p) lkpi_debugfs_create_dir(n, p)
#define debugfs_create_file(n, m, p, d, f) lkpi_debugfs_create_file(n, m, p, d, f)
#define debugfs_create_file_unsafe(n, m, p, d, f) lkpi_debugfs_create_file(n, m, p, d, f)
#define debugfs_create_file_size(n, m, p, d, f, s) lkpi_debugfs_create_file_size(n, m, p, d, f, s)
#define debugfs_remove(d) lkpi_debugfs_remove(d)
#define debugfs_remove_recursive(d) lkpi_debugfs_remove(d)
#define debugfs_lookup(n, p) lkpi_debugfs_lookup(n, p)
#define debugfs_lookup_and_remove(n, p) lkpi_debugfs_lookup_and_remove(n, p)
#define debugfs_create_u8(n, m, p, v) lkpi_debugfs_create_u8(n, m, p, v)
#define debugfs_create_u16(n, m, p, v) lkpi_debugfs_create_u16(n, m, p, v)
#define debugfs_create_u32(n, m, p, v) lkpi_debugfs_create_u32(n, m, p, v)
#define debugfs_create_u64(n, m, p, v) lkpi_debugfs_create_u64(n, m, p, v)
#define debugfs_create_x8(n, m, p, v) lkpi_debugfs_create_x8(n, m, p, v)
#define debugfs_create_x16(n, m, p, v) lkpi_debugfs_create_x16(n, m, p, v)
#define debugfs_create_x32(n, m, p, v) lkpi_debugfs_create_x32(n, m, p, v)
#define debugfs_create_x64(n, m, p, v) lkpi_debugfs_create_x64(n, m, p, v)
#define debugfs_create_ulong(n, m, p, v) lkpi_debugfs_create_ulong(n, m, p, v)
#define debugfs_create_size_t(n, m, p, v) lkpi_debugfs_create_size_t(n, m, p, v)
#define debugfs_create_atomic_t(n, m, p, v) lkpi_debugfs_create_atomic_t(n, m, p, v)
#define debugfs_create_bool(n, m, p, v) lkpi_debugfs_create_bool(n, m, p, v)
#define debugfs_create_blob(n, m, p, b) lkpi_debugfs_create_blob(n, m, p, b)
#define debugfs_create_regset32(n, m, p, r) lkpi_debugfs_create_regset32(n, m, p, r)
#define debugfs_initialized() lkpi_debugfs_initialized()
#define debugfs_create_symlink(n, p, t) ((struct dentry *)ERR_PTR(-ENODEV))

/* Atributos simples: get/set de un u64 con un formato (DEFINE_DEBUGFS_ATTRIBUTE). */
int lkpi_simple_attr_open(struct inode *inode, struct file *file, int (*get)(void *, u64 *),
			  int (*set)(void *, u64), const char *fmt);
int lkpi_simple_attr_release(struct inode *inode, struct file *file);
ssize_t lkpi_simple_attr_read(struct file *file, char __user *buf, size_t len, loff_t *ppos);
ssize_t lkpi_simple_attr_write(struct file *file, const char __user *buf, size_t len,
			       loff_t *ppos);
#define simple_attr_open lkpi_simple_attr_open
#define simple_attr_release lkpi_simple_attr_release
#define simple_attr_read lkpi_simple_attr_read
#define simple_attr_write lkpi_simple_attr_write
#define debugfs_attr_read lkpi_simple_attr_read
#define debugfs_attr_write lkpi_simple_attr_write

#define DEFINE_SIMPLE_ATTRIBUTE_XSIGNED(__fops, __get, __set, __fmt, __is_signed) \
static int __fops##_open(struct inode *inode, struct file *file) \
{ \
	return simple_attr_open(inode, file, __get, __set, __fmt); \
} \
static const struct file_operations __fops = { \
	.owner = THIS_MODULE, \
	.open = __fops##_open, \
	.release = simple_attr_release, \
	.read = simple_attr_read, \
	.write = simple_attr_write, \
	.llseek = generic_file_llseek, \
}
#define DEFINE_SIMPLE_ATTRIBUTE(__fops, __get, __set, __fmt) \
	DEFINE_SIMPLE_ATTRIBUTE_XSIGNED(__fops, __get, __set, __fmt, false)
#define DEFINE_DEBUGFS_ATTRIBUTE(__fops, __get, __set, __fmt) \
	DEFINE_SIMPLE_ATTRIBUTE(__fops, __get, __set, __fmt)
#define DEFINE_DEBUGFS_ATTRIBUTE_SIGNED(__fops, __get, __set, __fmt) \
	DEFINE_SIMPLE_ATTRIBUTE_XSIGNED(__fops, __get, __set, __fmt, true)

#endif
