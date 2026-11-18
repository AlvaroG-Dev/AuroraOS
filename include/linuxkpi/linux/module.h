/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimo de modulo para L1: module_init/module_exit se convierten en los
 * puntos de entrada del .kur (kur_init/kur_exit). struct module, el
 * refcount y /sys/module llegan con K4 (L3).
 */
#ifndef _LKPI_LINUX_MODULE_H
#define _LKPI_LINUX_MODULE_H

#include <linux/compiler.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/stringify.h>
#include <linux/types.h>

struct module;

#define __MODULE_INFO(tag, name, info) \
	static const char __UNIQUE_ID(name)[] __used __section(".modinfo") \
		__aligned(1) = __stringify(tag) "=" info
#define MODULE_INFO(tag, info) __MODULE_INFO(tag, tag, info)
#define MODULE_LICENSE(l) MODULE_INFO(license, l)
#define MODULE_AUTHOR(a) MODULE_INFO(author, a)
#define MODULE_DESCRIPTION(d) MODULE_INFO(description, d)
#define MODULE_VERSION(v) MODULE_INFO(version, v)
#define MODULE_ALIAS(a) MODULE_INFO(alias, a)
#define MODULE_ALIAS_CHARDEV_MAJOR(major) MODULE_ALIAS("char-major-" __stringify(major) "-*")
#define MODULE_SOFTDEP(s) MODULE_INFO(softdep, s)
#define MODULE_FIRMWARE(f) MODULE_INFO(firmware, f)
#define MODULE_PARM_DESC(p, d) MODULE_INFO(parm, #p ":" d)
#define MODULE_DEVICE_TABLE(type, name)

/* Un .kur de varios ficheros con initcall propio (lkpi_virtio) los redefine con -D. */
#ifndef module_init
#define module_init(initfn) \
	int kur_init(void); \
	int kur_init(void) { return initfn(); }
#endif
#ifndef module_exit
#define module_exit(exitfn) \
	void kur_exit(void); \
	void kur_exit(void) { exitfn(); }
#endif
#define late_initcall(fn) module_init(fn)
#define subsys_initcall(fn) module_init(fn)
#define device_initcall(fn) module_init(fn)
#ifndef core_initcall
#define core_initcall(fn) module_init(fn)
#endif
#define postcore_initcall(fn) module_init(fn)
#define arch_initcall(fn) module_init(fn)
#define fs_initcall(fn) module_init(fn)

/*
 * Referencias al modulo: mientras haya alguna (fichero abierto con sus fops,
 * dma-buf exportado...), kur_loader_unload() devuelve -EBUSY. Desde que la
 * descarga empieza (refcnt < 0) try_module_get() falla, como en Linux.
 */
static inline bool try_module_get(struct module *m)
{
	int r;

	if (!m)
		return true;
	r = __atomic_load_n(&m->refcnt, __ATOMIC_RELAXED);
	do {
		if (r < 0)
			return false;
	} while (!__atomic_compare_exchange_n(&m->refcnt, &r, r + 1, false,
					      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
	return true;
}
static inline void module_put(struct module *m)
{
	if (m)
		__atomic_fetch_sub(&m->refcnt, 1, __ATOMIC_RELEASE);
}
static inline void __module_get(struct module *m)
{
	if (m)
		__atomic_fetch_add(&m->refcnt, 1, __ATOMIC_RELAXED);
}
static inline int module_refcount(struct module *m)
{
	return m ? __atomic_load_n(&m->refcnt, __ATOMIC_RELAXED) : 0;
}
#define module_name(m) ((m) && (m)->name[0] ? (const char *)(m)->name : "linuxkpi")
#define symbol_get(x) (&(x))
#define symbol_put(x) do { } while (0)
#define symbol_request(x) (&(x))

#endif
