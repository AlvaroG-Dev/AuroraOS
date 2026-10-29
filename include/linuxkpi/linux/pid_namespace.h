/* SPDX-License-Identifier: GPL-2.0-only */
/* Stub minimo de LinuxKPI (N2: lo justo para compilar kernel-open/ de NVIDIA). */
#ifndef _LKPI_LINUX_PID_NAMESPACE_H
#define _LKPI_LINUX_PID_NAMESPACE_H
#include <linux/types.h>
struct task_struct;
struct pid_namespace { int level; };
extern struct pid_namespace init_pid_ns;
static inline struct pid_namespace *task_active_pid_ns(struct task_struct *t) { return &init_pid_ns; }
#endif
