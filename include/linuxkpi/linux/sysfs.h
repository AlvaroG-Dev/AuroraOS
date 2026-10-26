/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sysfs basico (K4). Los atributos de los kobject de la capa aparecen en el
 * /sys real de Aurora (registro dinamico, kernel/sysfs_dyn.c): cada fichero
 * llama a show/store del atributo cuando el userland lo abre o lo escribe.
 * Los atributos binarios (bin_attribute) se aceptan pero todavia no se
 * publican (L4/L5).
 */
#ifndef _LKPI_LINUX_SYSFS_H
#define _LKPI_LINUX_SYSFS_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/stringify.h>
#include <linux/errno.h>
#include <linux/string.h>

struct kobject;
struct file;
struct vm_area_struct;
struct address_space;

struct attribute {
	const char *name;
	umode_t mode;
};

struct bin_attribute;

struct attribute_group {
	const char *name;
	umode_t (*is_visible)(struct kobject *, struct attribute *, int);
	umode_t (*is_bin_visible)(struct kobject *, const struct bin_attribute *, int);
	size_t (*bin_size)(struct kobject *, const struct bin_attribute *, int);
	struct attribute **attrs;
	const struct bin_attribute *const *bin_attrs;
};

struct bin_attribute {
	struct attribute attr;
	size_t size;
	void *private;
	struct address_space *(*f_mapping)(void);
	ssize_t (*read)(struct file *, struct kobject *, const struct bin_attribute *,
			char *, loff_t, size_t);
	ssize_t (*write)(struct file *, struct kobject *, const struct bin_attribute *,
			 char *, loff_t, size_t);
	loff_t (*llseek)(struct file *, struct kobject *, const struct bin_attribute *,
			 loff_t, int);
	int (*mmap)(struct file *, struct kobject *, const struct bin_attribute *,
		    struct vm_area_struct *);
};

struct sysfs_ops {
	ssize_t (*show)(struct kobject *, struct attribute *, char *);
	ssize_t (*store)(struct kobject *, struct attribute *, const char *, size_t);
};

#define SYSFS_PREALLOC 010000
#define VERIFY_OCTAL_PERMISSIONS(perms) (perms)

#define __ATTR(_name, _mode, _show, _store) { \
	.attr = { .name = __stringify(_name), .mode = VERIFY_OCTAL_PERMISSIONS(_mode) }, \
	.show = _show, .store = _store, }
#define __ATTR_PREALLOC(_name, _mode, _show, _store) \
	__ATTR(_name, (_mode) | SYSFS_PREALLOC, _show, _store)
#define __ATTR_RO(_name) { \
	.attr = { .name = __stringify(_name), .mode = 0444 }, .show = _name##_show, }
#define __ATTR_RO_MODE(_name, _mode) { \
	.attr = { .name = __stringify(_name), .mode = VERIFY_OCTAL_PERMISSIONS(_mode) }, \
	.show = _name##_show, }
#define __ATTR_RW_MODE(_name, _mode) { \
	.attr = { .name = __stringify(_name), .mode = VERIFY_OCTAL_PERMISSIONS(_mode) }, \
	.show = _name##_show, .store = _name##_store, }
#define __ATTR_WO(_name) { \
	.attr = { .name = __stringify(_name), .mode = 0200 }, .store = _name##_store, }
#define __ATTR_RW(_name) __ATTR(_name, 0644, _name##_show, _name##_store)
#define __ATTR_NULL { .attr = { .name = NULL } }

#define __ATTRIBUTE_GROUPS(_name) \
	static const struct attribute_group *_name##_groups[] = { &_name##_group, NULL, }
#define ATTRIBUTE_GROUPS(_name) \
	static const struct attribute_group _name##_group = { .attrs = _name##_attrs, }; \
	__ATTRIBUTE_GROUPS(_name)
#define BIN_ATTRIBUTE_GROUPS(_name) \
	static const struct attribute_group _name##_group = { .bin_attrs = _name##_attrs, }; \
	__ATTRIBUTE_GROUPS(_name)

#define __BIN_ATTR(_name, _mode, _read, _write, _size) { \
	.attr = { .name = __stringify(_name), .mode = _mode }, \
	.read = _read, .write = _write, .size = _size, }
