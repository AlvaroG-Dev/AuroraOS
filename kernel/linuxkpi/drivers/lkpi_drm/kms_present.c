// SPDX-License-Identifier: GPL-2.0-only
/*
 * lkpi_drm.kur: presentacion del compositor por KMS (L7 hito 2, Plan_LinuxKPI.md §11).
 *
 * Un cliente DRM del kernel (como el de fbdev, pero sin fbdev) hace el modeset
 * propio del panel con el modo del tamaño del compositor y reserva dos
 * buffers dumb (GEM del driver: shmem en bochs/virtio, objeto GEM de i915).
 * El compositor pinta en el trasero y llama a flip(): un commit atomico
 * bloqueante, que en i915 espera al vblank real (page-flip), y se intercambian.
 * Si algo falla antes del primer commit, el compositor sigue en el GOP.
 *
 * Politica (`lkpi.kms=`): auto (por defecto) para i915 y nvidia-drm; 1 para cualquier
 * driver (QEMU: bochs, virtio-gpu); 0 desactiva. `lkpi.kms_flush=1|0` fuerza o
 * quita el clflush de lo escrito (por defecto solo i915: el mapeo es WB y el
 * barrido de pantalla de Haswell no es coherente con la cache de la CPU).
 * Con `drm.debug=0x14` se ve cada paso del modeset.
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <drm/drm_cache.h>
#include <drm/drm_client.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modes.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>
#include <linux/iosys-map.h>
#include "kms_present.h"
#include "hooks.h"

int lkpi_cmdline_get(const char *key, char *out, size_t outlen) __asm__("cmdline_get");
void lkpi_kms_screen_size(int *w, int *h) __asm__("drm_lx_kms_screen_size");
void lkpi_scan_close(int idx) __asm__("drm_lx_scanout_close");
int lkpi_kms_register(const struct drm_lx_kms_ops *ops) __asm__("drm_lx_kms_register");
void lkpi_kms_unregister(const struct drm_lx_kms_ops *ops) __asm__("drm_lx_kms_unregister");

#define KMS_FRAME_US 16667
#define KMS_FIRST_FAILS 3	/* commits fallidos (no EBUSY) antes de rendirse */

static struct {
	struct drm_client_dev client;
	struct drm_device *dev;
	struct drm_crtc *crtc;
	struct drm_mode_set *set;
	struct drm_client_buffer *buf[2];
	struct iosys_map map[2];
	struct mutex lock;
	struct task_struct *committer;
	struct drm_lx_kms_ops ops;
	int cur;		/* buffer trasero */
	int w, h;
	bool inuse;		/* hay cliente (registrado o no) */
	bool active;		/* ops registradas */
	bool started;		/* primer commit hecho */
	bool flush;
	bool hw_vblank;
	int bad;
	u64 last_vbl;
	ktime_t last_flip;
	unsigned long frames;
} K;

bool lkpi_kms_committing(void)
{
	return K.committer == current;
}

static u32 *kms_back(u32 *pitch_px)
{
	if (!K.active)
		return NULL;
	*pitch_px = K.buf[K.cur]->fb->pitches[0] / 4;
	return K.map[K.cur].is_iomem ? (u32 __force *)K.map[K.cur].vaddr_iomem : K.map[K.cur].vaddr;
}

