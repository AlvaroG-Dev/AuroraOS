/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_SWAB_H
#define _LKPI_LINUX_SWAB_H

#include <linux/types.h>

#define swab16(x) ((u16)__builtin_bswap16((u16)(x)))
#define swab32(x) ((u32)__builtin_bswap32((u32)(x)))
#define swab64(x) ((u64)__builtin_bswap64((u64)(x)))
#define __swab16 swab16
#define __swab32 swab32
#define __swab64 swab64
#define ___constant_swab16 swab16
#define ___constant_swab32 swab32
#define ___constant_swab64 swab64
static inline u32 swahw32(u32 x) { return (x << 16) | (x >> 16); }
static inline void swab16s(u16 *p) { *p = swab16(*p); }
static inline void swab32s(u32 *p) { *p = swab32(*p); }
static inline void swab64s(u64 *p) { *p = swab64(*p); }

#endif
