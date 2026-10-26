// SPDX-License-Identifier: GPL-2.0-only
/* Busqueda de bits, operaciones de bitmap por rangos y algo de aritmetica. */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/bitmap.h>
#include <linux/slab.h>

/* Primer bit (o, con invert = ~0, primer cero) en [offset, size). */
unsigned long lkpi_find_next_bit(const unsigned long *addr, unsigned long size,
				 unsigned long offset, unsigned long invert)
{
	unsigned long tmp;

	if (offset >= size)
		return size;
	tmp = (addr[offset / BITS_PER_LONG] ^ invert) & BITMAP_FIRST_WORD_MASK(offset);
	offset = round_down(offset, BITS_PER_LONG);
	while (!tmp) {
		offset += BITS_PER_LONG;
		if (offset >= size)
			return size;
		tmp = addr[offset / BITS_PER_LONG] ^ invert;
	}
	return min(offset + __ffs(tmp), size);
}
EXPORT_SYMBOL(lkpi_find_next_bit);

unsigned long lkpi_find_last_bit(const unsigned long *addr, unsigned long size)
{
	if (size) {
		unsigned long val = BITMAP_LAST_WORD_MASK(size);
		unsigned long idx = (size - 1) / BITS_PER_LONG;
		do {
			val &= addr[idx];
			if (val)
				return idx * BITS_PER_LONG + __fls(val);
			val = ~0UL;
		} while (idx--);
	}
	return size;
}
EXPORT_SYMBOL(lkpi_find_last_bit);

/* Indice del n-esimo bit a 1 (desde 0), o size. */
unsigned long lkpi_find_nth_bit(const unsigned long *addr, unsigned long size,
				unsigned long n)
{
	unsigned long bit;
	for_each_set_bit(bit, addr, size)
		if (n-- == 0)
			return bit;
	return size;
}
EXPORT_SYMBOL(lkpi_find_nth_bit);

void lkpi_bitmap_set(unsigned long *map, unsigned int start, unsigned int len)
{
	while (len) {
		unsigned int bit = start % BITS_PER_LONG;
		unsigned int n = min(len, (unsigned int)BITS_PER_LONG - bit);
		unsigned long mask = (n == BITS_PER_LONG ? ~0UL : ((1UL << n) - 1)) << bit;
		map[start / BITS_PER_LONG] |= mask;
		start += n;
		len -= n;
	}
}
EXPORT_SYMBOL(lkpi_bitmap_set);

void lkpi_bitmap_clear(unsigned long *map, unsigned int start, unsigned int len)
{
	while (len) {
		unsigned int bit = start % BITS_PER_LONG;
		unsigned int n = min(len, (unsigned int)BITS_PER_LONG - bit);
		unsigned long mask = (n == BITS_PER_LONG ? ~0UL : ((1UL << n) - 1)) << bit;
		map[start / BITS_PER_LONG] &= ~mask;
		start += n;
		len -= n;
	}
}
EXPORT_SYMBOL(lkpi_bitmap_clear);

unsigned int lkpi_bitmap_weight(const unsigned long *src, unsigned int nbits)
{
	unsigned int k, w = 0, lim = nbits / BITS_PER_LONG;
	for (k = 0; k < lim; k++)
		w += hweight_long(src[k]);
	if (nbits % BITS_PER_LONG)
		w += hweight_long(src[k] & BITMAP_LAST_WORD_MASK(nbits));
	return w;
}
EXPORT_SYMBOL(lkpi_bitmap_weight);

unsigned long *lkpi_bitmap_zalloc(unsigned int nbits, gfp_t flags)
{
	return kcalloc(BITS_TO_LONGS(nbits), sizeof(unsigned long), flags);
}
EXPORT_SYMBOL(lkpi_bitmap_zalloc);

/* Primer hueco de nr ceros alineado a (align_mask + 1), desde start. */
unsigned long lkpi_bitmap_find_next_zero_area(unsigned long *map, unsigned long size,
					      unsigned long start, unsigned int nr,
					      unsigned long align_mask)
{
	unsigned long index, end, i;
again:
	index = find_next_zero_bit(map, size, start);
	index = (index + align_mask) & ~align_mask;
	end = index + nr;
	if (end > size)
		return end;
	i = find_next_bit(map, end, index);
	if (i < end) {
		start = i + 1;
		goto again;
	}
	return index;
}
EXPORT_SYMBOL(lkpi_bitmap_find_next_zero_area);

unsigned long lkpi_int_sqrt(unsigned long x)
{
	unsigned long b, m, y = 0;
	if (x <= 1)
		return x;
	m = 1UL << (__fls(x) & ~1UL);
	while (m != 0) {
		b = y + m;
		y >>= 1;
		if (x >= b) {
			x -= b;
			y += m;
		}
		m >>= 2;
	}
	return y;
}
EXPORT_SYMBOL(lkpi_int_sqrt);

unsigned long lkpi_gcd(unsigned long a, unsigned long b)
{
	while (b) {
		unsigned long t = a % b;
		a = b;
		b = t;
	}
	return a;
}
EXPORT_SYMBOL(lkpi_gcd);
