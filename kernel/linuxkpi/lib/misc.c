// SPDX-License-Identifier: GPL-2.0-only
/*
 * Utilidades pequenas que i915 usa y que en Linux viven en lib/: logaritmo
 * entero en base 10 (formato 8.24), potencia entera, volcado hexadecimal a un
 * buffer, desplazamientos y regiones de bitmap.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/bitmap.h>
#include <linux/math.h>
#include <linux/string.h>

u64 lkpi_int_pow(u64 base, unsigned int exp)
{
	u64 result = 1;

	while (exp) {
		if (exp & 1)
			result *= base;
		exp >>= 1;
		base *= base;
	}
	return result;
}
EXPORT_SYMBOL(lkpi_int_pow);

/* log2(x) en formato 8.24 por el metodo de elevar al cuadrado la mantisa. */
static unsigned int lkpi_intlog2_fp(u32 x)
{
	unsigned int msb, frac = 0;
	u64 m;

	if (!x)
		return 0;
	msb = 31 - __builtin_clz(x);
	/* mantisa en [1,2) como punto fijo 1.31 */
	m = ((u64)x << (31 - msb)) & 0xffffffffULL;
	m |= 1ULL << 31;
	for (int i = 0; i < 24; i++) {
		m = (m * m) >> 31;
		frac <<= 1;
		if (m >= (2ULL << 31)) {
			m >>= 1;
			frac |= 1;
		}
	}
	return (msb << 24) | frac;
}

/* log10(x) * 2^24, como intlog10() de Linux. */
unsigned int lkpi_intlog10(u32 x)
{
	/* log10(2) en 0.32: 0.30102999566 * 2^32 */
	return (unsigned int)(((u64)lkpi_intlog2_fp(x) * 1292913986ULL) >> 32);
}
EXPORT_SYMBOL(lkpi_intlog10);

int lkpi_hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize, char *linebuf,
			    size_t linebuflen, bool ascii)
{
	const u8 *ptr = buf;
	int ngroups, lx = 0, j;
	size_t ascii_column;

	if (rowsize != 16 && rowsize != 32)
		rowsize = 16;
	if (len > (size_t)rowsize)
		len = rowsize;
	if (groupsize != 1 && groupsize != 2 && groupsize != 4 && groupsize != 8)
		groupsize = 1;
	if (len % groupsize)
		groupsize = 1;
	ngroups = len / groupsize;
	if (!linebuflen)
		return 0;
	for (j = 0; j < ngroups; j++) {
		for (int b = groupsize - 1; b >= 0; b--) {
			u8 v = ptr[j * groupsize + b];

			if (lx + 2 < (int)linebuflen) {
				linebuf[lx] = hex_asc_hi(v);
				linebuf[lx + 1] = hex_asc_lo(v);
			}
			lx += 2;
		}
		if (j + 1 < ngroups || ascii) {
			if (lx + 1 < (int)linebuflen)
				linebuf[lx] = ' ';
			lx++;
		}
	}
	if (ascii) {
		ascii_column = 2 * rowsize + rowsize / groupsize + 1;
		while ((size_t)lx < ascii_column) {
			if (lx + 1 < (int)linebuflen)
				linebuf[lx] = ' ';
			lx++;
		}
		for (j = 0; j < (int)len; j++) {
			u8 c = ptr[j];

			if (lx + 1 < (int)linebuflen)
				linebuf[lx] = (c >= 32 && c < 127) ? c : '.';
			lx++;
		}
	}
	linebuf[lx < (int)linebuflen ? lx : (int)linebuflen - 1] = '\0';
	return lx;
}
EXPORT_SYMBOL(lkpi_hex_dump_to_buffer);

void lkpi_bitmap_shift_right(unsigned long *dst, const unsigned long *src, unsigned int shift, unsigned int nbits)
{
	unsigned long tmp[BITS_TO_LONGS(256)];
	unsigned int words = BITS_TO_LONGS(nbits);

	/* nbits <= 256 en i915 (mascaras de DSS); si no, el bucle por bit sigue valiendo. */
	if (words > ARRAY_SIZE(tmp)) {
		for (unsigned int i = 0; i < nbits; i++) {
			bool v = i + shift < nbits && test_bit(i + shift, src);

			if (v)
				__set_bit(i, dst);
			else
				__clear_bit(i, dst);
		}
		return;
	}
	memset(tmp, 0, words * sizeof(long));
	for (unsigned int i = 0; i + shift < nbits; i++)
		if (test_bit(i + shift, src))
			__set_bit(i, tmp);
	memcpy(dst, tmp, words * sizeof(long));
}
EXPORT_SYMBOL(lkpi_bitmap_shift_right);

int lkpi_bitmap_find_free_region(unsigned long *bitmap, unsigned int bits, int order)
{
	unsigned int pos, end;

	for (pos = 0; (end = pos + (1U << order)) <= bits; pos = end) {
		unsigned int i;

		for (i = pos; i < end; i++)
			if (test_bit(i, bitmap))
				break;
		if (i < end)
			continue;
		for (i = pos; i < end; i++)
			__set_bit(i, bitmap);
		return pos;
	}
	return -ENOMEM;
}
EXPORT_SYMBOL(lkpi_bitmap_find_free_region);

void lkpi_bitmap_release_region(unsigned long *bitmap, unsigned int pos, int order)
{
	for (unsigned int i = pos; i < pos + (1U << order); i++)
		__clear_bit(i, bitmap);
}
EXPORT_SYMBOL(lkpi_bitmap_release_region);

#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sysctl.h>
#include <linux/pm.h>

const char lkpi_power_group_name[] = "power";
EXPORT_SYMBOL(lkpi_power_group_name);

const int lkpi_sysctl_zero = 0;
const int lkpi_sysctl_one = 1;
EXPORT_SYMBOL(lkpi_sysctl_zero);
EXPORT_SYMBOL(lkpi_sysctl_one);

void *lkpi_strndup_user(const char __user *s, long n)
{
	char *p;
	long len;

	if (n < 1)
		return ERR_PTR(-EINVAL);
	p = kmalloc(n + 1, GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);
	len = strncpy_from_user(p, s, n + 1);
	if (len < 0) {
		kfree(p);
		return ERR_PTR(len);
	}
	if (len > n) {
		kfree(p);
		return ERR_PTR(-EINVAL);
	}
	p[len] = '\0';
	return p;
}
EXPORT_SYMBOL(lkpi_strndup_user);
