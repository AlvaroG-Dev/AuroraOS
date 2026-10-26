/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * module_param: cada parametro queda en la seccion __param del modulo
 * (el cargador resuelve __start___param/__stop___param). Hoy solo toman su
 * valor por defecto; la lectura desde la linea de ordenes llegara con K4.
 */
#ifndef _LKPI_LINUX_MODULEPARAM_H
#define _LKPI_LINUX_MODULEPARAM_H

#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/stringify.h>

struct kernel_param;
struct kernel_param_ops {
	unsigned int flags;
	int (*set)(const char *val, const struct kernel_param *kp);
	int (*get)(char *buffer, const struct kernel_param *kp);
	void (*free)(void *arg);
};

struct kernel_param {
	const char *name;
	struct module *mod;
	const struct kernel_param_ops *ops;
	const u16 perm;
	s8 level;
	u8 flags;
	union {
		void *arg;
		const struct kparam_string *str;
		const struct kparam_array *arr;
	};
};

struct kparam_string {
	unsigned int maxlen;
	char *string;
};

struct kparam_array {
	unsigned int max;
	unsigned int elemsize;
	unsigned int *num;
	const struct kernel_param_ops *ops;
	void *elem;
};

#ifndef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX ""
#endif

#define __module_param_call(prefix, name, ops, arg, perm, level, flags) \
	static const char __param_str_##name[] = prefix #name; \
	static struct kernel_param __param_##name __used \
		__section("__param") __aligned(sizeof(void *)) = \
		{ __param_str_##name, NULL, ops, (perm), level, flags, { arg } }

#define module_param_cb(name, ops, arg, perm) \
	__module_param_call(MODULE_PARAM_PREFIX, name, ops, arg, perm, -1, 0)
#define module_param_named(name, value, type, perm) \
	__module_param_call(MODULE_PARAM_PREFIX, name, NULL, &(value), perm, -1, 0)
#define module_param(name, type, perm) module_param_named(name, name, type, perm)
#define module_param_named_unsafe(name, value, type, perm) \
	module_param_named(name, value, type, perm)
#define module_param_unsafe(name, type, perm) module_param(name, type, perm)
#define module_param_string(name, string, len, perm) \
	__module_param_call(MODULE_PARAM_PREFIX, name, NULL, string, perm, -1, 0)
#define module_param_array_named(name, array, type, nump, perm) \
	__module_param_call(MODULE_PARAM_PREFIX, name, NULL, array, perm, -1, 0)
#define module_param_array(name, type, nump, perm) \
	module_param_array_named(name, name, type, nump, perm)
#define core_param(name, var, type, perm) module_param_named(name, var, type, perm)

#endif
