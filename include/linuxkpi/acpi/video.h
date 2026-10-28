/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_ACPI_VIDEO_H
#define _LKPI_ACPI_VIDEO_H
/* Sin ACPI video: i915 usa su backlight nativo (acpi_video_get_backlight_type() = native). */
#include <linux/types.h>
#define ACPI_VIDEO_CLASS "video"
enum acpi_backlight_type { acpi_backlight_undef = -1, acpi_backlight_none = 0, acpi_backlight_video,
			   acpi_backlight_vendor, acpi_backlight_native, acpi_backlight_nvidia_wmi_ec,
			   acpi_backlight_apple_gmux };
static inline int acpi_video_register(void) { return 0; }
static inline void acpi_video_unregister(void) {}
static inline void acpi_video_register_backlight(void) {}
static inline enum acpi_backlight_type acpi_video_get_backlight_type(void) { return acpi_backlight_native; }
static inline bool acpi_video_backlight_use_native(void) { return true; }
static inline void acpi_video_report_nolcd(void) {}
#endif
