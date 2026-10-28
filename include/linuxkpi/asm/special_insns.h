/* SPDX-License-Identifier: GPL-2.0-only */
/* Instrucciones de cache de x86-64 */
#ifndef _LKPI_ASM_SPECIAL_INSNS_H
#define _LKPI_ASM_SPECIAL_INSNS_H

static inline void clflush(volatile void *p)
{
	asm volatile("clflush %0" : "+m" (*(volatile char *)p));
}

/* CLFLUSHOPT (66 0F AE /7); el ensamblador no siempre lo conoce */
static inline void clflushopt(volatile void *p)
{
	asm volatile(".byte 0x66; clflush %0" : "+m" (*(volatile char *)p));
}

#endif
