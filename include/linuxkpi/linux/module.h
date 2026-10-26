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
#define MODULE_SOFTDEP(s) MODULE_INFO(softdep, s)
#define MODULE_FIRMWARE(f) MODULE_INFO(firmware, f)
#define MODULE_PARM_DESC(p, d) MODULE_INFO(parm, #p ":" d)
#define MODULE_DEVICE_TABLE(type, name)

#define module_init(initfn) \
	int kur_init(void); \
	int kur_init(void) { return initfn(); }
#define module_exit(exitfn) \
	void kur_exit(void); \
	void kur_exit(void) { exitfn(); }
#define late_initcall(fn) module_init(fn)
#define subsys_initcall(fn) module_init(fn)
#define device_initcall(fn) module_init(fn)

static inline bool try_module_get(struct module *m) { return true; }
static inline void module_put(struct module *m) { }
static inline void __module_get(struct module *m) { }
#define module_name(m) "linuxkpi"
#define symbol_get(x) (&(x))
#define symbol_put(x) do { } while (0)
#define symbol_request(x) (&(x))

#endif
