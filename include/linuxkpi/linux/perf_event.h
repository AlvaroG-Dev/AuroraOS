/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_PERF_EVENT_H
#define _LKPI_LINUX_PERF_EVENT_H
/* CONFIG_PERF_EVENTS=n: solo los tipos que las cabeceras de i915 nombran (struct pmu embebido). */
#include <linux/types.h>
#include <linux/list.h>
#include <linux/cpumask.h>
struct perf_event;
struct pmu { int type; const char *name; };
static inline int perf_allow_cpu(void) { return 0; }
static inline int perf_allow_kernel(void) { return 0; }
#endif
