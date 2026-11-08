#ifndef _LKPI_CRYPTO_UTILS_H
#define _LKPI_CRYPTO_UTILS_H
#include <linux/types.h>
static inline unsigned long crypto_memneq(const void *a, const void *b, size_t size)
{ const u8 *x = a, *y = b; u8 d = 0; while (size--) d |= *x++ ^ *y++; return d; }
static inline void crypto_xor(u8 *dst, const u8 *src, unsigned int size) { while (size--) *dst++ ^= *src++; }
static inline void crypto_xor_cpy(u8 *dst, const u8 *src1, const u8 *src2, unsigned int size) { while (size--) *dst++ = *src1++ ^ *src2++; }
#endif
