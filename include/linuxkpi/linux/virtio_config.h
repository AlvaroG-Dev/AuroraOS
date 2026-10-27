/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Operaciones del transporte virtio (struct virtio_config_ops), acceso al
 * espacio de configuracion y conversion de endianness segun las features.
 */
#ifndef _LKPI_LINUX_VIRTIO_CONFIG_H
#define _LKPI_LINUX_VIRTIO_CONFIG_H

#include <linux/err.h>
#include <linux/bug.h>
#include <linux/virtio.h>
#include <linux/virtio_byteorder.h>
#include <linux/compiler_types.h>
#include <linux/rcupdate.h>
#include <linux/typecheck.h>
#include <uapi/linux/virtio_config.h>

struct irq_affinity;

struct virtio_shm_region {
	u64 addr;
	u64 len;
};

typedef void vq_callback_t(struct virtqueue *);

/* Descripcion de una cola que pide find_vqs() */
struct virtqueue_info {
	const char *name;
	vq_callback_t *callback;
	bool ctx;
};

struct virtio_config_ops {
	void (*get)(struct virtio_device *vdev, unsigned offset, void *buf, unsigned len);
	void (*set)(struct virtio_device *vdev, unsigned offset, const void *buf, unsigned len);
	u32 (*generation)(struct virtio_device *vdev);
	u8 (*get_status)(struct virtio_device *vdev);
	void (*set_status)(struct virtio_device *vdev, u8 status);
	void (*reset)(struct virtio_device *vdev);
	int (*find_vqs)(struct virtio_device *vdev, unsigned int nvqs, struct virtqueue *vqs[],
			struct virtqueue_info vqs_info[], struct irq_affinity *desc);
	void (*del_vqs)(struct virtio_device *);
	void (*synchronize_cbs)(struct virtio_device *);
	u64 (*get_features)(struct virtio_device *vdev);
	void (*get_extended_features)(struct virtio_device *vdev, u64 *features);
	int (*finalize_features)(struct virtio_device *vdev);
	const char *(*bus_name)(struct virtio_device *vdev);
	int (*set_vq_affinity)(struct virtqueue *vq, const struct cpumask *cpu_mask);
	const struct cpumask *(*get_vq_affinity)(struct virtio_device *vdev, int index);
	bool (*get_shm_region)(struct virtio_device *vdev, struct virtio_shm_region *region, u8 id);
	int (*disable_vq_and_reset)(struct virtqueue *vq);
	int (*enable_vq_after_reset)(struct virtqueue *vq);
};

/* Mapeo DMA alternativo (VDUSE): la capa usa siempre el struct device. */
struct virtio_map_ops {
	dma_addr_t (*map_page)(union virtio_map map, struct page *page, unsigned long offset,
			       size_t size, enum dma_data_direction dir, unsigned long attrs);
	void (*unmap_page)(union virtio_map map, dma_addr_t map_handle, size_t size,
			   enum dma_data_direction dir, unsigned long attrs);
	void (*sync_single_for_cpu)(union virtio_map map, dma_addr_t map_handle, size_t size,
				    enum dma_data_direction dir);
	void (*sync_single_for_device)(union virtio_map map, dma_addr_t map_handle, size_t size,
				       enum dma_data_direction dir);
	void *(*alloc)(union virtio_map map, size_t size, dma_addr_t *map_handle, gfp_t gfp);
	void (*free)(union virtio_map map, size_t size, void *vaddr, dma_addr_t map_handle,
		     unsigned long attrs);
	bool (*need_sync)(union virtio_map map, dma_addr_t map_handle);
	int (*mapping_error)(union virtio_map map, dma_addr_t map_handle);
	size_t (*max_mapping_size)(union virtio_map map);
};

void virtio_check_driver_offered_feature(const struct virtio_device *vdev, unsigned int fbit);

static inline bool __virtio_test_bit(const struct virtio_device *vdev, unsigned int fbit)
{
	return virtio_features_test_bit(vdev->features_array, fbit);
}

static inline void __virtio_set_bit(struct virtio_device *vdev, unsigned int fbit)
{
	virtio_features_set_bit(vdev->features_array, fbit);
}

