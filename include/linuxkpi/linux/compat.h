/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_COMPAT_H
#define _LKPI_LINUX_COMPAT_H
/* Sin ABI de 32 bits: CONFIG_COMPAT=n. */
#include <linux/types.h>
static inline bool in_compat_syscall(void) { return false; }
#endif
