/* SPDX-License-Identifier: GPL-2.0-only */
/* Stub minimo de LinuxKPI (N2: lo justo para compilar kernel-open/ de NVIDIA). */
#ifndef _LKPI_LINUX_MEMORY_H
#define _LKPI_LINUX_MEMORY_H
#include <linux/types.h>
struct notifier_block;
struct memory_notify { unsigned long start_pfn, nr_pages; int status_change_nid_normal, status_change_nid; };
#define MEM_GOING_ONLINE 1
#define MEM_ONLINE 2
#define MEM_GOING_OFFLINE 3
#endif
