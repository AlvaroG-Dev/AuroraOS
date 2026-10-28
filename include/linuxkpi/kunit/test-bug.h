/* SPDX-License-Identifier: GPL-2.0-only */
/* KUnit no existe en la capa: ningun codigo se ejecuta dentro de una prueba KUnit. */
#ifndef _LKPI_KUNIT_TEST_BUG_H
#define _LKPI_KUNIT_TEST_BUG_H

#include <linux/stddef.h>

#define kunit_get_current_test() ((void *)NULL)
#define kunit_fail_current_test(fmt, ...) do { } while (0)
#define kunit_is_running() 0

#endif