static int kms_flip(const int dirty[4], u32 *commit_us, u32 *vbl_gap)
{
	struct drm_client_buffer *b = K.buf[K.cur];
	ktime_t t0, t1;
	u64 vbl = 0;
	int ret, y;

	*commit_us = 0;
	*vbl_gap = 0;
	if (!K.active)
		return -ENODEV;

	if (K.flush && dirty[2] > 0 && dirty[3] > 0) {
		u8 *base = (u8 *)kms_back(&(u32){0});

		for (y = dirty[1]; y < dirty[1] + dirty[3]; y++)
			drm_clflush_virt_range(base + (size_t)y * b->fb->pitches[0] + dirty[0] * 4, dirty[2] * 4);
	}

	mutex_lock(&K.lock);
	K.set->fb = b->fb;
	K.set->x = K.set->y = 0;
	if (K.hw_vblank)
		drm_crtc_vblank_get(K.crtc);
	K.committer = current;
	t0 = ktime_get();
	ret = drm_client_modeset_commit(&K.client);
	t1 = ktime_get();
	K.committer = NULL;
	if (K.hw_vblank) {
		vbl = drm_crtc_vblank_count(K.crtc);
		drm_crtc_vblank_put(K.crtc);
	}
	mutex_unlock(&K.lock);

	if (ret) {
		if (ret == -EBUSY)	/* otro maestro DRM (userland): se reintenta */
			return ret;
		if (!K.started && ++K.bad >= KMS_FIRST_FAILS) {
			drm_err(K.dev, "kms: el modeset propio falla (%d); se queda el GOP\n", ret);
			return -ENODEV;
		}
		if (K.frames++ % 120 == 0)
			drm_warn(K.dev, "kms: commit fallido (%d)\n", ret);
		return ret;
	}

	if (ktime_to_ms(ktime_sub(t1, t0)) > 3000 && K.frames++ % 20 == 0)
		drm_err(K.dev, "kms: commit de %lld ms (vblank/flip_done sin IRQ?); `lkpi.kms=0` vuelve al GOP\n",
			ktime_to_ms(ktime_sub(t1, t0)));
	if (!K.started) {
		K.started = true;
		drm_info(K.dev, "kms: modeset propio hecho (%dx%d, %s)\n", K.w, K.h,
			 K.hw_vblank ? "vblank real" : "sin vblank de hardware");
	}
	K.cur ^= 1;
	*commit_us = (u32)ktime_to_us(ktime_sub(t1, t0));
	/* Vblanks desde el flip anterior, solo si los flips son seguidos (con la
	 * pantalla quieta no hay flips y el hueco no es una perdida). */
	if (K.hw_vblank && K.last_vbl && ktime_to_us(ktime_sub(t1, K.last_flip)) < 3 * KMS_FRAME_US)
		*vbl_gap = (u32)(vbl - K.last_vbl);
	K.last_vbl = vbl;
	K.last_flip = t1;
	return 0;
}

static void kms_release(void)
{
	int i;

	if (K.active) {
		lkpi_kms_unregister(&K.ops);
		K.active = false;
	}
	for (i = 0; i < 2; i++) {
		if (!K.buf[i])
			continue;
		drm_client_buffer_vunmap(K.buf[i]);
		drm_client_buffer_delete(K.buf[i]);
		K.buf[i] = NULL;
	}
}

static void kms_client_unregister(struct drm_client_dev *client)
{
	mutex_lock(&K.lock);
	kms_release();
	K.started = false;
	K.inuse = false;
	mutex_unlock(&K.lock);
	drm_client_release(client);
}

/* drm_lastclose(): un cliente de userland solto el dispositivo; se recupera. */
static int kms_client_restore(struct drm_client_dev *client, bool force)
{
	int ret;

	if (!K.started)
		return 0;
	mutex_lock(&K.lock);
	K.committer = current;
	ret = force ? drm_client_modeset_commit_locked(client) : drm_client_modeset_commit(client);
	K.committer = NULL;
	mutex_unlock(&K.lock);
	return ret;
}

static struct drm_display_mode *kms_find_mode(struct drm_connector *conn, int w, int h)
{
	struct drm_display_mode *m, *best = NULL;

	list_for_each_entry(m, &conn->modes, head) {
		if (m->hdisplay != w || m->vdisplay != h)
			continue;
		if (!best || ((m->type & DRM_MODE_TYPE_PREFERRED) && !(best->type & DRM_MODE_TYPE_PREFERRED)) ||
		    (!!(m->type & DRM_MODE_TYPE_PREFERRED) == !!(best->type & DRM_MODE_TYPE_PREFERRED) &&
		     drm_mode_vrefresh(m) > drm_mode_vrefresh(best)))
			best = m;
	}
	return best;
}

