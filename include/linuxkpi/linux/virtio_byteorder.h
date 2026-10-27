/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Conversion de los tipos __virtio16/32/64: little endian si el dispositivo
 * negocio VIRTIO_F_VERSION_1; en el modo legacy, el orden nativo (x86-64:
 * little endian).
 */
#ifndef _LKPI_LINUX_VIRTIO_BYTEORDER_H
#define _LKPI_LINUX_VIRTIO_BYTEORDER_H

#include <linux/types.h>
#include <asm/byteorder.h>
#include <uapi/linux/virtio_types.h>

static inline bool virtio_legacy_is_little_endian(void)
{
	return true;	/* x86-64 */
}

static inline u16 __virtio16_to_cpu(bool le, __virtio16 v)
{
	return le ? le16_to_cpu((__force __le16)v) : be16_to_cpu((__force __be16)v);
}

static inline __virtio16 __cpu_to_virtio16(bool le, u16 v)
{
	return (__force __virtio16)(le ? cpu_to_le16(v) : cpu_to_be16(v));
}

static inline u32 __virtio32_to_cpu(bool le, __virtio32 v)
{
	return le ? le32_to_cpu((__force __le32)v) : be32_to_cpu((__force __be32)v);
}

static inline __virtio32 __cpu_to_virtio32(bool le, u32 v)
{
	return (__force __virtio32)(le ? cpu_to_le32(v) : cpu_to_be32(v));
}

static inline u64 __virtio64_to_cpu(bool le, __virtio64 v)
{
	return le ? le64_to_cpu((__force __le64)v) : be64_to_cpu((__force __be64)v);
}

static inline __virtio64 __cpu_to_virtio64(bool le, u64 v)
{
	return (__force __virtio64)(le ? cpu_to_le64(v) : cpu_to_be64(v));
}

#endif
