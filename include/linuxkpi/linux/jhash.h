/* SPDX-License-Identifier: GPL-2.0-only */
/* Hash de Bob Jenkins (lookup3, dominio publico), interfaz de Linux. */
#ifndef _LKPI_LINUX_JHASH_H
#define _LKPI_LINUX_JHASH_H

#include <linux/bitops.h>
#include <linux/unaligned.h>

#define JHASH_INITVAL 0xdeadbeef

#define __jhash_mix(a, b, c) \
	{ a -= c; a ^= rol32(c, 4); c += b; \
	  b -= a; b ^= rol32(a, 6); a += c; \
	  c -= b; c ^= rol32(b, 8); b += a; \
	  a -= c; a ^= rol32(c, 16); c += b; \
	  b -= a; b ^= rol32(a, 19); a += c; \
	  c -= b; c ^= rol32(b, 4); b += a; }
#define __jhash_final(a, b, c) \
	{ c ^= b; c -= rol32(b, 14); \
	  a ^= c; a -= rol32(c, 11); \
	  b ^= a; b -= rol32(a, 25); \
	  c ^= b; c -= rol32(b, 16); \
	  a ^= c; a -= rol32(c, 4); \
	  b ^= a; b -= rol32(a, 14); \
	  c ^= b; c -= rol32(b, 24); }

static inline u32 jhash(const void *key, u32 length, u32 initval)
{
	u32 a, b, c;
	const u8 *k = key;
	a = b = c = JHASH_INITVAL + length + initval;
	while (length > 12) {
		a += get_unaligned_le32(k);
		b += get_unaligned_le32(k + 4);
		c += get_unaligned_le32(k + 8);
		__jhash_mix(a, b, c);
		length -= 12;
		k += 12;
	}
	switch (length) {
	case 12: c += (u32)k[11] << 24; fallthrough;
	case 11: c += (u32)k[10] << 16; fallthrough;
	case 10: c += (u32)k[9] << 8; fallthrough;
	case 9: c += k[8]; fallthrough;
	case 8: b += (u32)k[7] << 24; fallthrough;
	case 7: b += (u32)k[6] << 16; fallthrough;
	case 6: b += (u32)k[5] << 8; fallthrough;
	case 5: b += k[4]; fallthrough;
	case 4: a += (u32)k[3] << 24; fallthrough;
	case 3: a += (u32)k[2] << 16; fallthrough;
	case 2: a += (u32)k[1] << 8; fallthrough;
	case 1: a += k[0];
		__jhash_final(a, b, c);
		break;
	case 0:
		break;
	}
	return c;
}
static inline u32 jhash2(const u32 *k, u32 length, u32 initval)
{
	u32 a, b, c;
	a = b = c = JHASH_INITVAL + (length << 2) + initval;
	while (length > 3) {
		a += k[0];
		b += k[1];
		c += k[2];
		__jhash_mix(a, b, c);
		length -= 3;
		k += 3;
	}
	switch (length) {
	case 3: c += k[2]; fallthrough;
	case 2: b += k[1]; fallthrough;
	case 1: a += k[0];
		__jhash_final(a, b, c);
		break;
	case 0:
		break;
	}
	return c;
}
static inline u32 __jhash_nwords(u32 a, u32 b, u32 c, u32 initval)
{
	a += initval;
	b += initval;
	c += initval;
	__jhash_final(a, b, c);
	return c;
}
static inline u32 jhash_3words(u32 a, u32 b, u32 c, u32 initval)
{
	return __jhash_nwords(a, b, c, initval + JHASH_INITVAL + (3 << 2));
}
static inline u32 jhash_2words(u32 a, u32 b, u32 initval)
{
	return __jhash_nwords(a, b, 0, initval + JHASH_INITVAL + (2 << 2));
}
static inline u32 jhash_1word(u32 a, u32 initval)
{
	return __jhash_nwords(a, 0, 0, initval + JHASH_INITVAL + (1 << 2));
}

#endif
