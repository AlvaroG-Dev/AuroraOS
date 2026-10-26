/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_BITOPS_H
#define _LKPI_LINUX_BITOPS_H

#include <linux/bits.h>
#include <linux/compiler.h>
#include <linux/types.h>
#include <asm/barrier.h>

#define BITS_TO_LONGS(nr) __KERNEL_DIV_ROUND_UP(nr, BITS_PER_TYPE(long))
#define BITS_TO_U64(nr) __KERNEL_DIV_ROUND_UP(nr, BITS_PER_TYPE(u64))
#define BITS_TO_U32(nr) __KERNEL_DIV_ROUND_UP(nr, BITS_PER_TYPE(u32))
#define BITS_TO_BYTES(nr) __KERNEL_DIV_ROUND_UP(nr, BITS_PER_TYPE(char))

/* --- Atomicas (lock-prefijadas via __atomic) --- */
static __always_inline void set_bit(long nr, volatile unsigned long *addr)
{
	__atomic_fetch_or(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST);
}
static __always_inline void clear_bit(long nr, volatile unsigned long *addr)
{
	__atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_SEQ_CST);
}
static __always_inline void change_bit(long nr, volatile unsigned long *addr)
{
	__atomic_fetch_xor(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST);
}
static __always_inline bool test_and_set_bit(long nr, volatile unsigned long *addr)
{
	return __atomic_fetch_or(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST) & BIT_MASK(nr);
}
static __always_inline bool test_and_clear_bit(long nr, volatile unsigned long *addr)
{
	return __atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_SEQ_CST) & BIT_MASK(nr);
}
static __always_inline bool test_and_change_bit(long nr, volatile unsigned long *addr)
{
	return __atomic_fetch_xor(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST) & BIT_MASK(nr);
}
#define set_bit_lock set_bit
#define clear_bit_unlock clear_bit
#define test_and_set_bit_lock test_and_set_bit
#define __clear_bit_unlock clear_bit

/* --- No atomicas --- */
static __always_inline void __set_bit(long nr, volatile unsigned long *addr)
{
	addr[BIT_WORD(nr)] |= BIT_MASK(nr);
}
static __always_inline void __clear_bit(long nr, volatile unsigned long *addr)
{
	addr[BIT_WORD(nr)] &= ~BIT_MASK(nr);
}
static __always_inline void __change_bit(long nr, volatile unsigned long *addr)
{
	addr[BIT_WORD(nr)] ^= BIT_MASK(nr);
}
static __always_inline bool __test_and_set_bit(long nr, volatile unsigned long *addr)
{
	bool old = addr[BIT_WORD(nr)] & BIT_MASK(nr);
	addr[BIT_WORD(nr)] |= BIT_MASK(nr);
	return old;
}
static __always_inline bool __test_and_clear_bit(long nr, volatile unsigned long *addr)
{
	bool old = addr[BIT_WORD(nr)] & BIT_MASK(nr);
	addr[BIT_WORD(nr)] &= ~BIT_MASK(nr);
	return old;
}
static __always_inline bool test_bit(long nr, const volatile unsigned long *addr)
{
	return 1UL & (addr[BIT_WORD(nr)] >> (nr & (BITS_PER_LONG - 1)));
}
#define test_bit_acquire test_bit
#define __assign_bit(nr, addr, value) \
	((value) ? __set_bit(nr, addr) : __clear_bit(nr, addr))
#define assign_bit(nr, addr, value) \
	((value) ? set_bit(nr, addr) : clear_bit(nr, addr))

/* --- Busqueda de bits --- */
static __always_inline unsigned long __ffs(unsigned long word) { return __builtin_ctzl(word); }
static __always_inline unsigned long __fls(unsigned long word) { return 63 - __builtin_clzl(word); }
static __always_inline unsigned long ffz(unsigned long word) { return __ffs(~word); }
static __always_inline int ffs(int x) { return __builtin_ffs(x); }
static __always_inline int fls(unsigned int x) { return x ? 32 - __builtin_clz(x) : 0; }
static __always_inline int fls64(u64 x) { return x ? 64 - __builtin_clzll(x) : 0; }
static __always_inline unsigned int fls_long(unsigned long l) { return fls64(l); }
static __always_inline unsigned long __ffs64(u64 word) { return __builtin_ctzll(word); }

/* Recuento de bits sin __builtin_popcount (evita llamar a libgcc). */
static __always_inline unsigned long __lkpi_hweight64(u64 w)
{
	w -= (w >> 1) & 0x5555555555555555ULL;
	w = (w & 0x3333333333333333ULL) + ((w >> 2) & 0x3333333333333333ULL);
	w = (w + (w >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
	return (w * 0x0101010101010101ULL) >> 56;
}
static __always_inline unsigned int hweight8(unsigned int w) { return __lkpi_hweight64(w & 0xff); }
static __always_inline unsigned int hweight16(unsigned int w) { return __lkpi_hweight64(w & 0xffff); }
static __always_inline unsigned int hweight32(unsigned int w) { return __lkpi_hweight64(w); }
static __always_inline unsigned long hweight64(u64 w) { return __lkpi_hweight64(w); }
static __always_inline unsigned long hweight_long(unsigned long w) { return __lkpi_hweight64(w); }

static inline u64 rol64(u64 w, unsigned int s) { return (w << (s & 63)) | (w >> ((-s) & 63)); }
static inline u64 ror64(u64 w, unsigned int s) { return (w >> (s & 63)) | (w << ((-s) & 63)); }
static inline u32 rol32(u32 w, unsigned int s) { return (w << (s & 31)) | (w >> ((-s) & 31)); }
static inline u32 ror32(u32 w, unsigned int s) { return (w >> (s & 31)) | (w << ((-s) & 31)); }
static inline u16 rol16(u16 w, unsigned int s) { return (w << (s & 15)) | (w >> ((-s) & 15)); }
static inline u16 ror16(u16 w, unsigned int s) { return (w >> (s & 15)) | (w << ((-s) & 15)); }
static inline u8 rol8(u8 w, unsigned int s) { return (w << (s & 7)) | (w >> ((-s) & 7)); }
static inline u8 ror8(u8 w, unsigned int s) { return (w >> (s & 7)) | (w << ((-s) & 7)); }

static __always_inline s32 sign_extend32(u32 value, int index)
{
	u8 shift = 31 - index;
	return (s32)(value << shift) >> shift;
}
static __always_inline s64 sign_extend64(u64 value, int index)
{
	u8 shift = 63 - index;
	return (s64)(value << shift) >> shift;
}

#include <linux/find.h>

#define for_each_set_bit(bit, addr, size) \
	for ((bit) = 0; (bit) = find_next_bit((addr), (size), (bit)), (bit) < (size); (bit)++)
#define for_each_set_bit_from(bit, addr, size) \
	for (; (bit) = find_next_bit((addr), (size), (bit)), (bit) < (size); (bit)++)
#define for_each_clear_bit(bit, addr, size) \
	for ((bit) = 0; (bit) = find_next_zero_bit((addr), (size), (bit)), (bit) < (size); (bit)++)
#define for_each_clear_bit_from(bit, addr, size) \
	for (; (bit) = find_next_zero_bit((addr), (size), (bit)), (bit) < (size); (bit)++)
#define for_each_set_bitrange(b, e, addr, size) \
	for ((b) = 0; (b) = find_next_bit((addr), (size), b), \
	     (e) = find_next_zero_bit((addr), (size), (b) + 1), (b) < (size); (b) = (e) + 1)

#endif
