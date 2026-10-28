// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pruebas del nucleo DRM/KMS de Linux (L5, K9-drm): un driver DRM de juguete
 * (pipe simple + GEM shmem + conector virtual) que hace un modeset atomico
 * completo desde el kernel. Requiere lkpi_drm.kur cargado.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_edid.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>
#include "lkpi_test.h"

struct kt_drm {
	struct drm_device dev;
	struct drm_simple_display_pipe pipe;
	struct drm_connector conn;
	int enables, updates, disables;
};

static int kt_conn_get_modes(struct drm_connector *c)
{
	int n = drm_add_modes_noedid(c, 1024, 768);

	drm_set_preferred_mode(c, 800, 600);
	return n;
}

static const struct drm_connector_helper_funcs kt_conn_helper = {
	.get_modes = kt_conn_get_modes,
};

static const struct drm_connector_funcs kt_conn_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static void kt_enable(struct drm_simple_display_pipe *p, struct drm_crtc_state *cs,
		      struct drm_plane_state *ps)
{
	container_of(p, struct kt_drm, pipe)->enables++;
}

static void kt_disable(struct drm_simple_display_pipe *p)
{
	container_of(p, struct kt_drm, pipe)->disables++;
}

static void kt_update(struct drm_simple_display_pipe *p, struct drm_plane_state *old)
{
	container_of(p, struct kt_drm, pipe)->updates++;
}

static const struct drm_simple_display_pipe_funcs kt_pipe_funcs = {
	.enable = kt_enable,
	.disable = kt_disable,
	.update = kt_update,
};

static const struct drm_mode_config_funcs kt_mode_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct drm_framebuffer_funcs kt_fb_funcs = {
	.destroy = drm_gem_fb_destroy,
	.create_handle = drm_gem_fb_create_handle,
};

DEFINE_DRM_GEM_FOPS(kt_fops);

static const struct drm_driver kt_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_RENDER,
	.fops = &kt_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
	.name = "kt_drm",
	.desc = "LinuxKPI test",
	.major = 1,
	.minor = 0,
};

static const u32 kt_formats[] = { DRM_FORMAT_XRGB8888 };

