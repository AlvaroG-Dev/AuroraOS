/* SPDX-License-Identifier: GPL-2.0-only */
/* IS_ENABLED() y familia. Se incluye siempre (-include) como en Linux. */
#ifndef _LKPI_LINUX_KCONFIG_H
#define _LKPI_LINUX_KCONFIG_H

#include <lkpi/config.h>

/*
 * IS_ENABLED(CONFIG_FOO) vale 1 si CONFIG_FOO esta definido a 1 (o
 * CONFIG_FOO_MODULE a 1), y 0 si no esta definido. Mismo truco del
 * preprocesador que usa Linux: el marcador __ARG_PLACEHOLDER_1 inserta un
 * argumento extra cuando la macro vale 1.
 */
#define __ARG_PLACEHOLDER_1 0,
#define __take_second_arg(__ignored, val, ...) val
#define __is_defined(x) ___is_defined(x)
#define ___is_defined(val) ____is_defined(__ARG_PLACEHOLDER_##val)
#define ____is_defined(arg1_or_junk) __take_second_arg(arg1_or_junk 1, 0)
#define __or(x, y) ___or(x, y)
#define ___or(x, y) ____or(__ARG_PLACEHOLDER_##x, y)
#define ____or(arg1_or_junk, y) __take_second_arg(arg1_or_junk 1, y)

#define IS_BUILTIN(option) __is_defined(option)
#define IS_MODULE(option) __is_defined(option##_MODULE)
#define IS_REACHABLE(option) __or(IS_BUILTIN(option), IS_MODULE(option))
#define IS_ENABLED(option) __or(IS_BUILTIN(option), IS_MODULE(option))

#endif
