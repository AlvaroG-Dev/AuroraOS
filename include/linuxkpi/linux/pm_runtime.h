/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Runtime PM (L9.2). La implementacion esta en kernel/linuxkpi/base/pm.c.
 *
 * Politica: con `lkpi.runtime_pm=on` en la linea del kernel el motor es real
 * (callbacks runtime_suspend/resume/idle, autosuspend, D3hot de PCI). Sin
 * eso -el valor por omision, hasta probarlo en equipo real- las funciones
 * solo cuentan usos y el dispositivo esta siempre activo, como un kernel sin
 * gestion de energia (justo lo que i915 necesita para no ver los dominios
 * de energia apagados). Ver Plan_LinuxKPI.md, L9.2.
 */
#ifndef _LKPI_LINUX_PM_RUNTIME_H
#define _LKPI_LINUX_PM_RUNTIME_H

#include <linux/device.h>

bool lkpi_pm_runtime_policy(void);
void lkpi_pm_runtime_enable(struct device *dev);
void lkpi_pm_runtime_disable(struct device *dev);
int lkpi_pm_runtime_get_sync(struct device *dev);
int lkpi_pm_runtime_resume_and_get(struct device *dev);
int lkpi_pm_runtime_get(struct device *dev);
int lkpi_pm_runtime_get_if_in_use(struct device *dev);
int lkpi_pm_runtime_get_if_active(struct device *dev);
int lkpi_pm_runtime_put(struct device *dev);
int lkpi_pm_runtime_put_sync(struct device *dev);
int lkpi_pm_runtime_put_autosuspend(struct device *dev);
int lkpi_pm_runtime_put_sync_autosuspend(struct device *dev);
void lkpi_pm_runtime_use_autosuspend(struct device *dev, bool use);
void lkpi_pm_runtime_set_autosuspend_delay(struct device *dev, int delay);
int lkpi_pm_runtime_set_active(struct device *dev);
void lkpi_pm_runtime_set_suspended(struct device *dev);
void lkpi_pm_runtime_allow(struct device *dev);
void lkpi_pm_runtime_forbid(struct device *dev);
int lkpi_pm_runtime_idle(struct device *dev);
int lkpi_pm_runtime_suspend(struct device *dev);
int lkpi_pm_runtime_resume(struct device *dev);
int lkpi_pm_request_resume(struct device *dev);
int lkpi_pm_schedule_suspend(struct device *dev, unsigned int delay);
bool lkpi_pm_runtime_suspended(struct device *dev);
bool lkpi_pm_runtime_active(struct device *dev);
void lkpi_pm_runtime_barrier(struct device *dev);

#define pm_runtime_enable(d) lkpi_pm_runtime_enable(d)
#define pm_runtime_disable(d) lkpi_pm_runtime_disable(d)
#define pm_runtime_get_sync(d) lkpi_pm_runtime_get_sync(d)
#define pm_runtime_resume_and_get(d) lkpi_pm_runtime_resume_and_get(d)
#define pm_runtime_get(d) lkpi_pm_runtime_get(d)
#define pm_runtime_get_if_in_use(d) lkpi_pm_runtime_get_if_in_use(d)
#define pm_runtime_get_if_active(d) lkpi_pm_runtime_get_if_active(d)
#define pm_runtime_put(d) lkpi_pm_runtime_put(d)
#define pm_runtime_put_sync(d) lkpi_pm_runtime_put_sync(d)
#define pm_runtime_put_autosuspend(d) lkpi_pm_runtime_put_autosuspend(d)
#define pm_runtime_put_sync_autosuspend(d) lkpi_pm_runtime_put_sync_autosuspend(d)
#define pm_runtime_use_autosuspend(d) lkpi_pm_runtime_use_autosuspend(d, true)
#define pm_runtime_dont_use_autosuspend(d) lkpi_pm_runtime_use_autosuspend(d, false)
#define pm_runtime_set_autosuspend_delay(d, n) lkpi_pm_runtime_set_autosuspend_delay(d, n)
#define pm_runtime_set_active(d) lkpi_pm_runtime_set_active(d)
#define pm_runtime_set_suspended(d) lkpi_pm_runtime_set_suspended(d)
#define pm_runtime_allow(d) lkpi_pm_runtime_allow(d)
#define pm_runtime_forbid(d) lkpi_pm_runtime_forbid(d)
#define pm_runtime_idle(d) lkpi_pm_runtime_idle(d)
#define pm_runtime_suspend(d) lkpi_pm_runtime_suspend(d)
#define pm_runtime_resume(d) lkpi_pm_runtime_resume(d)
#define pm_request_resume(d) lkpi_pm_request_resume(d)
#define pm_schedule_suspend(d, n) lkpi_pm_schedule_suspend(d, n)
#define pm_runtime_suspended(d) lkpi_pm_runtime_suspended(d)
#define pm_runtime_active(d) lkpi_pm_runtime_active(d)
#define pm_runtime_status_suspended(d) lkpi_pm_runtime_suspended(d)
#define pm_runtime_barrier(d) lkpi_pm_runtime_barrier(d)

static inline void pm_runtime_get_noresume(struct device *dev) { atomic_inc(&dev->power.usage_count); }
static inline void pm_runtime_put_noidle(struct device *dev) { atomic_dec(&dev->power.usage_count); }
static inline void pm_runtime_mark_last_busy(struct device *dev) { dev->power.last_busy = jiffies; }
static inline void pm_runtime_no_callbacks(struct device *dev) { dev->power.no_callbacks = true; }
static inline void pm_runtime_irq_safe(struct device *dev) { dev->power.irq_safe = true; }
static inline bool pm_runtime_enabled(struct device *dev) { return !dev->power.disable_depth; }
static inline int devm_pm_runtime_enable(struct device *dev) { pm_runtime_enable(dev); return 0; }
static inline void pm_runtime_set_memalloc_noio(struct device *dev, bool enable) { }

static inline bool pm_suspend_via_firmware(void) { return false; }
static inline int device_wakeup_enable(struct device *dev) { return 0; }
static inline bool device_can_wakeup(struct device *dev) { return false; }
#endif
