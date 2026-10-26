/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_LOG2_H
#define _LKPI_LINUX_LOG2_H

#include <linux/types.h>
#include <linux/bitops.h>

static inline __attribute_const__ int __ilog2_u32(u32 n) { return fls(n) - 1; }
static inline __attribute_const__ int __ilog2_u64(u64 n) { return fls64(n) - 1; }

static inline __attribute_const__ bool is_power_of_2(unsigned long n)
{
	return (n != 0 && ((n & (n - 1)) == 0));
}

static inline __attribute_const__ unsigned long __roundup_pow_of_two(unsigned long n)
{
	return 1UL << fls_long(n - 1);
}

static inline __attribute_const__ unsigned long __rounddown_pow_of_two(unsigned long n)
{
	return 1UL << (fls_long(n) - 1);
}

#define ilog2(n) \
	(__builtin_constant_p(n) ? \
		((n) < 2 ? 0 : 63 - __builtin_clzll(n)) : \
		(sizeof(n) <= 4) ? __ilog2_u32(n) : __ilog2_u64(n))
#define roundup_pow_of_two(n) \
	(__builtin_constant_p(n) ? \
		(((n) == 1) ? 1 : (1UL << (ilog2((n) - 1) + 1))) : \
		__roundup_pow_of_two(n))
#define rounddown_pow_of_two(n) \
	(__builtin_constant_p(n) ? (1UL << ilog2(n)) : __rounddown_pow_of_two(n))
#define order_base_2(n) ((n) > 1 ? ilog2((n) - 1) + 1 : 0)
#define bits_per(n) ((n) < 2 ? 1 : ilog2(n) + 1)

static inline int get_count_order(unsigned int count)
{
	if (count == 0)
		return -1;
	return fls(--count);
}
static inline int get_count_order_long(unsigned long l)
{
	if (l == 0UL)
		return -1;
	return (int)fls_long(--l);
}

#endif
