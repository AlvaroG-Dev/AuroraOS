/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * atomic_t / atomic64_t / atomic_long_t sobre los __atomic de GCC. Las
 * operaciones con valor de retorno son totalmente ordenadas (SEQ_CST), las
 * sin retorno son relajadas, como en Linux.
 */
#ifndef _LKPI_LINUX_ATOMIC_H
#define _LKPI_LINUX_ATOMIC_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <asm/barrier.h>

#define ATOMIC_INIT(i) { (i) }
#define ATOMIC64_INIT(i) { (i) }
#define ATOMIC_LONG_INIT(i) ATOMIC64_INIT(i)

#define __LKPI_ATOMIC_OPS(pfx, atype, ctype) \
static __always_inline ctype pfx##_read(const atype *v) { return READ_ONCE(v->counter); } \
static __always_inline ctype pfx##_read_acquire(const atype *v) { return __atomic_load_n(&v->counter, __ATOMIC_ACQUIRE); } \
static __always_inline void pfx##_set(atype *v, ctype i) { WRITE_ONCE(v->counter, i); } \
static __always_inline void pfx##_set_release(atype *v, ctype i) { __atomic_store_n(&v->counter, i, __ATOMIC_RELEASE); } \
static __always_inline void pfx##_add(ctype i, atype *v) { __atomic_fetch_add(&v->counter, i, __ATOMIC_RELAXED); } \
static __always_inline void pfx##_sub(ctype i, atype *v) { __atomic_fetch_sub(&v->counter, i, __ATOMIC_RELAXED); } \
static __always_inline void pfx##_inc(atype *v) { pfx##_add(1, v); } \
static __always_inline void pfx##_dec(atype *v) { pfx##_sub(1, v); } \
static __always_inline void pfx##_and(ctype i, atype *v) { __atomic_fetch_and(&v->counter, i, __ATOMIC_RELAXED); } \
static __always_inline void pfx##_or(ctype i, atype *v) { __atomic_fetch_or(&v->counter, i, __ATOMIC_RELAXED); } \
static __always_inline void pfx##_xor(ctype i, atype *v) { __atomic_fetch_xor(&v->counter, i, __ATOMIC_RELAXED); } \
static __always_inline void pfx##_andnot(ctype i, atype *v) { __atomic_fetch_and(&v->counter, ~i, __ATOMIC_RELAXED); } \
static __always_inline ctype pfx##_add_return(ctype i, atype *v) { return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline ctype pfx##_sub_return(ctype i, atype *v) { return __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline ctype pfx##_inc_return(atype *v) { return pfx##_add_return(1, v); } \
static __always_inline ctype pfx##_dec_return(atype *v) { return pfx##_sub_return(1, v); } \
static __always_inline ctype pfx##_fetch_add(ctype i, atype *v) { return __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline ctype pfx##_fetch_sub(ctype i, atype *v) { return __atomic_fetch_sub(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline ctype pfx##_fetch_inc(atype *v) { return pfx##_fetch_add(1, v); } \
static __always_inline ctype pfx##_fetch_dec(atype *v) { return pfx##_fetch_sub(1, v); } \
static __always_inline ctype pfx##_fetch_and(ctype i, atype *v) { return __atomic_fetch_and(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline ctype pfx##_fetch_or(ctype i, atype *v) { return __atomic_fetch_or(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline ctype pfx##_fetch_xor(ctype i, atype *v) { return __atomic_fetch_xor(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline ctype pfx##_fetch_andnot(ctype i, atype *v) { return __atomic_fetch_and(&v->counter, ~i, __ATOMIC_SEQ_CST); } \
static __always_inline ctype pfx##_xchg(atype *v, ctype n) { return __atomic_exchange_n(&v->counter, n, __ATOMIC_SEQ_CST); } \
static __always_inline ctype pfx##_cmpxchg(atype *v, ctype o, ctype n) \
{ __atomic_compare_exchange_n(&v->counter, &o, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return o; } \
static __always_inline bool pfx##_try_cmpxchg(atype *v, ctype *o, ctype n) \
{ return __atomic_compare_exchange_n(&v->counter, o, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); } \
static __always_inline bool pfx##_sub_and_test(ctype i, atype *v) { return pfx##_sub_return(i, v) == 0; } \
static __always_inline bool pfx##_dec_and_test(atype *v) { return pfx##_dec_return(v) == 0; } \
static __always_inline bool pfx##_inc_and_test(atype *v) { return pfx##_inc_return(v) == 0; } \
static __always_inline bool pfx##_add_negative(ctype i, atype *v) { return pfx##_add_return(i, v) < 0; } \
static __always_inline ctype pfx##_fetch_add_unless(atype *v, ctype a, ctype u) \
{ ctype c = pfx##_read(v); do { if (unlikely(c == u)) break; } while (!pfx##_try_cmpxchg(v, &c, c + a)); return c; } \
static __always_inline bool pfx##_add_unless(atype *v, ctype a, ctype u) { return pfx##_fetch_add_unless(v, a, u) != u; } \
static __always_inline bool pfx##_inc_not_zero(atype *v) { return pfx##_add_unless(v, 1, 0); } \
static __always_inline ctype pfx##_dec_if_positive(atype *v) \
{ ctype c = pfx##_read(v), d; do { d = c - 1; if (unlikely(d < 0)) break; } while (!pfx##_try_cmpxchg(v, &c, d)); return d; }

__LKPI_ATOMIC_OPS(atomic, atomic_t, int)
__LKPI_ATOMIC_OPS(atomic64, atomic64_t, s64)
__LKPI_ATOMIC_OPS(atomic_long, atomic_long_t, long)
#undef __LKPI_ATOMIC_OPS

#define atomic_add_return_relaxed atomic_add_return
#define atomic_inc_return_relaxed atomic_inc_return
#define atomic_cmpxchg_relaxed atomic_cmpxchg
#define atomic64_inc_return_relaxed atomic64_inc_return
#define atomic_set_mask(m, v) atomic_or(m, v)
#define atomic_clear_mask(m, v) atomic_andnot(m, v)

/* xchg/cmpxchg sobre escalares cualesquiera. */
#define xchg(ptr, v) __atomic_exchange_n((ptr), (v), __ATOMIC_SEQ_CST)
#define cmpxchg(ptr, o, n) \
	({ typeof(*(ptr)) __o = (o); \
	   __atomic_compare_exchange_n((ptr), &__o, (n), false, \
				       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); __o; })
#define try_cmpxchg(ptr, po, n) \
	__atomic_compare_exchange_n((ptr), (po), (n), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define cmpxchg_relaxed cmpxchg
#define cmpxchg_acquire cmpxchg
#define cmpxchg_release cmpxchg
#define cmpxchg64 cmpxchg
#define cmpxchg64_relaxed cmpxchg
#define try_cmpxchg64 try_cmpxchg
#define try_cmpxchg_relaxed try_cmpxchg
#define xchg_relaxed xchg
#define xchg_acquire xchg
#define xchg_release xchg

#endif
