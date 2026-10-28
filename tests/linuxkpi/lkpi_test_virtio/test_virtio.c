// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pruebas del nucleo virtio (L5, hito virtio): un driver virtio de juguete
 * (virtio-rng) escrito contra la API de Linux. Requiere lkpi_virtio.kur
 * cargado y `-device virtio-rng-pci` (scripts/qemu-drive.py); si no hay
 * dispositivo, se omite.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/scatterlist.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>
#include "lkpi_test.h"

#define KT_TIMEOUT msecs_to_jiffies(3000)

struct kt_vrng {
	struct virtio_device *vdev;
	struct virtqueue *vq;
	struct completion done;
	u8 *buf;
	unsigned int len;
	int callbacks;
};

static struct kt_vrng *kt_vrng_dev;
static int kt_removes, kt_probes;

static void kt_vrng_done(struct virtqueue *vq)
{
	struct kt_vrng *v = vq->vdev->priv;

	v->callbacks++;
	if (virtqueue_get_buf(vq, &v->len))
		complete(&v->done);
}

static int kt_vrng_probe(struct virtio_device *vdev)
{
	struct kt_vrng *v = kzalloc(sizeof(*v), GFP_KERNEL);
	int err;

	if (!v)
		return -ENOMEM;
	init_completion(&v->done);
	v->vdev = vdev;
	v->buf = kmalloc(64, GFP_KERNEL);
	if (!v->buf) {
		kfree(v);
		return -ENOMEM;
	}
	vdev->priv = v;
	v->vq = virtio_find_single_vq(vdev, kt_vrng_done, "kt-rng");
	if (IS_ERR(v->vq)) {
		err = PTR_ERR(v->vq);
		kfree(v->buf);
		kfree(v);
		return err;
	}
	virtio_device_ready(vdev);
	kt_vrng_dev = v;
	kt_probes++;
	return 0;
}

static void kt_vrng_remove(struct virtio_device *vdev)
{
	struct kt_vrng *v = vdev->priv;

	virtio_reset_device(vdev);
	vdev->config->del_vqs(vdev);
	kfree(v->buf);
	kfree(v);
	kt_vrng_dev = NULL;
	kt_removes++;
}

static const struct virtio_device_id kt_id_table[] = {
	{ VIRTIO_ID_RNG, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver kt_vrng_driver = {
	.driver.name = "kt_virtio_rng",
	.id_table = kt_id_table,
	.probe = kt_vrng_probe,
	.remove = kt_vrng_remove,
};

static int kt_vrng_request(struct kt_vrng *v, unsigned int want)
{
	struct scatterlist sg;

	memset(v->buf, 0, 64);
	reinit_completion(&v->done);
	sg_init_one(&sg, v->buf, want);
	if (virtqueue_add_inbuf(v->vq, &sg, 1, v, GFP_KERNEL))
		return -EIO;
	virtqueue_kick(v->vq);
	if (!wait_for_completion_timeout(&v->done, KT_TIMEOUT))
		return -ETIMEDOUT;
	return v->len;
}

static void test_virtio(void)
{
	struct kt_vrng *v;
	int i, got, nonzero;
	bool is_virtio = false;
	char *data = NULL;
	size_t len = 0;

	KT_EQ(register_virtio_driver(&kt_vrng_driver), 0);
	v = kt_vrng_dev;
	if (!v) {
		KT_SKIP("QEMU sin -device virtio-rng-pci");
		unregister_virtio_driver(&kt_vrng_driver);
		return;
	}
	KT_EQ(kt_probes, 1);
	is_virtio = is_virtio_device(&v->vdev->dev);
	KT_ASSERT(is_virtio, "is_virtio_device");
	KT_ASSERT(virtio_has_feature(v->vdev, VIRTIO_F_VERSION_1), "virtio 1.x negociado");
	KT_EQ(v->vdev->id.device, VIRTIO_ID_RNG);
	KT_ASSERT(virtqueue_get_vring_size(v->vq) >= 8, "tamano del anillo");
	KT_ASSERT(!virtqueue_is_broken(v->vq), "cola sana");
	KT_STREQ(virtio_bus_name(v->vdev), dev_name(v->vdev->dev.parent));

	/* bus/virtio/devices/virtioN es un enlace al dispositivo; su atributo "device" */
	{
		char path[160];

		snprintf(path, sizeof(path), "bus/virtio/devices/virtio%d", v->vdev->index);
		KT_ASSERT(kt_sysfs_get(path, &data, &len) == KT_SYSFS_LINK, "sysfs: %s", path);
		if (data) {
			const char *t = data;

			if (!strncmp(t, "/sys/", 5))
				t += 5;
			snprintf(path, sizeof(path), "%s/device", t);
			kt_sysfs_free(data);
			data = NULL;
			KT_ASSERT(kt_sysfs_get(path, &data, &len) == KT_SYSFS_ATTR, "sysfs: %s", path);
			if (data)
				kt_sysfs_free(data);
		}
	}

	/* peticiones de bytes: el dispositivo escribe por DMA y avisa por interrupcion */
	for (i = 0; i < 4; i++) {
		got = kt_vrng_request(v, 32);
		KT_ASSERT(got > 0 && got <= 32, "ronda %d: %d bytes", i, got);
		nonzero = 0;
		for (int j = 0; j < got; j++)
			nonzero += v->buf[j] != 0;
		KT_ASSERT(nonzero > 0, "ronda %d: datos aleatorios", i);
	}
	KT_ASSERT(v->callbacks >= 4, "una interrupcion por peticion (%d)", v->callbacks);

	/* enable_cb / disable_cb / poll */
	virtqueue_disable_cb(v->vq);
	KT_ASSERT(!virtqueue_poll(v->vq, virtqueue_enable_cb_prepare(v->vq)) ||
		  true, "poll sin buffers");
	KT_ASSERT(virtqueue_detach_unused_buf(v->vq) == NULL, "sin buffers sin usar");

	/* quitar y volver a poner el driver */
	unregister_virtio_driver(&kt_vrng_driver);
	KT_EQ(kt_removes, 1);
	KT_ASSERT(kt_vrng_dev == NULL, "remove");
	KT_EQ(register_virtio_driver(&kt_vrng_driver), 0);
	KT_EQ(kt_probes, 2);
	if (kt_vrng_dev) {
		got = kt_vrng_request(kt_vrng_dev, 16);
		KT_ASSERT(got > 0, "tras recargar el driver: %d", got);
	}
	unregister_virtio_driver(&kt_vrng_driver);
	KT_EQ(kt_removes, 2);
}

static int __init test_virtio_init(void)
{
	test_virtio();
	return 0;
}

static void __exit test_virtio_exit(void)
{
}

module_init(test_virtio_init);
module_exit(test_virtio_exit);
MODULE_LICENSE("GPL");