static void test_drm(void)
{
	struct platform_device *pdev;
	struct kt_drm *k;
	struct drm_mode_set set = {};
	struct drm_modeset_acquire_ctx ctx;
	struct drm_gem_shmem_object *shmem;
	struct drm_mode_fb_cmd2 cmd = {};
	struct drm_framebuffer *fb;
	struct drm_display_mode *mode;
	char *data = NULL;
	size_t len = 0;
	int ret, w = 800, h = 600;
	u32 *map;

	pdev = platform_device_register_simple("kt-drm", -1, NULL, 0);
	KT_ASSERT(!IS_ERR(pdev), "platform_device");
	if (IS_ERR(pdev))
		return;
	k = devm_drm_dev_alloc(&pdev->dev, &kt_driver, struct kt_drm, dev);
	KT_ASSERT(!IS_ERR(k), "devm_drm_dev_alloc: %ld", IS_ERR(k) ? PTR_ERR(k) : 0);
	if (IS_ERR(k))
		goto out_pdev;

	ret = drmm_mode_config_init(&k->dev);
	KT_EQ(ret, 0);
	k->dev.mode_config.min_width = 64;
	k->dev.mode_config.min_height = 64;
	k->dev.mode_config.max_width = 2048;
	k->dev.mode_config.max_height = 2048;
	k->dev.mode_config.funcs = &kt_mode_funcs;

	ret = drm_connector_init(&k->dev, &k->conn, &kt_conn_funcs, DRM_MODE_CONNECTOR_VIRTUAL);
	KT_EQ(ret, 0);
	drm_connector_helper_add(&k->conn, &kt_conn_helper);
	ret = drm_simple_display_pipe_init(&k->dev, &k->pipe, &kt_pipe_funcs, kt_formats, 1, NULL,
					   &k->conn);
	KT_EQ(ret, 0);
	drm_mode_config_reset(&k->dev);

	ret = drm_dev_register(&k->dev, 0);
	KT_EQ(ret, 0);
	KT_ASSERT(k->dev.primary && k->dev.render, "minors primary/render");
	if (ret)
		goto out_pdev;
	KT_ASSERT(drm_dev_is_unplugged(&k->dev) == false, "no desenchufado");
	KT_EQ(k->dev.primary->type, DRM_MINOR_PRIMARY);
	KT_EQ(k->dev.render->type, DRM_MINOR_RENDER);

	/* /sys/class/drm/cardN lo crea device_create(drm_class) */
	{
		char path[64];

		snprintf(path, sizeof(path), "class/drm/card%d", k->dev.primary->index);
		KT_ASSERT(kt_sysfs_get(path, &data, &len) != KT_SYSFS_NONE, "sysfs: %s", path);
		if (data) {
			kt_sysfs_free(data);
			data = NULL;
		}
	}

	/* el conector sondea modos: 800x600 preferido */
	mutex_lock(&k->dev.mode_config.mutex);
	ret = k->conn.funcs->fill_modes(&k->conn, 4096, 4096);
	mutex_unlock(&k->dev.mode_config.mutex);
	KT_ASSERT(ret >= 2, "modos del conector: %d", ret);
	KT_EQ(k->conn.status, connector_status_connected);
	mode = list_first_entry_or_null(&k->conn.modes, struct drm_display_mode, head);
	KT_ASSERT(mode && mode->hdisplay == w && mode->vdisplay == h, "modo preferido primero");

	/* framebuffer sobre un objeto shmem */
	shmem = drm_gem_shmem_create(&k->dev, (size_t)w * h * 4);
	KT_ASSERT(!IS_ERR(shmem), "drm_gem_shmem_create");
	if (IS_ERR(shmem))
		goto out_unreg;
	cmd.width = w;
	cmd.height = h;
	cmd.pixel_format = DRM_FORMAT_XRGB8888;
	cmd.pitches[0] = w * 4;
	fb = kzalloc(sizeof(*fb), GFP_KERNEL);
	if (!fb) {
		drm_gem_object_put(&shmem->base);
		goto out_unreg;
	}
	drm_helper_mode_fill_fb_struct(&k->dev, fb, drm_get_format_info(&k->dev, DRM_FORMAT_XRGB8888, 0),
				       &cmd);
	fb->obj[0] = &shmem->base;
	ret = drm_framebuffer_init(&k->dev, fb, &kt_fb_funcs);
	KT_EQ(ret, 0);

	/* modeset atomico completo */
	drm_modeset_acquire_init(&ctx, 0);
	set.crtc = &k->pipe.crtc;
	set.fb = fb;
	set.mode = mode;
	set.connectors = (struct drm_connector **)&(struct drm_connector *){ &k->conn };
	set.num_connectors = 1;
retry:
	ret = drm_modeset_lock_all_ctx(&k->dev, &ctx);
	if (!ret)
		ret = drm_atomic_helper_set_config(&set, &ctx);
	if (ret == -EDEADLK) {
		drm_modeset_backoff(&ctx);
		goto retry;
	}
	KT_EQ(ret, 0);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	KT_EQ(k->enables, 1);
	KT_ASSERT(k->updates >= 1, "plano actualizado (%d)", k->updates);
	KT_ASSERT(k->pipe.crtc.state && k->pipe.crtc.state->active, "crtc activo");
	KT_ASSERT(k->pipe.plane.state && k->pipe.plane.state->fb == fb, "plano con el fb");

	/* los datos del objeto shmem se pueden mapear y escribir */
	{
		struct iosys_map m;

		ret = drm_gem_vmap(&shmem->base, &m);
		KT_EQ(ret, 0);
		if (!ret) {
			map = m.vaddr;
			map[0] = 0xdeadbeef;
			map[w * h - 1] = 0x12345678;
			KT_EQ(map[0], 0xdeadbeef);
			drm_gem_vunmap(&shmem->base, &m);
		}
	}

	/* apagar la salida */
	drm_modeset_acquire_init(&ctx, 0);
	set.fb = NULL;
	set.mode = NULL;
	set.num_connectors = 0;
retry2:
	ret = drm_modeset_lock_all_ctx(&k->dev, &ctx);
	if (!ret)
		ret = drm_atomic_helper_set_config(&set, &ctx);
	if (ret == -EDEADLK) {
		drm_modeset_backoff(&ctx);
		goto retry2;
	}
	KT_EQ(ret, 0);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	KT_EQ(k->disables, 1);
	drm_framebuffer_put(fb);

out_unreg:
	drm_dev_unplug(&k->dev);
	drm_atomic_helper_shutdown(&k->dev);
out_pdev:
	platform_device_unregister(pdev);
}

static int __init test_drm_init(void)
{
	test_drm();
	return 0;
}

static void __exit test_drm_exit(void)
{
}

module_init(test_drm_init);
module_exit(test_drm_exit);
MODULE_LICENSE("GPL");
