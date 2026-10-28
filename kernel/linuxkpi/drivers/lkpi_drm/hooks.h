/* SPDX-License-Identifier: GPL-2.0-only */
/* Ganchos de la capa dentro de drm_atomic_helper.c (patches/drm_atomic_helper.c.patch). */
#ifndef _LKPI_DRM_HOOKS_H
#define _LKPI_DRM_HOOKS_H

#include <linux/types.h>

struct drm_atomic_state;
struct drm_device;
struct drm_driver;
struct device;

void lkpi_drm_pace(struct drm_atomic_state *state);
void lkpi_drm_present(struct drm_atomic_state *state);

/* Presentacion del compositor por KMS (kms_present.c). */
bool lkpi_kms_committing(void);
void lkpi_kms_client_setup(struct drm_device *dev);

const struct drm_driver *lkpi_drm_driver_fixup(struct device *parent, const struct drm_driver *drv);

#endif
