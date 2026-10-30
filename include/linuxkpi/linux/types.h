/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_TYPES_H
#define _LKPI_LINUX_TYPES_H

#include <linux/stddef.h>

typedef __signed__ char __s8;
typedef unsigned char __u8;
typedef __signed__ short __s16;
typedef unsigned short __u16;
typedef __signed__ int __s32;
typedef unsigned int __u32;
typedef __signed__ long long __s64;
typedef unsigned long long __u64;

typedef __s8 s8;
typedef __u8 u8;
typedef __s16 s16;
typedef __u16 u16;
typedef __s32 s32;
typedef __u32 u32;
typedef __s64 s64;
typedef __u64 u64;

typedef __u16 __le16;
typedef __u16 __be16;
typedef __u32 __le32;
typedef __u32 __be32;
typedef __u64 __le64;
typedef __u64 __be64;
typedef __u16 __sum16;
typedef __u32 __wsum;

typedef _Bool bool;

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef long ssize_t;
typedef long __kernel_long_t;
typedef unsigned long __kernel_ulong_t;
typedef unsigned long uintptr_t;
typedef long intptr_t;

typedef __s8 int8_t;
typedef __u8 uint8_t;
typedef __s16 int16_t;
typedef __u16 uint16_t;
typedef __s32 int32_t;
typedef __u32 uint32_t;
typedef __s64 int64_t;
typedef __u64 uint64_t;
typedef __u8 u_int8_t;
typedef __u16 u_int16_t;
typedef __u32 u_int32_t;
typedef __u64 u_int64_t;

typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef unsigned char unchar;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned long ulong;

typedef u64 phys_addr_t;
typedef phys_addr_t resource_size_t;
typedef u64 dma_addr_t;
typedef unsigned long irq_hw_number_t;

typedef long long loff_t;
typedef long off_t;
typedef int pid_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int __kernel_dev_t;
typedef __kernel_dev_t dev_t;
typedef unsigned short umode_t;
typedef unsigned int fmode_t;
typedef unsigned int gfp_t;
typedef unsigned int slab_flags_t;
typedef long long time64_t;
typedef s64 ktime_t;
typedef long __kernel_time_t;
typedef long clock_t;
typedef unsigned long blkcnt_t;
typedef unsigned long sector_t;
typedef unsigned long pgoff_t;
typedef unsigned short __kernel_sa_family_t;
typedef unsigned long __kernel_size_t;
typedef long __kernel_ssize_t;
typedef long __kernel_ptrdiff_t;

typedef struct {
	int counter;
} atomic_t;

typedef struct {
	s64 counter;
} atomic64_t;

typedef atomic64_t atomic_long_t;

struct list_head {
	struct list_head *next, *prev;
};

struct hlist_head {
	struct hlist_node *first;
};

struct hlist_node {
	struct hlist_node *next, **pprev;
};

struct callback_head {
	struct callback_head *next;
	void (*func)(struct callback_head *head);
} __attribute__((aligned(sizeof(void *))));
#define rcu_head callback_head

typedef unsigned int __poll_t;
typedef int clockid_t;

#define DECLARE_BITMAP(name, bits) \
	unsigned long name[((bits) + 64 - 1) / 64]

#define __aligned_u64 __u64 __attribute__((aligned(8)))
#define __aligned_be64 __be64 __attribute__((aligned(8)))
#define __aligned_le64 __le64 __attribute__((aligned(8)))

#endif
