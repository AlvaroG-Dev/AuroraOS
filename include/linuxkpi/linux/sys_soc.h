/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_SYS_SOC_H
#define _LKPI_LINUX_SYS_SOC_H
#include <linux/types.h>
struct soc_device_attribute { const char *machine, *family, *revision, *soc_id; const void *data; };
static inline const struct soc_device_attribute *soc_device_match(const struct soc_device_attribute *m) { return NULL; }
#endif
