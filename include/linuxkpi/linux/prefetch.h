/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_PREFETCH_H
#define _LKPI_LINUX_PREFETCH_H
#define prefetch(x)	__builtin_prefetch(x)
#define prefetchw(x)	__builtin_prefetch(x, 1)
#endif
