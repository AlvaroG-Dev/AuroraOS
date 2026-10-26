/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * BUG/WARN. WARN* imprime el aviso con fichero y linea y devuelve la
 * condicion; BUG() detiene el sistema (panic de Aurora).
 */
#ifndef _LKPI_LINUX_BUG_H
#define _LKPI_LINUX_BUG_H

#include <linux/compiler.h>
#include <linux/build_bug.h>
#include <linux/printk.h>

void lkpi_warn_slowpath(const char *file, int line, const char *fmt, ...)
	__printf(3, 4);
void __noreturn lkpi_bug(const char *file, int line);

#define BUG() lkpi_bug(__FILE__, __LINE__)
#define BUG_ON(condition) do { if (unlikely(condition)) BUG(); } while (0)

#define __WARN() lkpi_warn_slowpath(__FILE__, __LINE__, NULL)
#define __WARN_printf(taint, arg...) lkpi_warn_slowpath(__FILE__, __LINE__, arg)

#define WARN_ON(condition) \
	({ int __ret_warn_on = !!(condition); \
	   if (unlikely(__ret_warn_on)) __WARN(); \
	   unlikely(__ret_warn_on); })
#define WARN(condition, format...) \
	({ int __ret_warn_on = !!(condition); \
	   if (unlikely(__ret_warn_on)) __WARN_printf(0, format); \
	   unlikely(__ret_warn_on); })
#define WARN_TAINT(condition, taint, format...) WARN(condition, format)

#define WARN_ON_ONCE(condition) \
	({ static bool __warned; int __ret_warn_once = !!(condition); \
	   if (unlikely(__ret_warn_once && !__warned)) { __warned = true; __WARN(); } \
	   unlikely(__ret_warn_once); })
#define WARN_ONCE(condition, format...) \
	({ static bool __warned; int __ret_warn_once = !!(condition); \
	   if (unlikely(__ret_warn_once && !__warned)) { __warned = true; \
		   __WARN_printf(0, format); } \
	   unlikely(__ret_warn_once); })
#define WARN_TAINT_ONCE(condition, taint, format...) WARN_ONCE(condition, format)

#define BUG_ON_INVALID(e) ((void)(sizeof((long)(e))))

enum bug_trap_type { BUG_TRAP_TYPE_NONE = 0, BUG_TRAP_TYPE_WARN = 1, BUG_TRAP_TYPE_BUG = 2 };

#endif
