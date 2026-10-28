/* SPDX-License-Identifier: GPL-2.0-only */
/* Parametro "nomodeset": en Aurora los drivers DRM siempre pueden hacer modeset. */
#ifndef _LKPI_VIDEO_NOMODESET_H
#define _LKPI_VIDEO_NOMODESET_H

#include <linux/types.h>

static inline bool video_firmware_drivers_only(void)
{
	return false;
}

#endif
