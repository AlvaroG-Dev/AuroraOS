// SPDX-License-Identifier: GPL-2.0-only
/* request_firmware (K6, L4) sobre el cargador de Aurora. Ver linux/firmware.h. */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "../aurora.h"
#include "../lkpi_internal.h"

static int lkpi_fw_load(const struct aurora_firmware **afw, const char *name,
			struct device *device, bool warn)
{
	int rc = aurora_request_firmware(afw, name);

	if (rc && warn)
		dev_err(device, "Direct firmware load for %s failed with error %d\n", name, rc);
	return rc;
}

int lkpi_request_firmware(const struct firmware **fw, const char *name, struct device *device,
			  bool warn)
{
	const struct aurora_firmware *afw;
	struct firmware *f;
	int rc;

	if (!fw)
		return -EINVAL;
	*fw = NULL;
	if (!name || !name[0])
		return -EINVAL;
	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	rc = lkpi_fw_load(&afw, name, device, warn);
	if (rc) {
		kfree(f);
		return rc;
	}
	f->size = afw->size;
	f->data = afw->data;
	f->priv = (void *)afw;
	*fw = f;
	return 0;
}
EXPORT_SYMBOL(lkpi_request_firmware);

void lkpi_release_firmware(const struct firmware *fw)
{
	if (!fw)
		return;
	if (fw->priv)
		aurora_release_firmware(fw->priv);
	kfree(fw);
}
EXPORT_SYMBOL(lkpi_release_firmware);

int lkpi_request_firmware_into_buf(const struct firmware **firmware_p, const char *name,
				   struct device *device, void *buf, size_t size,
				   size_t offset, bool partial)
{
	const struct aurora_firmware *afw;
	struct firmware *f;
	size_t n;
	int rc;

	*firmware_p = NULL;
	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	rc = lkpi_fw_load(&afw, name, device, true);
	if (rc) {
		kfree(f);
		return rc;
	}
	if (offset > afw->size || (!partial && afw->size > size)) {
		aurora_release_firmware(afw);
		kfree(f);
		return -EFBIG;
	}
	n = min(size, afw->size - offset);
	memcpy(buf, afw->data + offset, n);
	aurora_release_firmware(afw);
	f->data = buf;
	f->size = n;
	*firmware_p = f;
	return 0;
}
EXPORT_SYMBOL(lkpi_request_firmware_into_buf);

struct lkpi_fw_work {
	struct work_struct work;
	struct device *device;
	void *context;
	void (*cont)(const struct firmware *fw, void *context);
	char name[];
};

static void lkpi_fw_work_fn(struct work_struct *w)
{
	struct lkpi_fw_work *fw_work = container_of(w, struct lkpi_fw_work, work);
	const struct firmware *fw = NULL;

	lkpi_request_firmware(&fw, fw_work->name, fw_work->device, true);
	fw_work->cont(fw, fw_work->context);	/* cont libera fw (o recibe NULL) */
	put_device(fw_work->device);
	kfree(fw_work);
}

int lkpi_request_firmware_nowait(struct module *module, bool uevent, const char *name,
				 struct device *device, gfp_t gfp, void *context,
				 void (*cont)(const struct firmware *fw, void *context))
{
	struct lkpi_fw_work *fw_work;

	if (!name || !cont)
		return -EINVAL;
	fw_work = kzalloc(sizeof(*fw_work) + strlen(name) + 1, gfp);
	if (!fw_work)
		return -ENOMEM;
	strcpy(fw_work->name, name);
	fw_work->device = get_device(device);
	fw_work->context = context;
	fw_work->cont = cont;
	INIT_WORK(&fw_work->work, lkpi_fw_work_fn);
	schedule_work(&fw_work->work);
	return 0;
}
EXPORT_SYMBOL(lkpi_request_firmware_nowait);
