/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kobject/kset (K4). Cuenta de referencias con kref; mientras un kobject
 * esta anadido (kobject_add) tiene un directorio en el /sys real de Aurora,
 * "padre/nombre" ("devices/..." para los dispositivos). Los uevents no
 * salen todavia hacia el userland (L5): kobject_uevent() devuelve 0.
 */
#ifndef _LKPI_LINUX_KOBJECT_H
#define _LKPI_LINUX_KOBJECT_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/sysfs.h>
#include <linux/compiler.h>
#include <linux/spinlock.h>
#include <linux/kref.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/stdarg.h>
#include <linux/uidgid.h>

#define UEVENT_HELPER_PATH_LEN 256
#define UEVENT_NUM_ENVP 64
#define UEVENT_BUFFER_SIZE 2048

enum kobject_action {
	KOBJ_ADD,
	KOBJ_REMOVE,
	KOBJ_CHANGE,
	KOBJ_MOVE,
	KOBJ_ONLINE,
	KOBJ_OFFLINE,
	KOBJ_BIND,
	KOBJ_UNBIND,
};

struct kobj_type;
struct kset;

struct kobject {
	const char *name;
	struct list_head entry;		/* en kset->list */
	struct kobject *parent;
	struct kset *kset;
	const struct kobj_type *ktype;
	struct kref kref;
	unsigned int state_initialized:1;
	unsigned int state_in_sysfs:1;
	unsigned int state_add_uevent_sent:1;
	unsigned int state_remove_uevent_sent:1;
	unsigned int uevent_suppress:1;
	/* capa: ruta en /sys (sin "/sys/") y ficheros creados bajo ella */
	char *lkpi_path;
	struct list_head lkpi_files;
};

struct kobj_uevent_env {
	char *argv[3];
	char *envp[UEVENT_NUM_ENVP];
	int envp_idx;
	char buf[UEVENT_BUFFER_SIZE];
	int buflen;
};

struct kobj_ns_type_operations;

struct kobj_type {
	void (*release)(struct kobject *kobj);
	const struct sysfs_ops *sysfs_ops;
	const struct attribute_group **default_groups;
	const struct kobj_ns_type_operations *(*child_ns_type)(const struct kobject *kobj);
	const void *(*namespace)(const struct kobject *kobj);
	void (*get_ownership)(const struct kobject *kobj, kuid_t *uid, kgid_t *gid);
};

struct kset_uevent_ops {
	int (*const filter)(const struct kobject *kobj);
	const char *(*const name)(const struct kobject *kobj);
	int (*const uevent)(const struct kobject *kobj, struct kobj_uevent_env *env);
};

struct kobj_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj, struct kobj_attribute *attr, char *buf);
	ssize_t (*store)(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count);
};

extern const struct sysfs_ops lkpi_kobj_sysfs_ops;
#define kobj_sysfs_ops lkpi_kobj_sysfs_ops

struct kset {
	struct list_head list;
	spinlock_t list_lock;
	struct kobject kobj;
	const struct kset_uevent_ops *uevent_ops;
};

static inline const char *kobject_name(const struct kobject *kobj)
{
	return kobj->name;
}

static inline struct kset *to_kset(struct kobject *kobj)
{
	return kobj ? container_of(kobj, struct kset, kobj) : NULL;
}

void lkpi_kobject_init(struct kobject *kobj, const struct kobj_type *ktype);
__printf(3, 4) int lkpi_kobject_add(struct kobject *kobj, struct kobject *parent,
				    const char *fmt, ...);
__printf(4, 5) int lkpi_kobject_init_and_add(struct kobject *kobj, const struct kobj_type *ktype,
					     struct kobject *parent, const char *fmt, ...);
__printf(2, 3) int lkpi_kobject_set_name(struct kobject *kobj, const char *fmt, ...);
__printf(2, 0) int lkpi_kobject_set_name_vargs(struct kobject *kobj, const char *fmt,
					       va_list vargs);
int lkpi_kobject_rename(struct kobject *kobj, const char *new_name);
void lkpi_kobject_del(struct kobject *kobj);
struct kobject *lkpi_kobject_get(struct kobject *kobj);
struct kobject *lkpi_kobject_get_unless_zero(struct kobject *kobj);
void lkpi_kobject_put(struct kobject *kobj);
struct kobject *lkpi_kobject_create_and_add(const char *name, struct kobject *parent);
char *lkpi_kobject_get_path(const struct kobject *kobj, gfp_t gfp);
int lkpi_kobject_uevent(struct kobject *kobj, enum kobject_action action);
int lkpi_kobject_uevent_env(struct kobject *kobj, enum kobject_action action, char *envp[]);
__printf(2, 3) int lkpi_add_uevent_var(struct kobj_uevent_env *env, const char *format, ...);
const struct kobj_type *lkpi_get_ktype(const struct kobject *kobj);

void lkpi_kset_init(struct kset *kset);
int lkpi_kset_register(struct kset *kset);
void lkpi_kset_unregister(struct kset *kset);
struct kset *lkpi_kset_create_and_add(const char *name, const struct kset_uevent_ops *u,
				      struct kobject *parent_kobj);
struct kobject *lkpi_kset_find_obj(struct kset *kset, const char *name);

#define kobject_init(k, t) lkpi_kobject_init(k, t)
#define kobject_add lkpi_kobject_add
#define kobject_init_and_add lkpi_kobject_init_and_add
#define kobject_set_name lkpi_kobject_set_name
#define kobject_set_name_vargs lkpi_kobject_set_name_vargs
#define kobject_rename(k, n) lkpi_kobject_rename(k, n)
#define kobject_del(k) lkpi_kobject_del(k)
#define kobject_get(k) lkpi_kobject_get(k)
#define kobject_get_unless_zero(k) lkpi_kobject_get_unless_zero(k)
#define kobject_put(k) lkpi_kobject_put(k)
#define kobject_create_and_add(n, p) lkpi_kobject_create_and_add(n, p)
#define kobject_get_path(k, g) lkpi_kobject_get_path(k, g)
#define kobject_uevent(k, a) lkpi_kobject_uevent(k, a)
#define kobject_uevent_env(k, a, e) lkpi_kobject_uevent_env(k, a, e)
#define add_uevent_var lkpi_add_uevent_var
#define get_ktype(k) lkpi_get_ktype(k)
#define kset_init(k) lkpi_kset_init(k)
#define kset_register(k) lkpi_kset_register(k)
#define kset_unregister(k) lkpi_kset_unregister(k)
#define kset_create_and_add(n, u, p) lkpi_kset_create_and_add(n, u, p)
#define kset_find_obj(k, n) lkpi_kset_find_obj(k, n)

static inline struct kset *kset_get(struct kset *k)
{
	return k ? to_kset(kobject_get(&k->kobj)) : NULL;
}
static inline void kset_put(struct kset *k)
{
	kobject_put(&k->kobj);
}

/* Raices de /sys que ven los drivers. */
extern struct kobject *lkpi_kernel_kobj;
#define kernel_kobj lkpi_kernel_kobj

#endif
