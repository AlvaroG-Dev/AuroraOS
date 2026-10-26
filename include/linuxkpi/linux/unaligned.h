/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_UNALIGNED_H
#define _LKPI_LINUX_UNALIGNED_H

#include <linux/types.h>
#include <linux/byteorder/generic.h>

#define __get_unaligned_t(type, ptr) \
	({ const struct { type x; } __attribute__((__packed__)) *__pptr = \
		(typeof(__pptr))(ptr); __pptr->x; })
#define __put_unaligned_t(type, val, ptr) \
	do { struct { type x; } __attribute__((__packed__)) *__pptr = \
		(typeof(__pptr))(ptr); __pptr->x = (val); } while (0)
#define get_unaligned(ptr) __get_unaligned_t(typeof(*(ptr)), (ptr))
#define put_unaligned(val, ptr) __put_unaligned_t(typeof(*(ptr)), (val), (ptr))

static inline u16 get_unaligned_le16(const void *p) { return le16_to_cpu(__get_unaligned_t(__le16, p)); }
static inline u32 get_unaligned_le32(const void *p) { return le32_to_cpu(__get_unaligned_t(__le32, p)); }
static inline u64 get_unaligned_le64(const void *p) { return le64_to_cpu(__get_unaligned_t(__le64, p)); }
static inline void put_unaligned_le16(u16 val, void *p) { __put_unaligned_t(__le16, cpu_to_le16(val), p); }
static inline void put_unaligned_le32(u32 val, void *p) { __put_unaligned_t(__le32, cpu_to_le32(val), p); }
static inline void put_unaligned_le64(u64 val, void *p) { __put_unaligned_t(__le64, cpu_to_le64(val), p); }
static inline u16 get_unaligned_be16(const void *p) { return be16_to_cpu(__get_unaligned_t(__be16, p)); }
static inline u32 get_unaligned_be32(const void *p) { return be32_to_cpu(__get_unaligned_t(__be32, p)); }
static inline u64 get_unaligned_be64(const void *p) { return be64_to_cpu(__get_unaligned_t(__be64, p)); }
static inline void put_unaligned_be16(u16 val, void *p) { __put_unaligned_t(__be16, cpu_to_be16(val), p); }
static inline void put_unaligned_be32(u32 val, void *p) { __put_unaligned_t(__be32, cpu_to_be32(val), p); }
static inline void put_unaligned_be64(u64 val, void *p) { __put_unaligned_t(__be64, cpu_to_be64(val), p); }
static inline u32 get_unaligned_le24(const void *p)
{
	const u8 *b = p;
	return b[0] | b[1] << 8 | b[2] << 16;
}
static inline u32 get_unaligned_be24(const void *p)
{
	const u8 *b = p;
	return b[2] | b[1] << 8 | b[0] << 16;
}
static inline void put_unaligned_le24(u32 val, void *p)
{
	u8 *b = p;
	b[0] = val; b[1] = val >> 8; b[2] = val >> 16;
}

#endif