#define __BIN_ATTR_RO(_name, _size) __BIN_ATTR(_name, 0444, _name##_read, NULL, _size)
#define __BIN_ATTR_RW(_name, _size) __BIN_ATTR(_name, 0644, _name##_read, _name##_write, _size)
#define BIN_ATTR(_name, _mode, _read, _write, _size) \
	struct bin_attribute bin_attr_##_name = __BIN_ATTR(_name, _mode, _read, _write, _size)
#define BIN_ATTR_RO(_name, _size) struct bin_attribute bin_attr_##_name = __BIN_ATTR_RO(_name, _size)
#define BIN_ATTR_RW(_name, _size) struct bin_attribute bin_attr_##_name = __BIN_ATTR_RW(_name, _size)

#define sysfs_attr_init(attr) do { } while (0)
#define sysfs_bin_attr_init(bin_attr) do { } while (0)

int lkpi_sysfs_create_file(struct kobject *kobj, const struct attribute *attr);
void lkpi_sysfs_remove_file(struct kobject *kobj, const struct attribute *attr);
int lkpi_sysfs_create_files(struct kobject *kobj, const struct attribute *const *attrs);
void lkpi_sysfs_remove_files(struct kobject *kobj, const struct attribute *const *attrs);
int lkpi_sysfs_create_bin_file(struct kobject *kobj, const struct bin_attribute *attr);
void lkpi_sysfs_remove_bin_file(struct kobject *kobj, const struct bin_attribute *attr);
int lkpi_sysfs_create_group(struct kobject *kobj, const struct attribute_group *grp);
int lkpi_sysfs_update_group(struct kobject *kobj, const struct attribute_group *grp);
void lkpi_sysfs_remove_group(struct kobject *kobj, const struct attribute_group *grp);
int lkpi_sysfs_create_groups(struct kobject *kobj, const struct attribute_group **groups);
void lkpi_sysfs_remove_groups(struct kobject *kobj, const struct attribute_group **groups);
int lkpi_sysfs_create_link(struct kobject *kobj, struct kobject *target, const char *name);
void lkpi_sysfs_remove_link(struct kobject *kobj, const char *name);
__printf(2, 3) int lkpi_sysfs_emit(char *buf, const char *fmt, ...);
__printf(3, 4) int lkpi_sysfs_emit_at(char *buf, int at, const char *fmt, ...);

#define sysfs_create_file(k, a) lkpi_sysfs_create_file(k, a)
#define sysfs_remove_file(k, a) lkpi_sysfs_remove_file(k, a)
#define sysfs_create_files(k, a) lkpi_sysfs_create_files(k, a)
#define sysfs_remove_files(k, a) lkpi_sysfs_remove_files(k, a)
#define sysfs_create_bin_file(k, a) lkpi_sysfs_create_bin_file(k, a)
#define sysfs_remove_bin_file(k, a) lkpi_sysfs_remove_bin_file(k, a)
#define sysfs_create_group(k, g) lkpi_sysfs_create_group(k, g)
#define sysfs_update_group(k, g) lkpi_sysfs_update_group(k, g)
#define sysfs_remove_group(k, g) lkpi_sysfs_remove_group(k, g)
#define sysfs_create_groups(k, g) lkpi_sysfs_create_groups(k, g)
#define sysfs_update_groups(k, g) lkpi_sysfs_create_groups(k, g)
#define sysfs_remove_groups(k, g) lkpi_sysfs_remove_groups(k, g)
#define sysfs_create_link(k, t, n) lkpi_sysfs_create_link(k, t, n)
#define sysfs_create_link_nowarn(k, t, n) lkpi_sysfs_create_link(k, t, n)
#define sysfs_remove_link(k, n) lkpi_sysfs_remove_link(k, n)
#define sysfs_emit lkpi_sysfs_emit
#define sysfs_emit_at lkpi_sysfs_emit_at
/* Sin poll() sobre ficheros de sysfs todavia: nadie espera la notificacion. */
static inline void sysfs_notify(struct kobject *kobj, const char *dir, const char *attr) { }

#endif
