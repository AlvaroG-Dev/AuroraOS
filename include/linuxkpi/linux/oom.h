/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_OOM_H
#define _LKPI_LINUX_OOM_H
#include <linux/notifier.h>
/* El OOM-killer de Aurora no avisa a nadie: se anota el notifier y no se llama. */
static inline int register_oom_notifier(struct notifier_block *nb) { return 0; }
static inline int unregister_oom_notifier(struct notifier_block *nb) { return 0; }
#endif
