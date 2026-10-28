/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_MATH_H
#define _LKPI_LINUX_MATH_H

#include <linux/types.h>

#define __round_mask(x, y) ((__typeof__(x))((y) - 1))
#define round_up(x, y) ((((x) - 1) | __round_mask(x, y)) + 1)
#define round_down(x, y) ((x) & ~__round_mask(x, y))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define DIV_ROUND_DOWN_ULL(ll, d) ({ unsigned long long _tmp = (ll); _tmp / (d); })
#define DIV_ROUND_UP_ULL(ll, d) DIV_ROUND_DOWN_ULL((unsigned long long)(ll) + (d) - 1, (d))
#define DIV_ROUND_UP_SECTOR_T(ll, d) DIV_ROUND_UP_ULL(ll, d)
#define roundup(x, y) ({ typeof(y) __y = y; (((x) + (__y - 1)) / __y) * __y; })
#define rounddown(x, y) ({ typeof(x) __x = (x); __x - (__x % (y)); })
#define DIV_ROUND_CLOSEST(x, divisor) \
	({ typeof(x) __x = x; typeof(divisor) __d = divisor; \
	   (((typeof(x))-1) > 0 || ((typeof(divisor))-1) > 0 || \
	    (((__x) > 0) == ((__d) > 0))) ? \
		(((__x) + ((__d) / 2)) / (__d)) : \
		(((__x) - ((__d) / 2)) / (__d)); })
#define DIV_ROUND_CLOSEST_ULL(x, divisor) \
	({ typeof(divisor) __d = divisor; unsigned long long _tmp = (x) + (__d) / 2; \
	   _tmp / __d; })
#define mult_frac(x, numer, denom) \
	({ typeof(x) quot = (x) / (denom); typeof(x) rem = (x) % (denom); \
	   (quot * (numer)) + ((rem * (numer)) / (denom)); })
#define sector_div(a, b) do_div(a, b)
#define abs(x) __builtin_choose_expr(sizeof(x) == sizeof(long long), \
	__builtin_llabs(x), __builtin_abs(x))
#define abs_diff(a, b) ({ typeof(a) __a = (a); typeof(b) __b = (b); \
	(void)(&__a == &__b); __a > __b ? (__a - __b) : (__b - __a); })
#define reciprocal_scale(val, ep_ro) ((u32)(((u64)(val) * (ep_ro)) >> 32))

/* do_div(n, base): n /= base y devuelve el resto (n de 64 bits). */
#define do_div(n, base) \
	({ u32 __base = (base); u32 __rem = (u32)((n) % __base); (n) = (n) / __base; __rem; })

static inline u64 div_u64_rem(u64 dividend, u32 divisor, u32 *remainder)
{
	*remainder = dividend % divisor;
	return dividend / divisor;
}
static inline s64 div_s64_rem(s64 dividend, s32 divisor, s32 *remainder)
{
	*remainder = dividend % divisor;
	return dividend / divisor;
}
static inline u64 div64_u64_rem(u64 dividend, u64 divisor, u64 *remainder)
{
	*remainder = dividend % divisor;
	return dividend / divisor;
}
static inline u64 div_u64(u64 dividend, u32 divisor) { return dividend / divisor; }
static inline s64 div_s64(s64 dividend, s32 divisor) { return dividend / divisor; }
static inline u64 div64_u64(u64 dividend, u64 divisor) { return dividend / divisor; }
static inline s64 div64_s64(s64 dividend, s64 divisor) { return dividend / divisor; }
static inline u64 div64_ul(u64 dividend, unsigned long divisor) { return dividend / divisor; }
#define div64_long(x, y) div64_s64((x), (y))
#define div64_u64_round_up(dividend, divisor) \
	({ u64 _d = (divisor); div64_u64((dividend) + _d - 1, _d); })
#define DIV64_U64_ROUND_UP(ll, d) div64_u64_round_up(ll, d)
#define DIV64_U64_ROUND_CLOSEST(dividend, divisor) \
	({ u64 _tmp = (divisor); div64_u64((dividend) + _tmp / 2, _tmp); })
#define DIV_U64_ROUND_CLOSEST(dividend, divisor) \
	({ u32 _tmp = (divisor); div_u64((u64)(dividend) + _tmp / 2, _tmp); })

static inline u64 mul_u32_u32(u32 a, u32 b) { return (u64)a * b; }
static inline u64 mul_u64_u32_shr(u64 a, u32 mul, unsigned int shift)
{
	return (u64)(((unsigned __int128)a * mul) >> shift);
}
static inline u64 mul_u64_u64_shr(u64 a, u64 mul, unsigned int shift)
{
	return (u64)(((unsigned __int128)a * mul) >> shift);
}
/* a * b / c con producto de 128 bits (mulq + divq, como x86 en Linux; el
 * cociente debe caber en 64 bits). Sin __int128 / divisiones de libgcc. */
static inline u64 mul_u64_u64_div_u64(u64 a, u64 b, u64 c)
{
	u64 q;
	__asm__("mulq %2; divq %3" : "=a"(q) : "a"(a), "rm"(b), "rm"(c) : "rdx");
	return q;
}
static inline u64 mul_u64_u32_div(u64 a, u32 mul, u32 divisor)
{
	return mul_u64_u64_div_u64(a, mul, divisor);
}

unsigned long lkpi_int_sqrt(unsigned long x);
#define int_sqrt(x) lkpi_int_sqrt(x)
static inline u32 int_sqrt64(u64 x) { return (u32)lkpi_int_sqrt(x); }
unsigned long lkpi_gcd(unsigned long a, unsigned long b);
#define gcd(a, b) lkpi_gcd(a, b)
static inline unsigned long lcm(unsigned long a, unsigned long b)
{
	return (a && b) ? (a / gcd(a, b)) * b : 0;
}


u64 lkpi_int_pow(u64 base, unsigned int exp);
unsigned int lkpi_intlog10(u32 x);
#define int_pow lkpi_int_pow
#define intlog10 lkpi_intlog10
#endif
