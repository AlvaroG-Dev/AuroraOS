/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_CRC32_H
#define _LKPI_LINUX_CRC32_H

#include <linux/types.h>

/* CRC-32 IEEE 802.3 (polinomio reflejado 0xEDB88320) y CRC-32C (Castagnoli). */
u32 lkpi_crc32_le(u32 crc, const void *p, size_t len);
u32 lkpi_crc32_be(u32 crc, const void *p, size_t len);
u32 lkpi_crc32c(u32 crc, const void *p, size_t len);
#define crc32_le(crc, p, len) lkpi_crc32_le(crc, p, len)
#define crc32_be(crc, p, len) lkpi_crc32_be(crc, p, len)
#define crc32(seed, data, length) crc32_le(seed, (const void *)(data), length)
#define crc32c(crc, p, len) lkpi_crc32c(crc, p, len)
#define ether_crc_le(length, data) crc32_le(~0, data, length)

#endif
