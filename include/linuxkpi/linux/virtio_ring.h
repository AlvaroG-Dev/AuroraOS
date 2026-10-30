/* SPDX-License-Identifier: GPL-2.0-only */
/* Anillos (virtqueues) split y packed: constructores y barreras. */
#ifndef _LKPI_LINUX_VIRTIO_RING_H
#define _LKPI_LINUX_VIRTIO_RING_H

#include <asm/barrier.h>
#include <linux/virtio.h>
#include <linux/irqreturn.h>
#include <uapi/linux/virtio_ring.h>

/* weak_barriers: host y guest comparten CPUs (virtio-pci en una VM) */
static inline void virtio_mb(bool weak_barriers)
{
	if (weak_barriers)
		virt_mb();
	else
		mb();
}

static inline void virtio_rmb(bool weak_barriers)
{
	if (weak_barriers)
		virt_rmb();
	else
		dma_rmb();
}

static inline void virtio_wmb(bool weak_barriers)
{
	if (weak_barriers)
		virt_wmb();
	else
		dma_wmb();
}

#define virtio_store_mb(weak_barriers, p, v)	\
do {						\
	if (weak_barriers) {			\
		virt_store_mb(*p, v);		\
	} else {				\
		WRITE_ONCE(*p, v);		\
		mb();				\
	}					\
} while (0)

struct virtqueue *vring_create_virtqueue(unsigned int index, unsigned int num,
					 unsigned int vring_align, struct virtio_device *vdev,
					 bool weak_barriers, bool may_reduce_num, bool ctx,
					 bool (*notify)(struct virtqueue *vq),
					 void (*callback)(struct virtqueue *vq),
					 const char *name);

struct virtqueue *vring_create_virtqueue_map(unsigned int index, unsigned int num,
					     unsigned int vring_align, struct virtio_device *vdev,
					     bool weak_barriers, bool may_reduce_num, bool ctx,
					     bool (*notify)(struct virtqueue *vq),
					     void (*callback)(struct virtqueue *vq),
					     const char *name, union virtio_map map);

struct virtqueue *vring_new_virtqueue(unsigned int index, unsigned int num,
				      unsigned int vring_align, struct virtio_device *vdev,
				      bool weak_barriers, bool ctx, void *pages,
				      bool (*notify)(struct virtqueue *vq),
				      void (*callback)(struct virtqueue *vq),
				      const char *name);

void vring_del_virtqueue(struct virtqueue *vq);
void vring_transport_features(struct virtio_device *vdev);
irqreturn_t vring_interrupt(int irq, void *_vq);
u32 vring_notification_data(struct virtqueue *_vq);

#endif
