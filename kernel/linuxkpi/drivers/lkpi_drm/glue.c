// SPDX-License-Identifier: GPL-2.0-only
/* lkpi_drm.kur: punto de entrada. */
#include <linux/module.h>
#include <linux/init.h>

/* Sin tabla DMI: ningun panel con orientacion forzada. */
int drm_get_panel_orientation_quirk(int width, int height)
{
	return -1;	/* DRM_MODE_PANEL_ORIENTATION_UNKNOWN */
}

int lkpi_drm_core_init(void);
void lkpi_drm_core_exit(void);

static int __init lkpi_drm_init(void)
{
	return lkpi_drm_core_init();
}

static void __exit lkpi_drm_exit(void)
{
	lkpi_drm_core_exit();
}

module_init(lkpi_drm_init);
module_exit(lkpi_drm_exit);
MODULE_DESCRIPTION("Nucleo DRM/KMS de Linux sobre LinuxKPI");
MODULE_LICENSE("GPL");
