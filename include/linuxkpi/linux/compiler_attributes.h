/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_COMPILER_ATTRIBUTES_H
#define _LKPI_LINUX_COMPILER_ATTRIBUTES_H

#define __alias(sym) __attribute__((__alias__(#sym)))
#define __aligned(x) __attribute__((__aligned__(x)))
#define __aligned_largest __attribute__((__aligned__))
#define __always_inline inline __attribute__((__always_inline__))
#define __assume_aligned(a, ...) __attribute__((__assume_aligned__(a, ##__VA_ARGS__)))
#define __cold __attribute__((__cold__))
#define __hot __attribute__((__hot__))
#define __attribute_const__ __attribute__((__const__))
#define __copy(sym) __attribute__((__copy__(sym)))
#define __counted_by(m)
#define __deprecated
#define __designated_init
#define __flatten __attribute__((flatten))
#define __gnu_inline __attribute__((__gnu_inline__))
#define __malloc __attribute__((__malloc__))
#define __mode(x) __attribute__((__mode__(x)))
#define __must_check __attribute__((__warn_unused_result__))
#define __noclone __attribute__((__noclone__))
#define __nonstring __attribute__((__nonstring__))
#define noinline __attribute__((__noinline__))
#define __noreturn __attribute__((__noreturn__))
#define __packed __attribute__((__packed__))
#define __pure __attribute__((__pure__))
#define __section(s) __attribute__((__section__(s)))
#define __always_unused __attribute__((__unused__))
#define __maybe_unused __attribute__((__unused__))
#define __used __attribute__((__used__))
#define __weak __attribute__((__weak__))
#define __printf(a, b) __attribute__((__format__(printf, a, b)))
#define __scanf(a, b) __attribute__((__format__(scanf, a, b)))
#define __visible __attribute__((__externally_visible__))
#define __alloc_size(x, ...) __attribute__((__alloc_size__(x, ##__VA_ARGS__)))
#define __realloc_size(x, ...) __attribute__((__alloc_size__(x, ##__VA_ARGS__)))
#define fallthrough __attribute__((__fallthrough__))
#define __cleanup(func) __attribute__((__cleanup__(func)))
#define __no_sanitize_address
#define __no_kcsan
#define __no_sanitize_or_inline __always_inline
#define __noinline_for_stack noinline

#endif
