/* SPDX-License-Identifier: GPL-2.0-only */
/* x86-64 es little-endian: cpu_to_le* es la identidad y *_be* intercambia. */
#ifndef _LKPI_LINUX_BYTEORDER_GENERIC_H
#define _LKPI_LINUX_BYTEORDER_GENERIC_H

#include <linux/swab.h>

#define __LITTLE_ENDIAN 1234
#define __LITTLE_ENDIAN_BITFIELD
#ifndef __ORDER_LITTLE_ENDIAN__
#define __ORDER_LITTLE_ENDIAN__ 1234
#endif

#define cpu_to_le64(x) ((__le64)(u64)(x))
#define le64_to_cpu(x) ((u64)(__le64)(x))
#define cpu_to_le32(x) ((__le32)(u32)(x))
#define le32_to_cpu(x) ((u32)(__le32)(x))
#define cpu_to_le16(x) ((__le16)(u16)(x))
#define le16_to_cpu(x) ((u16)(__le16)(x))
#define cpu_to_be64(x) ((__be64)swab64(x))
#define be64_to_cpu(x) swab64((u64)(__be64)(x))
#define cpu_to_be32(x) ((__be32)swab32(x))
#define be32_to_cpu(x) swab32((u32)(__be32)(x))
#define cpu_to_be16(x) ((__be16)swab16(x))
#define be16_to_cpu(x) swab16((u16)(__be16)(x))
#define __cpu_to_le64 cpu_to_le64
#define __le64_to_cpu le64_to_cpu
#define __cpu_to_le32 cpu_to_le32
#define __le32_to_cpu le32_to_cpu
#define __cpu_to_le16 cpu_to_le16
#define __le16_to_cpu le16_to_cpu
#define __cpu_to_be64 cpu_to_be64
#define __be64_to_cpu be64_to_cpu
#define __cpu_to_be32 cpu_to_be32
#define __be32_to_cpu be32_to_cpu
#define __cpu_to_be16 cpu_to_be16
#define __be16_to_cpu be16_to_cpu

#define cpu_to_le64p(p) (*(p))
#define le64_to_cpup(p) (*(const u64 *)(p))
#define cpu_to_le32p(p) (*(p))
#define le32_to_cpup(p) (*(const u32 *)(p))
#define cpu_to_le16p(p) (*(p))
#define le16_to_cpup(p) (*(const u16 *)(p))
#define be32_to_cpup(p) be32_to_cpu(*(p))
#define be16_to_cpup(p) be16_to_cpu(*(p))
#define le32_to_cpus(p) do { (void)(p); } while (0)
#define le16_to_cpus(p) do { (void)(p); } while (0)
#define cpu_to_le32s(p) do { (void)(p); } while (0)
#define cpu_to_le16s(p) do { (void)(p); } while (0)
#define be32_to_cpus(p) swab32s((u32 *)(p))
#define cpu_to_be32s(p) swab32s((u32 *)(p))

#define htonl(x) cpu_to_be32(x)
#define ntohl(x) be32_to_cpu(x)
#define htons(x) cpu_to_be16(x)
#define ntohs(x) be16_to_cpu(x)

static inline void le32_add_cpu(__le32 *var, u32 val) { *var = cpu_to_le32(le32_to_cpu(*var) + val); }
static inline void le16_add_cpu(__le16 *var, u16 val) { *var = cpu_to_le16(le16_to_cpu(*var) + val); }
static inline void le64_add_cpu(__le64 *var, u64 val) { *var = cpu_to_le64(le64_to_cpu(*var) + val); }
static inline void cpu_to_be32_array(__be32 *dst, const u32 *src, size_t len)
{
	for (size_t i = 0; i < len; i++) dst[i] = cpu_to_be32(src[i]);
}
static inline void be32_to_cpu_array(u32 *dst, const __be32 *src, size_t len)
{
	for (size_t i = 0; i < len; i++) dst[i] = be32_to_cpu(src[i]);
}

#endif
