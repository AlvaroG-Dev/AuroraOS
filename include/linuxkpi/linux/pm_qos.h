/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_PM_QOS_H
#define _LKPI_LINUX_PM_QOS_H
/* Latencia de CPU: Aurora no tiene estados C que limitar; las peticiones se anotan y no hacen nada. */
#include <linux/types.h>
#define PM_QOS_CPU_LATENCY_DEFAULT_VALUE	2000000000
#define PM_QOS_DEFAULT_VALUE			(-1)
struct pm_qos_request { s32 value; bool active; };
static inline void cpu_latency_qos_add_request(struct pm_qos_request *req, s32 value)
{
	req->value = value;
	req->active = true;
}
static inline void cpu_latency_qos_update_request(struct pm_qos_request *req, s32 value) { req->value = value; }
static inline void cpu_latency_qos_remove_request(struct pm_qos_request *req) { req->active = false; }
static inline bool cpu_latency_qos_request_active(struct pm_qos_request *req) { return req->active; }
static inline s32 cpu_latency_qos_limit(void) { return PM_QOS_CPU_LATENCY_DEFAULT_VALUE; }
#endif
