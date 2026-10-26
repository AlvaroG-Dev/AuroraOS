/* SPDX-License-Identifier: GPL-2.0-only */
/* FIELD_GET/FIELD_PREP y familia: campos de registro definidos por mascara. */
#ifndef _LKPI_LINUX_BITFIELD_H
#define _LKPI_LINUX_BITFIELD_H

#include <linux/build_bug.h>
#include <linux/types.h>

#define __bf_shf(x) (__builtin_ffsll(x) - 1)

#define FIELD_MAX(_mask) ((typeof(_mask))((_mask) >> __bf_shf(_mask)))
#define FIELD_FIT(_mask, _val) \
	(!((((typeof(_mask))(_val)) << __bf_shf(_mask)) & ~(_mask)))
#define FIELD_PREP(_mask, _val) \
	((typeof(_mask))(((typeof(_mask))(_val) << __bf_shf(_mask)) & (_mask)))
#define FIELD_PREP_CONST(_mask, _val) FIELD_PREP(_mask, _val)
#define FIELD_GET(_mask, _reg) \
	((typeof(_mask))(((_reg) & (_mask)) >> __bf_shf(_mask)))
#define FIELD_MODIFY(_mask, _reg_p, _val) \
	({ *(_reg_p) &= ~(_mask); *(_reg_p) |= FIELD_PREP(_mask, _val); })

#define ____MAKE_OP(type, base, to, from) \
static __always_inline __##type type##_encode_bits(base v, base field) \
{ return to((v & (field / (field & -field))) * (field & -field)); } \
static __always_inline __##type type##_replace_bits(__##type old, base val, base field) \
{ return (old & ~to(field)) | type##_encode_bits(val, field); } \
static __always_inline void type##p_replace_bits(__##type *p, base val, base field) \
{ *p = (*p & ~to(field)) | type##_encode_bits(val, field); } \
static __always_inline base type##_get_bits(__##type v, base field) \
{ return (from(v) & field) / (field & -field); }
#define __MAKE_OP(size) \
	____MAKE_OP(le##size, u##size, (__le##size), (u##size)) \
	____MAKE_OP(be##size, u##size, __builtin_bswap##size, __builtin_bswap##size) \
	____MAKE_OP(u##size, u##size, , )
typedef u8 __u8_t;
static __always_inline u8 u8_encode_bits(u8 v, u8 field)
{ return (v & (field / (field & -field))) * (field & -field); }
static __always_inline u8 u8_get_bits(u8 v, u8 field)
{ return (v & field) / (field & -field); }
static __always_inline u8 u8_replace_bits(u8 old, u8 val, u8 field)
{ return (old & ~field) | u8_encode_bits(val, field); }
__MAKE_OP(16)
__MAKE_OP(32)
__MAKE_OP(64)
#undef __MAKE_OP
#undef ____MAKE_OP

#endif
