/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Gestion de energia: solo los tipos. Aurora no suspende dispositivos
 * todavia (L9), asi que las retrollamadas de dev_pm_ops no se invocan.
 */
#ifndef _LKPI_LINUX_PM_H
#define _LKPI_LINUX_PM_H

#include <linux/types.h>

struct device;

typedef struct pm_message {
	int event;
} pm_message_t;

#define PM_EVENT_ON 0x0000
#define PM_EVENT_FREEZE 0x0001
#define PM_EVENT_SUSPEND 0x0002
#define PM_EVENT_HIBERNATE 0x0004
#define PM_EVENT_QUIESCE 0x0008
#define PM_EVENT_RESUME 0x0010
#define PMSG_ON ((struct pm_message){ .event = PM_EVENT_ON })
#define PMSG_FREEZE ((struct pm_message){ .event = PM_EVENT_FREEZE })
#define PMSG_SUSPEND ((struct pm_message){ .event = PM_EVENT_SUSPEND })
#define PMSG_RESUME ((struct pm_message){ .event = PM_EVENT_RESUME })

struct dev_pm_ops {
	int (*prepare)(struct device *dev);
	void (*complete)(struct device *dev);
	int (*suspend)(struct device *dev);
	int (*resume)(struct device *dev);
	int (*freeze)(struct device *dev);
	int (*thaw)(struct device *dev);
	int (*poweroff)(struct device *dev);
	int (*restore)(struct device *dev);
	int (*suspend_late)(struct device *dev);
	int (*resume_early)(struct device *dev);
	int (*freeze_late)(struct device *dev);
	int (*thaw_early)(struct device *dev);
	int (*poweroff_late)(struct device *dev);
	int (*restore_early)(struct device *dev);
	int (*suspend_noirq)(struct device *dev);
	int (*resume_noirq)(struct device *dev);
	int (*freeze_noirq)(struct device *dev);
	int (*thaw_noirq)(struct device *dev);
	int (*poweroff_noirq)(struct device *dev);
	int (*restore_noirq)(struct device *dev);
	int (*runtime_suspend)(struct device *dev);
	int (*runtime_resume)(struct device *dev);
	int (*runtime_idle)(struct device *dev);
};

struct dev_pm_info {
	pm_message_t power_state;
	bool can_wakeup:1;
	bool async_suspend:1;
	bool is_prepared:1;
	bool is_suspended:1;
	bool should_wakeup:1;
	atomic_t usage_count;		/* runtime PM: solo se cuenta */
	int disable_depth;
	spinlock_t lock;
	bool runtime_auto;
	int runtime_status;
};
#define RPM_ACTIVE 0
#define RPM_SUSPENDED 2

struct dev_pm_domain {
	struct dev_pm_ops ops;
	int (*start)(struct device *dev);
	void (*detach)(struct device *dev, bool power_off);
	int (*activate)(struct device *dev);
	void (*sync)(struct device *dev);
	void (*dismiss)(struct device *dev);
	int (*set_performance_state)(struct device *dev, unsigned int state);
};

#define SET_SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn) \
	.suspend = suspend_fn, .resume = resume_fn, .freeze = suspend_fn, \
	.thaw = resume_fn, .poweroff = suspend_fn, .restore = resume_fn,
#define SET_LATE_SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn) \
	.suspend_late = suspend_fn, .resume_early = resume_fn, \
	.freeze_late = suspend_fn, .thaw_early = resume_fn, \
	.poweroff_late = suspend_fn, .restore_early = resume_fn,
#define SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn) \
	.suspend_noirq = suspend_fn, .resume_noirq = resume_fn, \
	.freeze_noirq = suspend_fn, .thaw_noirq = resume_fn, \
	.poweroff_noirq = suspend_fn, .restore_noirq = resume_fn,
#define SET_RUNTIME_PM_OPS(suspend_fn, resume_fn, idle_fn) \
	.runtime_suspend = suspend_fn, .runtime_resume = resume_fn, .runtime_idle = idle_fn,
#define SYSTEM_SLEEP_PM_OPS(s, r) SET_SYSTEM_SLEEP_PM_OPS(s, r)
#define LATE_SYSTEM_SLEEP_PM_OPS(s, r) SET_LATE_SYSTEM_SLEEP_PM_OPS(s, r)
#define NOIRQ_SYSTEM_SLEEP_PM_OPS(s, r) SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(s, r)
#define RUNTIME_PM_OPS(s, r, i) SET_RUNTIME_PM_OPS(s, r, i)
#define DEFINE_SIMPLE_DEV_PM_OPS(name, suspend_fn, resume_fn) \
	const struct dev_pm_ops name = { SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn) }
#define SIMPLE_DEV_PM_OPS(name, suspend_fn, resume_fn) \
	const struct dev_pm_ops __maybe_unused name = { SET_SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn) }
#define DEFINE_RUNTIME_DEV_PM_OPS(name, suspend_fn, resume_fn, idle_fn) \
	const struct dev_pm_ops name = { SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn) \
					 RUNTIME_PM_OPS(suspend_fn, resume_fn, idle_fn) }
#define pm_sleep_ptr(p) (p)
#define pm_ptr(p) (p)

static inline void device_set_wakeup_capable(struct device *dev, bool capable) { }
static inline int device_init_wakeup(struct device *dev, bool enable) { return 0; }
static inline int device_set_wakeup_enable(struct device *dev, bool enable) { return 0; }
static inline bool device_may_wakeup(struct device *dev) { return false; }


typedef int suspend_state_t;
#define PM_SUSPEND_ON		0
#define PM_SUSPEND_TO_IDLE	1
#define PM_SUSPEND_STANDBY	2
#define PM_SUSPEND_MEM		3
#define PM_SUSPEND_MAX		4
#define pm_suspend_target_state	PM_SUSPEND_ON
#define DPM_FLAG_NO_DIRECT_COMPLETE	BIT(0)
#define DPM_FLAG_SMART_PREPARE		BIT(1)
#define DPM_FLAG_SMART_SUSPEND		BIT(2)
#define DPM_FLAG_MAY_SKIP_RESUME	BIT(3)
struct device;
static inline void dev_pm_set_driver_flags(struct device *dev, u32 flags) {}
extern const char lkpi_power_group_name[];
#define power_group_name lkpi_power_group_name
struct device;
static inline void pm_vt_switch_required(struct device *dev, bool required) {}
static inline void pm_vt_switch_unregister(struct device *dev) {}
#endif
