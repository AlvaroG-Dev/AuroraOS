/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_COMPILER_H
#define _LKPI_LINUX_COMPILER_H

#include <linux/compiler_types.h>

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define likely_notrace(x) likely(x)
#define unlikely_notrace(x) unlikely(x)

#define barrier() __asm__ __volatile__("" : : : "memory")
#define barrier_data(ptr) __asm__ __volatile__("" : : "r"(ptr) : "memory")
#define unreachable() __builtin_unreachable()

#define RELOC_HIDE(ptr, off) \
	({ unsigned long __ptr; \
	   __asm__("" : "=r"(__ptr) : "0"(ptr)); \
	   (typeof(ptr))(__ptr + (off)); })
#define OPTIMIZER_HIDE_VAR(var) __asm__("" : "=r"(var) : "0"(var))

#define __UNIQUE_ID(prefix) __PASTE(__PASTE(__UNIQUE_ID_, prefix), __COUNTER__)
#define ___PASTE(a, b) a##b
#define __PASTE(a, b) ___PASTE(a, b)

/* &a[0] degrada a puntero: si a ya es puntero, no es un array. */
#define __must_be_array(a) \
	(sizeof(struct { int:(-!!__same_type((a), &(a)[0])); }))
#define __must_be_cstr(p) 0

#define data_race(expr) (expr)

/*
 * READ_ONCE/WRITE_ONCE: acceso volatil de un escalar nativo (sin rasgado ni
 * reordenacion por el compilador). Los tamanos no nativos se copian tal cual.
 */
#define __READ_ONCE(x) (*(const volatile __unqual_scalar_typeof(x) *)&(x))
#define READ_ONCE(x) \
	({ compiletime_assert(__native_word(x) || sizeof(x) == sizeof(long long), \
			      "READ_ONCE de tamano no nativo"); \
	   (typeof(x))__READ_ONCE(x); })
#define __WRITE_ONCE(x, val) \
	do { *(volatile typeof(x) *)&(x) = (val); } while (0)
#define WRITE_ONCE(x, val) \
	do { compiletime_assert(__native_word(x) || sizeof(x) == sizeof(long long), \
				"WRITE_ONCE de tamano no nativo"); \
	     __WRITE_ONCE(x, val); } while (0)

#define prefetch(x) __builtin_prefetch(x)
#define prefetchw(x) __builtin_prefetch(x, 1)

#endif
