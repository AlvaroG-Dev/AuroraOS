/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_ASCII85_H
#define _LKPI_LINUX_ASCII85_H
#include <linux/types.h>
#include <linux/string.h>
#define ASCII85_BUFSZ 6
static inline long ascii85_encode_len(long len) { return (len * 5 + 3) / 4; }
static inline char *ascii85_encode(u32 in, char *out)
{
	int i;

	if (in == 0)
		return memset(out, '!', 5), out[5] = 0, out;
	for (i = 5; i--; ) {
		out[i] = '!' + in % 85;
		in /= 85;
	}
	out[5] = 0;
	return out;
}
#endif
