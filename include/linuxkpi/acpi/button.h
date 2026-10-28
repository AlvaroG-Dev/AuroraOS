/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_ACPI_BUTTON_H
#define _LKPI_ACPI_BUTTON_H
#include <linux/notifier.h>
/* Sin boton ACPI de tapa: intel_lvds asume tapa abierta. */
static inline int acpi_lid_notifier_register(struct notifier_block *nb) { return 0; }
static inline int acpi_lid_notifier_unregister(struct notifier_block *nb) { return 0; }
static inline int acpi_lid_open(void) { return 1; }
#endif
