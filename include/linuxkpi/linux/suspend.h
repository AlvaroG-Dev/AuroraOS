/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_SUSPEND_H
#define _LKPI_LINUX_SUSPEND_H
#include <linux/notifier.h>
#define PM_HIBERNATION_PREPARE	0x0001
#define PM_POST_HIBERNATION	0x0002
#define PM_SUSPEND_PREPARE	0x0003
#define PM_POST_SUSPEND		0x0004
#define PM_RESTORE_PREPARE	0x0005
#define PM_POST_RESTORE		0x0006
static inline int register_pm_notifier(struct notifier_block *nb) { return 0; }
static inline int unregister_pm_notifier(struct notifier_block *nb) { return 0; }
#endif