static inline void __virtio_clear_bit(struct virtio_device *vdev, unsigned int fbit)
{
	virtio_features_clear_bit(vdev->features_array, fbit);
}

/* Los drivers preguntan por sus features con esto; las de transporte, sin comprobar. */
static inline bool virtio_has_feature(const struct virtio_device *vdev, unsigned int fbit)
{
	if (fbit < VIRTIO_TRANSPORT_F_START)
		virtio_check_driver_offered_feature(vdev, fbit);
	return __virtio_test_bit(vdev, fbit);
}

static inline void virtio_get_features(struct virtio_device *vdev, u64 *features_out)
{
	if (vdev->config->get_extended_features) {
		vdev->config->get_extended_features(vdev, features_out);
		return;
	}
	virtio_features_from_u64(features_out, vdev->config->get_features(vdev));
}

/* Sin ACCESS_PLATFORM el dispositivo ve la memoria fisica del guest tal cual. */
static inline bool virtio_has_dma_quirk(const struct virtio_device *vdev)
{
	return !virtio_has_feature(vdev, VIRTIO_F_ACCESS_PLATFORM);
}

static inline int virtio_find_vqs(struct virtio_device *vdev, unsigned int nvqs,
				  struct virtqueue *vqs[], struct virtqueue_info vqs_info[],
				  struct irq_affinity *desc)
{
	return vdev->config->find_vqs(vdev, nvqs, vqs, vqs_info, desc);
}

static inline struct virtqueue *virtio_find_single_vq(struct virtio_device *vdev,
						      vq_callback_t *c, const char *n)
{
	struct virtqueue_info vqs_info[] = { { n, c }, };
	struct virtqueue *vq;
	int err = virtio_find_vqs(vdev, 1, &vq, vqs_info, NULL);

	return err < 0 ? ERR_PTR(err) : vq;
}

static inline void virtio_synchronize_cbs(struct virtio_device *dev)
{
	if (dev->config->synchronize_cbs)
		dev->config->synchronize_cbs(dev);
	else
		synchronize_rcu();
}

static inline void virtio_device_ready(struct virtio_device *dev)
{
	unsigned status = dev->config->get_status(dev);

	WARN_ON(status & VIRTIO_CONFIG_S_DRIVER_OK);
	dev->config->set_status(dev, status | VIRTIO_CONFIG_S_DRIVER_OK);
}

static inline const char *virtio_bus_name(struct virtio_device *vdev)
{
	if (!vdev->config->bus_name)
		return "virtio";
	return vdev->config->bus_name(vdev);
}

static inline int virtqueue_set_affinity(struct virtqueue *vq, const struct cpumask *cpu_mask)
{
	struct virtio_device *vdev = vq->vdev;

	if (vdev->config->set_vq_affinity)
		return vdev->config->set_vq_affinity(vq, cpu_mask);
	return 0;
}

static inline bool virtio_get_shm_region(struct virtio_device *vdev,
					 struct virtio_shm_region *region_out, u8 id)
{
	if (!vdev->config->get_shm_region)
		return false;
	return vdev->config->get_shm_region(vdev, region_out, id);
}

static inline bool virtio_is_little_endian(struct virtio_device *vdev)
{
	return virtio_has_feature(vdev, VIRTIO_F_VERSION_1) || virtio_legacy_is_little_endian();
}

static inline u16 virtio16_to_cpu(struct virtio_device *vdev, __virtio16 val)
{
	return __virtio16_to_cpu(virtio_is_little_endian(vdev), val);
}

static inline __virtio16 cpu_to_virtio16(struct virtio_device *vdev, u16 val)
{
	return __cpu_to_virtio16(virtio_is_little_endian(vdev), val);
}

static inline u32 virtio32_to_cpu(struct virtio_device *vdev, __virtio32 val)
{
	return __virtio32_to_cpu(virtio_is_little_endian(vdev), val);
}

static inline __virtio32 cpu_to_virtio32(struct virtio_device *vdev, u32 val)
{
	return __cpu_to_virtio32(virtio_is_little_endian(vdev), val);
}

