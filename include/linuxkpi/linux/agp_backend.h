/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_AGP_BACKEND_H
#define _LKPI_LINUX_AGP_BACKEND_H
/* Solo las constantes de tipo de memoria: la GMCH GTT (Gen2-5, intel-gtt.c) no esta soportada (L7). */
#define AGP_USER_MEMORY		(0x00000001 + 1)
#define AGP_USER_CACHED_MEMORY	(0x00000001 + 2)
#endif
