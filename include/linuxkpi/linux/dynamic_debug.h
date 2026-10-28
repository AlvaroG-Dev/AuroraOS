/* SPDX-License-Identifier: GPL-2.0-only */
/* dynamic debug: sin CONFIG_DYNAMIC_DEBUG, pr_debug/dev_dbg los resuelve printk.h. */
#ifndef _LKPI_LINUX_DYNAMIC_DEBUG_H
#define _LKPI_LINUX_DYNAMIC_DEBUG_H

struct _ddebug;

enum class_map_type {
	DD_CLASS_TYPE_DISJOINT_BITS,
	DD_CLASS_TYPE_LEVEL_NUM,
	DD_CLASS_TYPE_DISJOINT_NAMES,
	DD_CLASS_TYPE_LEVEL_NAMES,
};

#define DECLARE_DYNDBG_CLASSMAP(_var, _maptype, _base, ...)
#define DYNDBG_CLASSMAP_PARAM(_name, _var, _flags)
#define DYNDBG_CLASSMAP_DEFINE(_var, _maptype, _base, ...)
#define DYNDBG_CLASSMAP_USE(_var)

#endif
