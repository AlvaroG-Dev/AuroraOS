/* SPDX-License-Identifier: GPL-2.0-only */
/* Tamano de linea de cache (x86-64: 64 bytes) y atributos de colocacion. */
#ifndef _LKPI_LINUX_CACHE_H
#define _LKPI_LINUX_CACHE_H

#define L1_CACHE_SHIFT		6
#define L1_CACHE_BYTES		(1 << L1_CACHE_SHIFT)
#define SMP_CACHE_BYTES		L1_CACHE_BYTES
#define SMP_CACHE_ALIGN(x)	(((x) + SMP_CACHE_BYTES - 1) & ~(SMP_CACHE_BYTES - 1))

/* __cacheline_aligned, __read_mostly y __ro_after_init: compiler_types.h */
#ifndef ____cacheline_aligned
#define ____cacheline_aligned		__attribute__((__aligned__(SMP_CACHE_BYTES)))
#define ____cacheline_aligned_in_smp	____cacheline_aligned
#endif

#endif
