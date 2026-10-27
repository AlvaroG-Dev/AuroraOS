/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Capacidades (K8, L4). Aurora no tiene capacidades por proceso: root
 * (euid 0) las tiene todas y nadie mas tiene ninguna. Hilos del kernel: todas.
 */
#ifndef _LKPI_LINUX_CAPABILITY_H
#define _LKPI_LINUX_CAPABILITY_H

#include <linux/types.h>

#define CAP_CHOWN 0
#define CAP_DAC_OVERRIDE 1
#define CAP_KILL 5
#define CAP_NET_ADMIN 12
#define CAP_IPC_LOCK 14
#define CAP_SYS_RAWIO 17
#define CAP_SYS_PTRACE 19
#define CAP_SYS_ADMIN 21
#define CAP_SYS_NICE 23
#define CAP_SYS_RESOURCE 24
#define CAP_PERFMON 38
#define CAP_LAST_CAP 40

bool lkpi_capable(int cap);
#define capable(cap) lkpi_capable(cap)
#define ns_capable(ns, cap) lkpi_capable(cap)
#define perfmon_capable() lkpi_capable(CAP_PERFMON)
#define has_capability(p, cap) lkpi_capable(cap)

#endif
