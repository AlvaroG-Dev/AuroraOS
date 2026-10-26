/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * EXPORT_SYMBOL*: entrada en .kur_ksymtab, que el cargador .kur de Aurora
 * registra en su tabla dinamica (mismo formato que KUR_EXPORT_SYMBOL de
 * kernel/kur.h). _GPL y los espacios de nombres no se distinguen: todo lo
 * que se carga sobre la capa es GPL (Plan_LinuxKPI.md, decisiones).
 */
#ifndef _LKPI_LINUX_EXPORT_H
#define _LKPI_LINUX_EXPORT_H

#include <linux/compiler.h>
#include <linux/types.h>

struct lkpi_kur_export_entry {
	const char *name;
	u64 addr;
};

#define __LKPI_EXPORT(sym) \
	static const struct lkpi_kur_export_entry __kur_exp_##sym \
	__used __section(".kur_ksymtab") = { #sym, (u64)(uintptr_t)&(sym) }

#define EXPORT_SYMBOL(sym) __LKPI_EXPORT(sym)
#define EXPORT_SYMBOL_GPL(sym) __LKPI_EXPORT(sym)
#define EXPORT_SYMBOL_NS(sym, ns) __LKPI_EXPORT(sym)
#define EXPORT_SYMBOL_NS_GPL(sym, ns) __LKPI_EXPORT(sym)
#define EXPORT_SYMBOL_GPL_FOR_MODULES(sym, mods) __LKPI_EXPORT(sym)
#define EXPORT_SYMBOL_FOR_TESTS_ONLY(sym) __LKPI_EXPORT(sym)
#define EXPORT_SYMBOL_IF_KUNIT(sym)
#define MODULE_IMPORT_NS(ns)
#define THIS_MODULE ((struct module *)0)

#endif
