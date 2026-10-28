/* SPDX-License-Identifier: GPL-2.0-only */
/* Datos de la CPU de arranque (x86-64) que usan los drivers. */
#ifndef _LKPI_ASM_PROCESSOR_H
#define _LKPI_ASM_PROCESSOR_H

#include <linux/types.h>

struct cpuinfo_x86 {
	unsigned int x86_clflush_size;	/* bytes de una linea de cache para CLFLUSH */
	unsigned int x86_cache_alignment;
};

extern struct cpuinfo_x86 lkpi_boot_cpu_data;
#define boot_cpu_data lkpi_boot_cpu_data

#endif
