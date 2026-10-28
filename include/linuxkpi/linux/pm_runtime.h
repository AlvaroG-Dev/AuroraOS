/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Runtime PM: los dispositivos estan siempre encendidos (sin gestion de
 * energia en Aurora todavia, L9). Las funciones cuentan el uso y devuelven
 * exito, como un kernel sin CONFIG_PM.
 */
#ifndef _LKPI_LINUX_PM_RUNTIME_H
#define _LKPI_LINUX_PM_RUNTIME_H

#include <linux/device.h>

static inline void pm_runtime_enable(struct device *dev) { dev->power.disable_depth = 0; }
static inline void pm_runtime_disable(struct device *dev) { dev->power.disable_depth = 1; }
static inline int pm_runtime_get_sync(struct device *dev) { atomic_inc(&dev->power.usage_count); return 1; }
static inline int pm_runtime_resume_and_get(struct device *dev) { atomic_inc(&dev->power.usage_count); return 0; }
static inline int pm_runtime_get(struct device *dev) { atomic_inc(&dev->power.usage_count); return 1; }
static inline void pm_runtime_get_noresume(struct device *dev) { atomic_inc(&dev->power.usage_count); }
static inline int pm_runtime_get_if_in_use(struct device *dev) { return -EINVAL; }
static inline int pm_runtime_get_if_active(struct device *dev) { return -EINVAL; }
static inline int pm_runtime_put(struct device *dev) { atomic_dec(&dev->power.usage_count); return 0; }
static inline int pm_runtime_put_sync(struct device *dev) { atomic_dec(&dev->power.usage_count); return 0; }
static inline int pm_runtime_put_autosuspend(struct device *dev) { atomic_dec(&dev->power.usage_count); return 0; }
static inline void pm_runtime_put_noidle(struct device *dev) { atomic_dec(&dev->power.usage_count); }
static inline int pm_runtime_put_sync_autosuspend(struct device *dev) { atomic_dec(&dev->power.usage_count); return 0; }
static inline void pm_runtime_mark_last_busy(struct device *dev) { }
static inline void pm_runtime_use_autosuspend(struct device *dev) { }
static inline void pm_runtime_dont_use_autosuspend(struct device *dev) { }
static inline void pm_runtime_set_autosuspend_delay(struct device *dev, int delay) { }
static inline int pm_runtime_set_active(struct device *dev) { return 0; }
static inline void pm_runtime_set_suspended(struct device *dev) { }
static inline void pm_runtime_allow(struct device *dev) { }
static inline void pm_runtime_forbid(struct device *dev) { }
static inline void pm_runtime_no_callbacks(struct device *dev) { }
static inline void pm_runtime_irq_safe(struct device *dev) { }
static inline bool pm_runtime_suspended(struct device *dev) { return false; }
static inline bool pm_runtime_active(struct device *dev) { return true; }
static inline bool pm_runtime_enabled(struct device *dev) { return !dev->power.disable_depth; }
static inline int pm_runtime_idle(struct device *dev) { return -ENOSYS; }
static inline int pm_runtime_resume(struct device *dev) { return 0; }
static inline int devm_pm_runtime_enable(struct device *dev) { pm_runtime_enable(dev); return 0; }

#endif
