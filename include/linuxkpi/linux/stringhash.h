/* SPDX-License-Identifier: GPL-2.0-only */
/* Stub minimo de LinuxKPI (N2: lo justo para compilar kernel-open/ de NVIDIA). */
#ifndef _LKPI_LINUX_STRINGHASH_H
#define _LKPI_LINUX_STRINGHASH_H
#include <linux/types.h>
static inline unsigned int full_name_hash(const void *salt, const char *name, unsigned int len)
{
	unsigned int h = (unsigned int)(unsigned long)salt;
	while (len--) h = (h + (unsigned char)*name++) * 0x9e3779b1u;
	return h;
}
#endif
