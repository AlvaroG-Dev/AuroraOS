// SPDX-License-Identifier: GPL-2.0-only
/* lkpi_drm.kur: punto de entrada. */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <drm/drm_atomic.h>
#include <drm/drm_drv.h>
#include <linux/device.h>
#include <drm/drm_crtc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_plane.h>
#include <linux/iosys-map.h>
#include "hooks.h"

/*
 * Pantalla (decision 1A de L5): el compositor es dueño del GOP, asi que el
 * driver de Linux no programa la pantalla real. Tras cada commit, el plano
 * primario de cada CRTC se copia a una ventana del compositor (una por CRTC);
 * kernel/drm/drm_linux_scanout.c.
 */
int lkpi_scan_open(int idx, int w, int h) __asm__("drm_lx_scanout_open");
void lkpi_scan_close(int idx) __asm__("drm_lx_scanout_close");
int lkpi_scan_present(int idx, const void *src, u32 pitch, u32 fourcc, int w, int h)
	__asm__("drm_lx_scanout_present");

int lkpi_cmdline_get(const char *key, char *out, size_t outlen) __asm__("cmdline_get");

#define LKPI_FRAME_MS 16	/* 60 Hz por software, como el vblank del backend nativo */

/* El commit no empieza antes de 16 ms despues del anterior: sin vblank real
 * (bochs, simpledrm, virtio-gpu 2D) el compositor repintaria sin freno. */
/*
 * Los drivers KMS sencillos de Linux (bochs, simpledrm) no declaran nodo
 * render ni syncobj; el backend nativo de Aurora los ofrecia siempre
 * (renderD128 para PRIME, DRM_CAP_SYNCOBJ para Mesa y los compositores). Se
 * anaden sobre una copia de la tabla del driver, salvo `lkpi.drm_quirks=0`.
 */
const struct drm_driver *lkpi_drm_driver_fixup(struct device *parent, const struct drm_driver *drv)
{
	struct drm_driver *copy;
	char opt[8];

	if (!(drv->driver_features & DRIVER_GEM) || !(drv->driver_features & DRIVER_MODESET))
		return drv;
	if (lkpi_cmdline_get("lkpi.drm_quirks", opt, sizeof(opt)) && opt[0] == '0')
		return drv;
	copy = devm_kmemdup(parent, drv, sizeof(*drv), GFP_KERNEL);
	if (!copy)
		return drv;
	copy->driver_features |= DRIVER_RENDER | DRIVER_SYNCOBJ | DRIVER_SYNCOBJ_TIMELINE;
	return copy;
}

void lkpi_drm_pace(struct drm_atomic_state *state)
{
	static ktime_t last;
	ktime_t now = ktime_get();
	s64 ms;

	if (state->legacy_cursor_update)
		return;
	ms = ktime_to_ms(ktime_sub(now, last));
	if (last && ms >= 0 && ms < LKPI_FRAME_MS)
		msleep(LKPI_FRAME_MS - ms);
	last = ktime_get();
}

void lkpi_drm_present(struct drm_atomic_state *state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *cs;
	int i;

	for_each_new_crtc_in_state(state, crtc, cs, i) {
		struct drm_framebuffer *fb = crtc->primary ? crtc->primary->state->fb : NULL;
		struct iosys_map map[DRM_FORMAT_MAX_PLANES];
		struct iosys_map data[DRM_FORMAT_MAX_PLANES];
		int idx = crtc->index, w = cs->mode.hdisplay, h = cs->mode.vdisplay;

		if (!cs->active || !fb || w <= 0 || h <= 0) {
			lkpi_scan_close(idx);
			continue;
		}
		if (lkpi_scan_open(idx, w, h))
			continue;
		if (drm_gem_fb_vmap(fb, map, data))
			continue;
		if (!iosys_map_is_null(&map[0]) && !map[0].is_iomem)
			lkpi_scan_present(idx, map[0].vaddr + fb->offsets[0], fb->pitches[0],
					  fb->format->format, min_t(int, w, fb->width),
					  min_t(int, h, fb->height));
		drm_gem_fb_vunmap(fb, map);
	}
}

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
