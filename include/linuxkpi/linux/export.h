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

/*
 * El nombre exportado es el simbolo ya expandido: el codigo de Linux que se
 * compila tal cual (dma-fence.c...) usa nombres de Linux que las cabeceras
 * de la capa renombran con macros objeto (dma_fence_signal ->
 * lkpi_dma_fence_signal); el ksymtab lleva el nombre con prefijo.
 */
#define __LKPI_STR(x) #x
#define __LKPI_XSTR(x) __LKPI_STR(x)
#define __LKPI_EXPORT(sym) \
	static const struct lkpi_kur_export_entry __kur_exp_##sym \
	__used __section(".kur_ksymtab") = { __LKPI_XSTR(sym), (u64)(uintptr_t)&(sym) }

#define EXPORT_SYMBOL(sym) __LKPI_EXPORT(sym)
#define EXPORT_SYMBOL_GPL(sym) __LKPI_EXPORT(sym)
#define EXPORT_SYMBOL_NS(sym, ns) __LKPI_EXPORT(sym)
#define EXPORT_SYMBOL_NS_GPL(sym, ns) __LKPI_EXPORT(sym)
#define EXPORT_SYMBOL_GPL_FOR_MODULES(sym, mods) __LKPI_EXPORT(sym)
#define EXPORT_SYMBOL_FOR_TESTS_ONLY(sym) __LKPI_EXPORT(sym)
#define EXPORT_SYMBOL_IF_KUNIT(sym)
#define MODULE_IMPORT_NS(ns)

/*
 * struct module: solo lo que usa la capa. Su comienzo es struct
 * kur_module_ref de kernel/kur.h (el cargador .kur pone magic y nombre y
 * consulta refcnt al descargar). Cada .kur tiene su propio __this_module:
 * definicion debil y oculta en cada unidad, que `ld -r` funde en una.
 */
#define LKPI_MODULE_NAME_LEN 64
struct module {
	int refcnt;		/* >= 0 referencias; < 0 descargando */
	unsigned int magic;
	char name[LKPI_MODULE_NAME_LEN];
};
extern struct module __this_module;
struct module __this_module __attribute__((weak, visibility("hidden")));
#define THIS_MODULE (&__this_module)

#endif
