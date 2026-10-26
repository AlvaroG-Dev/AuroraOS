/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_BITMAP_H
#define _LKPI_LINUX_BITMAP_H

#include <linux/bitops.h>
#include <linux/string.h>
#include <linux/gfp_types.h>

#define BITMAP_FIRST_WORD_MASK(start) (~0UL << ((start) & (BITS_PER_LONG - 1)))
#define BITMAP_LAST_WORD_MASK(nbits) (~0UL >> (-(nbits) & (BITS_PER_LONG - 1)))
#define DIV_ROUND_UP_U32(n) (((n) + 31) / 32)
#define bitmap_size(nbits) (BITS_TO_LONGS(nbits) * sizeof(unsigned long))

void lkpi_bitmap_set(unsigned long *map, unsigned int start, unsigned int len);
void lkpi_bitmap_clear(unsigned long *map, unsigned int start, unsigned int len);
unsigned int lkpi_bitmap_weight(const unsigned long *src, unsigned int nbits);
unsigned long *lkpi_bitmap_zalloc(unsigned int nbits, gfp_t flags);
unsigned long lkpi_bitmap_find_next_zero_area(unsigned long *map, unsigned long size,
					      unsigned long start, unsigned int nr,
					      unsigned long align_mask);
void lkpi_kfree(const void *p);

static inline void bitmap_zero(unsigned long *dst, unsigned int nbits)
{
	memset(dst, 0, bitmap_size(nbits));
}
static inline void bitmap_fill(unsigned long *dst, unsigned int nbits)
{
	memset(dst, 0xff, bitmap_size(nbits));
}
static inline void bitmap_copy(unsigned long *dst, const unsigned long *src, unsigned int nbits)
{
	memcpy(dst, src, bitmap_size(nbits));
}

#define __bitmap_op2(name, op) \
static inline void bitmap_##name(unsigned long *dst, const unsigned long *a, \
				 const unsigned long *b, unsigned int nbits) \
{ \
	for (unsigned int k = 0; k < BITS_TO_LONGS(nbits); k++) \
		dst[k] = op; \
}
__bitmap_op2(or, a[k] | b[k])
__bitmap_op2(xor, a[k] ^ b[k])
#undef __bitmap_op2

static inline bool bitmap_and(unsigned long *dst, const unsigned long *a,
			      const unsigned long *b, unsigned int nbits)
{
	unsigned long r = 0;
	unsigned int k, n = BITS_TO_LONGS(nbits);
	for (k = 0; k < n; k++)
		r |= (dst[k] = a[k] & b[k] & (k == n - 1 ? BITMAP_LAST_WORD_MASK(nbits) : ~0UL));
	return r != 0;
}
static inline bool bitmap_andnot(unsigned long *dst, const unsigned long *a,
				 const unsigned long *b, unsigned int nbits)
{
	unsigned long r = 0;
	unsigned int k, n = BITS_TO_LONGS(nbits);
	for (k = 0; k < n; k++)
		r |= (dst[k] = a[k] & ~b[k] & (k == n - 1 ? BITMAP_LAST_WORD_MASK(nbits) : ~0UL));
	return r != 0;
}
static inline void bitmap_complement(unsigned long *dst, const unsigned long *src,
				     unsigned int nbits)
{
	for (unsigned int k = 0; k < BITS_TO_LONGS(nbits); k++)
		dst[k] = ~src[k];
}
static inline bool bitmap_equal(const unsigned long *a, const unsigned long *b,
				unsigned int nbits)
{
	unsigned int k, lim = nbits / BITS_PER_LONG;
	for (k = 0; k < lim; k++)
		if (a[k] != b[k])
			return false;
	if (nbits % BITS_PER_LONG)
		return !((a[k] ^ b[k]) & BITMAP_LAST_WORD_MASK(nbits));
	return true;
}
static inline bool bitmap_intersects(const unsigned long *a, const unsigned long *b,
				     unsigned int nbits)
{
	unsigned int k, lim = nbits / BITS_PER_LONG;
	for (k = 0; k < lim; k++)
		if (a[k] & b[k])
			return true;
	if (nbits % BITS_PER_LONG)
		return (a[k] & b[k] & BITMAP_LAST_WORD_MASK(nbits)) != 0;
	return false;
}
static inline bool bitmap_subset(const unsigned long *a, const unsigned long *b,
				 unsigned int nbits)
{
	unsigned int k, lim = nbits / BITS_PER_LONG;
	for (k = 0; k < lim; k++)
		if (a[k] & ~b[k])
			return false;
	if (nbits % BITS_PER_LONG)
		return !(a[k] & ~b[k] & BITMAP_LAST_WORD_MASK(nbits));
	return true;
}
static inline bool bitmap_empty(const unsigned long *src, unsigned int nbits)
{
	return find_first_bit(src, nbits) >= nbits;
}
static inline bool bitmap_full(const unsigned long *src, unsigned int nbits)
{
	return find_first_zero_bit(src, nbits) >= nbits;
}
static inline unsigned int bitmap_weight(const unsigned long *src, unsigned int nbits)
{
	return lkpi_bitmap_weight(src, nbits);
}
static inline void bitmap_set(unsigned long *map, unsigned int start, unsigned int nbits)
{
	lkpi_bitmap_set(map, start, nbits);
}
static inline void bitmap_clear(unsigned long *map, unsigned int start, unsigned int nbits)
{
	lkpi_bitmap_clear(map, start, nbits);
}
static inline unsigned long bitmap_find_next_zero_area(unsigned long *map,
		unsigned long size, unsigned long start, unsigned int nr,
		unsigned long align_mask)
{
	return lkpi_bitmap_find_next_zero_area(map, size, start, nr, align_mask);
}
static inline unsigned long *bitmap_zalloc(unsigned int nbits, gfp_t flags)
{
	return lkpi_bitmap_zalloc(nbits, flags);
}
static inline unsigned long *bitmap_alloc(unsigned int nbits, gfp_t flags)
{
	return lkpi_bitmap_zalloc(nbits, flags);
}
static inline void bitmap_free(const unsigned long *bitmap)
{
	lkpi_kfree(bitmap);
}
static inline void bitmap_from_arr32(unsigned long *bitmap, const u32 *buf,
				     unsigned int nbits)
{
	unsigned int i, halfwords = DIV_ROUND_UP_U32(nbits);
	for (i = 0; i < halfwords; i++) {
		bitmap[i / 2] &= ~(0xffffffffUL << (32 * (i % 2)));
		bitmap[i / 2] |= ((unsigned long)buf[i]) << (32 * (i % 2));
	}
}
static inline void bitmap_to_arr32(u32 *buf, const unsigned long *bitmap,
				   unsigned int nbits)
{
	unsigned int i, halfwords = DIV_ROUND_UP_U32(nbits);
	for (i = 0; i < halfwords; i++)
		buf[i] = (u32)(bitmap[i / 2] >> (32 * (i % 2)));
}
#endif