static inline u64 virtio64_to_cpu(struct virtio_device *vdev, __virtio64 val)
{
	return __virtio64_to_cpu(virtio_is_little_endian(vdev), val);
}

static inline __virtio64 cpu_to_virtio64(struct virtio_device *vdev, u64 val)
{
	return __cpu_to_virtio64(virtio_is_little_endian(vdev), val);
}

#define virtio_to_cpu(vdev, x) \
	_Generic((x), \
		__u8: (x), \
		__virtio16: virtio16_to_cpu((vdev), (x)), \
		__virtio32: virtio32_to_cpu((vdev), (x)), \
		__virtio64: virtio64_to_cpu((vdev), (x)) \
		)

#define cpu_to_virtio(vdev, x, m) \
	_Generic((m), \
		__u8: (x), \
		__virtio16: cpu_to_virtio16((vdev), (x)), \
		__virtio32: cpu_to_virtio32((vdev), (x)), \
		__virtio64: cpu_to_virtio64((vdev), (x)) \
		)

#define __virtio_native_type(structname, member) \
	typeof(virtio_to_cpu(NULL, ((structname *)0)->member))

/* Lee/escribe un campo de la config (endianness del dispositivo) */
#define virtio_cread(vdev, structname, member, ptr)				\
	do {									\
		typeof(((structname *)0)->member) virtio_cread_v;		\
										\
		might_sleep();							\
		typecheck(typeof(virtio_to_cpu((vdev), virtio_cread_v)), *(ptr)); \
										\
		switch (sizeof(virtio_cread_v)) {				\
		case 1:								\
		case 2:								\
		case 4:								\
			vdev->config->get((vdev), offsetof(structname, member),	\
					  &virtio_cread_v, sizeof(virtio_cread_v));	\
			break;							\
		default:							\
			__virtio_cread_many((vdev), offsetof(structname, member), \
					    &virtio_cread_v, 1, sizeof(virtio_cread_v)); \
			break;							\
		}								\
		*(ptr) = virtio_to_cpu(vdev, virtio_cread_v);			\
	} while (0)

#define virtio_cwrite(vdev, structname, member, ptr)				\
	do {									\
		typeof(((structname *)0)->member) virtio_cwrite_v =		\
			cpu_to_virtio(vdev, *(ptr), ((structname *)0)->member);	\
										\
		might_sleep();							\
		typecheck(typeof(virtio_to_cpu((vdev), virtio_cwrite_v)), *(ptr)); \
										\
		vdev->config->set((vdev), offsetof(structname, member),		\
				  &virtio_cwrite_v, sizeof(virtio_cwrite_v));	\
	} while (0)

/* Campos siempre little endian (los de la config de dispositivos modernos) */
#define virtio_le_to_cpu(x) \
	_Generic((x), \
		__u8: (u8)(x), \
		__le16: (u16)le16_to_cpu(x), \
		__le32: (u32)le32_to_cpu(x), \
		__le64: (u64)le64_to_cpu(x) \
		)

#define virtio_cpu_to_le(x, m) \
	_Generic((m), \
		__u8: (x), \
		__le16: cpu_to_le16(x), \
		__le32: cpu_to_le32(x), \
		__le64: cpu_to_le64(x) \
		)

#define virtio_cread_le(vdev, structname, member, ptr)				\
	do {									\
		typeof(((structname *)0)->member) virtio_cread_v;		\
										\
		might_sleep();							\
		typecheck(typeof(virtio_le_to_cpu(virtio_cread_v)), *(ptr));	\
										\
		switch (sizeof(virtio_cread_v)) {				\
		case 1:								\
		case 2:								\
		case 4:								\
			vdev->config->get((vdev), offsetof(structname, member),	\
					  &virtio_cread_v, sizeof(virtio_cread_v));	\
			break;							\
		default:							\
			__virtio_cread_many((vdev), offsetof(structname, member), \
					    &virtio_cread_v, 1, sizeof(virtio_cread_v)); \
			break;							\
		}								\
		*(ptr) = virtio_le_to_cpu(virtio_cread_v);			\
	} while (0)

