/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_BITS_H
#define _LKPI_LINUX_BITS_H

#include <linux/const.h>
#include <linux/build_bug.h>

#define BITS_PER_LONG 64
#define BITS_PER_LONG_LONG 64
#define BITS_PER_BYTE 8
#define BITS_PER_TYPE(type) (sizeof(type) * BITS_PER_BYTE)

#define BIT(nr) (UL(1) << (nr))
#define BIT_ULL(nr) (ULL(1) << (nr))
#define BIT_MASK(nr) (UL(1) << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr) ((nr) / BITS_PER_LONG)
#define BIT_ULL_MASK(nr) (ULL(1) << ((nr) % BITS_PER_LONG_LONG))
#define BIT_ULL_WORD(nr) ((nr) / BITS_PER_LONG_LONG)

#define GENMASK_INPUT_CHECK(h, l) \
	(BUILD_BUG_ON_ZERO(__builtin_choose_expr(__is_constexpr((l) > (h)), (l) > (h), 0)))
#define __GENMASK(h, l) \
	(((~UL(0)) - (UL(1) << (l)) + 1) & (~UL(0) >> (BITS_PER_LONG - 1 - (h))))
#define __GENMASK_ULL(h, l) \
	(((~ULL(0)) - (ULL(1) << (l)) + 1) & (~ULL(0) >> (BITS_PER_LONG_LONG - 1 - (h))))
#define GENMASK(h, l) (GENMASK_INPUT_CHECK(h, l) + __GENMASK(h, l))
#define GENMASK_ULL(h, l) (GENMASK_INPUT_CHECK(h, l) + __GENMASK_ULL(h, l))
#define GENMASK_U8(h, l) ((u8)GENMASK(h, l))
#define GENMASK_U16(h, l) ((u16)GENMASK(h, l))
#define GENMASK_U32(h, l) ((u32)GENMASK(h, l))
#define GENMASK_U64(h, l) ((u64)GENMASK_ULL(h, l))
#define BIT_U8(b) ((u8)BIT(b))
#define BIT_U16(b) ((u16)BIT(b))
#define BIT_U32(b) ((u32)BIT(b))
#define BIT_U64(b) ((u64)BIT_ULL(b))

#endif
