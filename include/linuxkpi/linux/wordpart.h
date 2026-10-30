/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_WORDPART_H
#define _LKPI_LINUX_WORDPART_H
#include <linux/kernel.h>
#ifndef upper_16_bits
#define upper_16_bits(n) ((u16)((n) >> 16))
#define lower_16_bits(n) ((u16)((n) & 0xffff))
#endif
#endif