static int kms_client_hotplug(struct drm_client_dev *client)
{
	struct drm_device *dev = client->dev;
	struct drm_display_mode *want = NULL, *dup;
	struct drm_mode_set *set;
	struct drm_connector *conn;
	int w, h, ret, i;

	if (K.active)
		return 0;

	lkpi_kms_screen_size(&w, &h);
	if (w <= 0 || h <= 0)
		return 0;

	ret = drm_client_modeset_probe(client, w, h);
	if (ret) {
		drm_dbg_kms(dev, "kms: probe de modos: %d\n", ret);
		return 0;	/* se reintenta en el siguiente hotplug */
	}

	mutex_lock(&client->modeset_mutex);
	drm_client_for_each_modeset(set, client) {
		if (!set->num_connectors || !set->mode)
			continue;
		conn = set->connectors[0];
		want = kms_find_mode(conn, w, h);
		if (want)
			break;
		drm_info(dev, "kms: %s no tiene modo %dx%d; se queda el GOP\n", conn->name, w, h);
	}
	if (!want || !set->crtc) {
		mutex_unlock(&client->modeset_mutex);
		return 0;
	}
	dup = drm_mode_duplicate(dev, want);
	if (!dup) {
		mutex_unlock(&client->modeset_mutex);
		return 0;
	}
	drm_mode_destroy(dev, set->mode);
	set->mode = dup;
	K.set = set;
	K.crtc = set->crtc;
	mutex_unlock(&client->modeset_mutex);

	K.w = w;
	K.h = h;
	for (i = 0; i < 2; i++) {
		struct iosys_map map;

		K.buf[i] = drm_client_framebuffer_create(client, w, h, DRM_FORMAT_XRGB8888);
		if (IS_ERR(K.buf[i])) {
			drm_info(dev, "kms: buffer %d: %ld\n", i, PTR_ERR(K.buf[i]));
			K.buf[i] = NULL;
			goto fail;
		}
		if (drm_client_buffer_vmap(K.buf[i], &map)) {
			drm_info(dev, "kms: sin mapeo CPU del buffer %d\n", i);
			goto fail;
		}
		K.map[i] = map;
	}

	K.hw_vblank = drm_dev_has_vblank(dev) && drm_crtc_index(K.crtc) < dev->num_crtcs &&
		      drm_crtc_vblank_get(K.crtc) == 0;
	if (K.hw_vblank) {
		/* Diagnostico para hardware real: si el contador no avanza, el
		 * pipe esta apagado o la IRQ de vblank no llega (el commit
		 * bloqueante esperaria al timeout de flip_done). */
		u64 c0 = drm_crtc_vblank_count(K.crtc);

		msleep(60);
		drm_info(dev, "kms: vblank %s (contador %llu -> %llu en 60 ms)\n",
			 drm_crtc_vblank_count(K.crtc) != c0 ? "avanza" : "NO avanza", c0,
			 drm_crtc_vblank_count(K.crtc));
		drm_crtc_vblank_put(K.crtc);
	}
	K.cur = 0;
	K.bad = 0;
	K.last_vbl = 0;
	K.ops.name = dev->driver->name;
	K.ops.w = w;
	K.ops.h = h;
	K.ops.hw_vblank = K.hw_vblank;
	K.ops.back = kms_back;
	K.ops.flip = kms_flip;
	K.active = true;
	if (lkpi_kms_register(&K.ops)) {
		K.active = false;
		goto fail;
	}
	/* La ventana «AuroraOS DRM» que abrio el commit inicial del driver ya no pinta nada. */
	lkpi_scan_close(drm_crtc_index(K.crtc));
	drm_info(dev, "kms: %s %dx%d@%d con dos buffers, presentacion por page-flip\n", set->connectors[0]->name,
		 w, h, drm_mode_vrefresh(set->mode));
	return 0;
fail:
	kms_release();
	return 0;
}

static const struct drm_client_funcs kms_client_funcs = {
	.owner = THIS_MODULE,
	.unregister = kms_client_unregister,
	.restore = kms_client_restore,
	.hotplug = kms_client_hotplug,
};

/* Llamado al final de drm_client_setup() (patches/drm_client_setup.c.patch). */
void lkpi_kms_client_setup(struct drm_device *dev)
{
	char opt[8];
	bool want;

	if (K.inuse || !drm_core_check_feature(dev, DRIVER_MODESET))
		return;
	want = !strcmp(dev->driver->name, "i915") || !strcmp(dev->driver->name, "nvidia-drm");
	if (lkpi_cmdline_get("lkpi.kms", opt, sizeof(opt)))
		want = opt[0] == '1' || (opt[0] != '0' && want);
	if (!want)
		return;

	mutex_init(&K.lock);
	K.flush = !strcmp(dev->driver->name, "i915") || !strcmp(dev->driver->name, "nvidia-drm");
	if (lkpi_cmdline_get("lkpi.kms_flush", opt, sizeof(opt)))
		K.flush = opt[0] == '1';
	K.dev = dev;
	K.inuse = true;
	if (drm_client_init(dev, &K.client, "aurora-kms", &kms_client_funcs)) {
		K.inuse = false;
		return;
	}
	drm_client_register(&K.client);
}
