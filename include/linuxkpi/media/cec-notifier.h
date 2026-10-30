/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_MEDIA_CEC_NOTIFIER_H
#define _LKPI_MEDIA_CEC_NOTIFIER_H
#include <linux/types.h>
#include <linux/device.h>
struct cec_notifier;
struct cec_connector_info;
static inline struct cec_notifier *cec_notifier_conn_register(struct device *hdmi_dev, const char *port_name,
							      const struct cec_connector_info *conn_info) { return NULL; }
static inline void cec_notifier_conn_unregister(struct cec_notifier *n) {}
static inline void cec_notifier_set_phys_addr_from_edid(struct cec_notifier *n, const struct edid *edid) {}
static inline void cec_notifier_phys_addr_invalidate(struct cec_notifier *n) {}

struct cec_connector_info { int type; };
struct drm_connector;
static inline void cec_notifier_set_phys_addr(struct cec_notifier *n, u16 pa) {}
static inline void cec_fill_conn_info_from_drm(struct cec_connector_info *conn_info, const struct drm_connector *connector) {}
#endif
