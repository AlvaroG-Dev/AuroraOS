/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_ARRAY_SIZE_H
#define _LKPI_LINUX_ARRAY_SIZE_H

#include <linux/compiler.h>

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]) + __must_be_array(arr) * 0)

#endif
