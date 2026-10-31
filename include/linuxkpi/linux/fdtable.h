/* SPDX-License-Identifier: GPL-2.0-only */
/* Stub minimo de LinuxKPI (N2: lo justo para compilar kernel-open/ de NVIDIA). */
#ifndef _LKPI_LINUX_FDTABLE_H
#define _LKPI_LINUX_FDTABLE_H
#include <linux/types.h>
struct files_struct;
static inline int iterate_fd(struct files_struct *f, unsigned n, int (*fn)(const void *, struct file *, unsigned), const void *p) { return 0; }
#endif
