/* SPDX-License-Identifier: GPL-2.0-only */
/* Stub minimo de LinuxKPI (N2: lo justo para compilar kernel-open/ de NVIDIA). */
#ifndef _LKPI_LINUX_CPU_H
#define _LKPI_LINUX_CPU_H
#include <linux/types.h>
static inline void native_cpuid(unsigned int *eax, unsigned int *ebx, unsigned int *ecx, unsigned int *edx)
{
	__asm__ volatile("cpuid" : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx) : "0"(*eax), "2"(*ecx));
}
#endif
