/* SPDX-License-Identifier: GPL-2.0-only */
/* cgroup dmem (contabilidad de VRAM por cgroup): sin cgroups, no existe. */
#ifndef _LKPI_LINUX_CGROUP_DMEM_H
#define _LKPI_LINUX_CGROUP_DMEM_H

#include <linux/types.h>
#include <linux/err.h>

struct dmem_cgroup_region;
struct dmem_cgroup_pool_state;

static inline struct dmem_cgroup_region *dmem_cgroup_register_region(u64 size, const char *name_fmt, ...)
{
	return NULL;
}

static inline void dmem_cgroup_unregister_region(struct dmem_cgroup_region *region) { }

#endif
