/* SPDX-License-Identifier: GPL-2.0-only */
/* Instrucciones de cache de x86-64 */
#ifndef _LKPI_ASM_SPECIAL_INSNS_H
#define _LKPI_ASM_SPECIAL_INSNS_H

static inline void clflush(volatile void *p)
{
	asm volatile("clflush %0" : "+m" (*(volatile char *)p));
}

/* CLFLUSHOPT (66 0F AE /7). Solo desde Skylake: en CPUs anteriores (Haswell
 * del Portégé R30-A) esa codificacion es #UD, asi que se detecta por CPUID
 * (hoja 7, EBX bit 23) y si no, se usa clflush. */
static inline void clflushopt(volatile void *p)
{
	static int has = -1;

	if (has < 0) {
		unsigned int a = 0, b = 0, c = 0, d = 0, max;

		asm volatile("cpuid" : "=a"(max) : "0"(0) : "ebx", "ecx", "edx");
		if (max >= 7) {
			a = 7; c = 0;
			asm volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
		}
		has = !!(b & (1u << 23));
	}
	if (has)
		asm volatile(".byte 0x66; clflush %0" : "+m" (*(volatile char *)p));
	else
		asm volatile("clflush %0" : "+m" (*(volatile char *)p));
}

#endif
