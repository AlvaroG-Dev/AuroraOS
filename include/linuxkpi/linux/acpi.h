/* SPDX-License-Identifier: GPL-2.0-only */
/* ACPI: la capa no lo expone a los drivers (sin CONFIG_ACPI). */
#ifndef _LKPI_LINUX_ACPI_H
#define _LKPI_LINUX_ACPI_H

#include <linux/types.h>

struct acpi_device;

static inline bool has_acpi_companion(struct device *dev) { return false; }

#endif
