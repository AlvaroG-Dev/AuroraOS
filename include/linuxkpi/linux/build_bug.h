/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_BUILD_BUG_H
#define _LKPI_LINUX_BUILD_BUG_H

#include <linux/compiler.h>

#define BUILD_BUG_ON_ZERO(e) ((int)(sizeof(struct { int:(-!!(e)); })))
#define BUILD_BUG_ON_MSG(cond, msg) compiletime_assert(!(cond), msg)
#define BUILD_BUG_ON(condition) \
	BUILD_BUG_ON_MSG(condition, "BUILD_BUG_ON fallo: " #condition)
#define BUILD_BUG_ON_NOT_POWER_OF_2(n) \
	BUILD_BUG_ON((n) == 0 || (((n) & ((n) - 1)) != 0))
#define BUILD_BUG() BUILD_BUG_ON_MSG(1, "BUILD_BUG")
#define BUILD_BUG_ON_INVALID(e) ((void)(sizeof((long)(e))))

#define static_assert(expr, ...) __static_assert(expr, ##__VA_ARGS__, #expr)
#define __static_assert(expr, msg, ...) _Static_assert(expr, msg)

#endif
