/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * request_firmware (K6, L4) sobre el cargador de Aurora: busca `name` en
 * /lib/firmware/updates y /lib/firmware (lo que instala make iso desde
 * linux-firmware, kernel/linuxkpi/firmware.list). Sin respaldo por uevent ni
 * ficheros comprimidos. _nowait corre en system_wq.
 */
#ifndef _LKPI_LINUX_FIRMWARE_H
#define _LKPI_LINUX_FIRMWARE_H

#include <linux/types.h>
#include <linux/gfp.h>

#define FW_ACTION_NOUEVENT 0
#define FW_ACTION_UEVENT 1

struct device;
struct module;

struct firmware {
	size_t size;
	const u8 *data;
	void *priv;			/* struct firmware de Aurora (o NULL: into_buf) */
};

int lkpi_request_firmware(const struct firmware **fw, const char *name, struct device *device,
			  bool warn);
int lkpi_request_firmware_nowait(struct module *module, bool uevent, const char *name,
				 struct device *device, gfp_t gfp, void *context,
				 void (*cont)(const struct firmware *fw, void *context));
int lkpi_request_firmware_into_buf(const struct firmware **firmware_p, const char *name,
				   struct device *device, void *buf, size_t size,
				   size_t offset, bool partial);
void lkpi_release_firmware(const struct firmware *fw);

#define request_firmware(f, n, d) lkpi_request_firmware(f, n, d, true)
#define firmware_request_nowarn(f, n, d) lkpi_request_firmware(f, n, d, false)
#define firmware_request_platform(f, n, d) lkpi_request_firmware(f, n, d, true)
#define request_firmware_direct(f, n, d) lkpi_request_firmware(f, n, d, false)
#define request_firmware_nowait(m, u, n, d, g, c, f) \
	lkpi_request_firmware_nowait(m, u, n, d, g, c, f)
#define request_firmware_into_buf(f, n, d, b, s) \
	lkpi_request_firmware_into_buf(f, n, d, b, s, 0, false)
#define request_partial_firmware_into_buf(f, n, d, b, s, o) \
	lkpi_request_firmware_into_buf(f, n, d, b, s, o, true)
#define release_firmware(f) lkpi_release_firmware(f)
#define firmware_request_cache(d, n) 0

#endif
