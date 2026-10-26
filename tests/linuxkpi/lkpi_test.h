/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Pruebas unitarias de LinuxKPI dentro de un .kur. Cada modulo de prueba
 * hace sus comprobaciones en module_init y las cuenta en la suite del
 * kernel (test_pass/test_fail de kernel/test.c), bajo el REGISTER_TEST que
 * lo carga (kernel/tests/kernel_tests.c, "linuxkpi: ...").
 */
#ifndef _LKPI_TEST_H
#define _LKPI_TEST_H

#include <linux/kernel.h>
#include <linux/string.h>

void lkpi_ktest_pass(void) __asm__("test_pass");
void lkpi_ktest_fail(const char *file, int line, const char *fmt, ...)
	__asm__("test_fail") __printf(3, 4);

#define KT_ASSERT(cond, ...) \
	do { \
		if (cond) \
			lkpi_ktest_pass(); \
		else \
			lkpi_ktest_fail(__FILE__, __LINE__, __VA_ARGS__); \
	} while (0)

#define KT_EQ(a, b) \
	do { \
		long long __a = (long long)(a), __b = (long long)(b); \
		KT_ASSERT(__a == __b, "%s == %s: %lld != %lld", #a, #b, __a, __b); \
	} while (0)

#define KT_STREQ(a, b) \
	do { \
		const char *__a = (a), *__b = (b); \
		KT_ASSERT(__a && __b && !strcmp(__a, __b), "%s: \"%s\" != \"%s\"", #a, \
			  __a ? __a : "(null)", __b ? __b : "(null)"); \
	} while (0)

/* PRNG determinista para las pruebas (xorshift64). */
static inline u64 kt_rand(u64 *state)
{
	u64 x = *state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return *state = x;
}

#endif