#define virtio_cwrite_le(vdev, structname, member, ptr)				\
	do {									\
		typeof(((structname *)0)->member) virtio_cwrite_v =		\
			virtio_cpu_to_le(*(ptr), ((structname *)0)->member);	\
										\
		might_sleep();							\
		typecheck(typeof(virtio_le_to_cpu(virtio_cwrite_v)), *(ptr));	\
										\
		vdev->config->set((vdev), offsetof(structname, member),		\
				  &virtio_cwrite_v, sizeof(virtio_cwrite_v));	\
	} while (0)

/* Lectura coherente de campos de mas de 4 bytes: repite hasta que la generacion no cambie */
static inline void __virtio_cread_many(struct virtio_device *vdev, unsigned int offset,
				       void *buf, size_t count, size_t bytes)
{
	u32 old, gen = vdev->config->generation ? vdev->config->generation(vdev) : 0;
	size_t i;

	might_sleep();
	do {
		old = gen;
		for (i = 0; i < count; i++)
			vdev->config->get(vdev, offset + bytes * i, buf + i * bytes, bytes);
		gen = vdev->config->generation ? vdev->config->generation(vdev) : 0;
	} while (gen != old);
}

static inline void virtio_cread_bytes(struct virtio_device *vdev, unsigned int offset,
				      void *buf, size_t len)
{
	__virtio_cread_many(vdev, offset, buf, len, 1);
}

static inline u8 virtio_cread8(struct virtio_device *vdev, unsigned int offset)
{
	u8 ret;

	might_sleep();
	vdev->config->get(vdev, offset, &ret, sizeof(ret));
	return ret;
}

static inline void virtio_cwrite8(struct virtio_device *vdev, unsigned int offset, u8 val)
{
	might_sleep();
	vdev->config->set(vdev, offset, &val, sizeof(val));
}

static inline u16 virtio_cread16(struct virtio_device *vdev, unsigned int offset)
{
	__virtio16 ret;

	might_sleep();
	vdev->config->get(vdev, offset, &ret, sizeof(ret));
	return virtio16_to_cpu(vdev, ret);
}

static inline void virtio_cwrite16(struct virtio_device *vdev, unsigned int offset, u16 val)
{
	__virtio16 v;

	might_sleep();
	v = cpu_to_virtio16(vdev, val);
	vdev->config->set(vdev, offset, &v, sizeof(v));
}

static inline u32 virtio_cread32(struct virtio_device *vdev, unsigned int offset)
{
	__virtio32 ret;

	might_sleep();
	vdev->config->get(vdev, offset, &ret, sizeof(ret));
	return virtio32_to_cpu(vdev, ret);
}

static inline void virtio_cwrite32(struct virtio_device *vdev, unsigned int offset, u32 val)
{
	__virtio32 v;

	might_sleep();
	v = cpu_to_virtio32(vdev, val);
	vdev->config->set(vdev, offset, &v, sizeof(v));
}

static inline u64 virtio_cread64(struct virtio_device *vdev, unsigned int offset)
{
	__virtio64 ret;

	__virtio_cread_many(vdev, offset, &ret, 1, sizeof(ret));
	return virtio64_to_cpu(vdev, ret);
}

static inline void virtio_cwrite64(struct virtio_device *vdev, unsigned int offset, u64 val)
{
	__virtio64 v;

	might_sleep();
	v = cpu_to_virtio64(vdev, val);
	vdev->config->set(vdev, offset, &v, sizeof(v));
}

/* Lee un campo solo si la feature esta negociada (-ENOENT si no) */
#define virtio_cread_feature(vdev, fbit, structname, member, ptr)		\
	({									\
		int _r = 0;							\
		if (!virtio_has_feature(vdev, fbit))				\
			_r = -ENOENT;						\
		else								\
			virtio_cread((vdev), structname, member, ptr);		\
		_r;								\
	})

#define virtio_cread_le_feature(vdev, fbit, structname, member, ptr)		\
	({									\
		int _r = 0;							\
		if (!virtio_has_feature(vdev, fbit))				\
			_r = -ENOENT;						\
		else								\
			virtio_cread_le((vdev), structname, member, ptr);	\
		_r;								\
	})

#endif
