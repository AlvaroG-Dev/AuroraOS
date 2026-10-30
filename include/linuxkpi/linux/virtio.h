/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * virtio (L5): lo que un driver virtio necesita de cualquier transporte.
 * Virtqueues, struct virtio_device / virtio_driver y el bus "virtio" del
 * nucleo drivers/virtio de Linux, compilado en lkpi_virtio.kur.
 */
#ifndef _LKPI_LINUX_VIRTIO_H
#define _LKPI_LINUX_VIRTIO_H

#include <linux/types.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/device.h>
#include <linux/mod_devicetable.h>
#include <linux/gfp.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>
#include <linux/idr.h>
#include <linux/virtio_features.h>

/* Cola para registrar buffers que el dispositivo consume o rellena. */
struct virtqueue {
	struct list_head list;		/* colas del dispositivo */
	void (*callback)(struct virtqueue *vq);
	const char *name;
	struct virtio_device *vdev;
	unsigned int index;
	unsigned int num_free;
	unsigned int num_max;
	bool reset;
	void *priv;
};

struct vduse_vq_group;

/* Quien hace el DMA de un dispositivo: un struct device (el caso de la capa). */
union virtio_map {
	struct device *dma_dev;
	struct vduse_vq_group *group;
};

int virtqueue_add_outbuf(struct virtqueue *vq, struct scatterlist sg[], unsigned int num,
			 void *data, gfp_t gfp);
int virtqueue_add_inbuf(struct virtqueue *vq, struct scatterlist sg[], unsigned int num,
			void *data, gfp_t gfp);
int virtqueue_add_inbuf_ctx(struct virtqueue *vq, struct scatterlist sg[], unsigned int num,
			    void *data, void *ctx, gfp_t gfp);
int virtqueue_add_inbuf_premapped(struct virtqueue *vq, struct scatterlist *sg,
				  unsigned int num, void *data, void *ctx, gfp_t gfp);
int virtqueue_add_outbuf_premapped(struct virtqueue *vq, struct scatterlist *sg,
				   unsigned int num, void *data, gfp_t gfp);
int virtqueue_add_sgs(struct virtqueue *vq, struct scatterlist *sgs[], unsigned int out_sgs,
		      unsigned int in_sgs, void *data, gfp_t gfp);

struct device *virtqueue_dma_dev(struct virtqueue *vq);

bool virtqueue_kick(struct virtqueue *vq);
bool virtqueue_kick_prepare(struct virtqueue *vq);
bool virtqueue_notify(struct virtqueue *vq);

void *virtqueue_get_buf(struct virtqueue *vq, unsigned int *len);
void *virtqueue_get_buf_ctx(struct virtqueue *vq, unsigned int *len, void **ctx);

void virtqueue_disable_cb(struct virtqueue *vq);
bool virtqueue_enable_cb(struct virtqueue *vq);
unsigned virtqueue_enable_cb_prepare(struct virtqueue *vq);
bool virtqueue_poll(struct virtqueue *vq, unsigned);
bool virtqueue_enable_cb_delayed(struct virtqueue *vq);

void *virtqueue_detach_unused_buf(struct virtqueue *vq);

unsigned int virtqueue_get_vring_size(const struct virtqueue *vq);
bool virtqueue_is_broken(const struct virtqueue *vq);

const struct vring *virtqueue_get_vring(const struct virtqueue *vq);
dma_addr_t virtqueue_get_desc_addr(const struct virtqueue *vq);
dma_addr_t virtqueue_get_avail_addr(const struct virtqueue *vq);
dma_addr_t virtqueue_get_used_addr(const struct virtqueue *vq);

int virtqueue_resize(struct virtqueue *vq, u32 num,
		     void (*recycle)(struct virtqueue *vq, void *buf),
		     void (*recycle_done)(struct virtqueue *vq));
int virtqueue_reset(struct virtqueue *vq,
		    void (*recycle)(struct virtqueue *vq, void *buf),
		    void (*recycle_done)(struct virtqueue *vq));

/* Comando de la cola de administracion (la capa no la usa). */
struct virtio_admin_cmd {
	__le16 opcode;
	__le16 group_type;
	__le64 group_member_id;
	struct scatterlist *data_sg;
	struct scatterlist *result_sg;
	struct completion completion;
	u32 result_sg_size;
	int ret;
};

struct vringh_config_ops;
struct virtio_map_ops;

/* Un dispositivo virtio en el bus. */
struct virtio_device {
	int index;
	bool failed;
	bool config_core_enabled;	/* avisos de cambio de config: el nucleo */
	bool config_driver_disabled;	/* ... y el driver */
	bool config_change_pending;
	spinlock_t config_lock;
	spinlock_t vqs_list_lock;
	struct device dev;
	struct virtio_device_id id;
	const struct virtio_config_ops *config;
	const struct vringh_config_ops *vringh_config;
	const struct virtio_map_ops *map;
	struct list_head vqs;
	VIRTIO_DECLARE_FEATURES(features);	/* acordadas con el dispositivo */
	void *priv;
	union virtio_map vmap;
};

#define dev_to_virtio(_dev)	container_of(_dev, struct virtio_device, dev)

void virtio_add_status(struct virtio_device *dev, unsigned int status);
int register_virtio_device(struct virtio_device *dev);
void unregister_virtio_device(struct virtio_device *dev);
bool is_virtio_device(struct device *dev);

void virtio_break_device(struct virtio_device *dev);
void __virtio_unbreak_device(struct virtio_device *dev);
void __virtqueue_break(struct virtqueue *_vq);
void __virtqueue_unbreak(struct virtqueue *_vq);

void virtio_config_changed(struct virtio_device *dev);
void virtio_config_driver_disable(struct virtio_device *dev);
void virtio_config_driver_enable(struct virtio_device *dev);

void virtio_reset_device(struct virtio_device *dev);
void virtio_device_shutdown(struct virtio_device *dev);
int virtio_device_reset_prepare(struct virtio_device *dev);
int virtio_device_reset_done(struct virtio_device *dev);

size_t virtio_max_dma_size(const struct virtio_device *vdev);

#define virtio_device_for_each_vq(vdev, vq) \
	list_for_each_entry(vq, &(vdev)->vqs, list)

struct virtio_driver {
	struct device_driver driver;
	const struct virtio_device_id *id_table;
	const unsigned int *feature_table;
	unsigned int feature_table_size;
	const unsigned int *feature_table_legacy;
	unsigned int feature_table_size_legacy;
	int (*validate)(struct virtio_device *dev);
	int (*probe)(struct virtio_device *dev);
	void (*scan)(struct virtio_device *dev);
	void (*remove)(struct virtio_device *dev);
	void (*config_changed)(struct virtio_device *dev);
	int (*freeze)(struct virtio_device *dev);
	int (*restore)(struct virtio_device *dev);
	int (*reset_prepare)(struct virtio_device *dev);
	int (*reset_done)(struct virtio_device *dev);
	void (*shutdown)(struct virtio_device *dev);
};

#define drv_to_virtio(__drv)	container_of(__drv, struct virtio_driver, driver)

#define register_virtio_driver(drv) __register_virtio_driver(drv, THIS_MODULE)
int __register_virtio_driver(struct virtio_driver *drv, struct module *owner);
void unregister_virtio_driver(struct virtio_driver *drv);

#define module_virtio_driver(__virtio_driver) \
	module_driver(__virtio_driver, register_virtio_driver, unregister_virtio_driver)

/* Memoria y mapeos DMA de las colas, por el dispositivo que hace el DMA. */
void *virtqueue_map_alloc_coherent(struct virtio_device *vdev, union virtio_map mapping_token,
				   size_t size, dma_addr_t *dma_handle, gfp_t gfp);
void virtqueue_map_free_coherent(struct virtio_device *vdev, union virtio_map mapping_token,
				 size_t size, void *vaddr, dma_addr_t dma_handle);
dma_addr_t virtqueue_map_page_attrs(const struct virtqueue *_vq, struct page *page,
				    unsigned long offset, size_t size,
				    enum dma_data_direction dir, unsigned long attrs);
void virtqueue_unmap_page_attrs(const struct virtqueue *_vq, dma_addr_t dma_handle,
				size_t size, enum dma_data_direction dir, unsigned long attrs);
dma_addr_t virtqueue_map_single_attrs(const struct virtqueue *_vq, void *ptr, size_t size,
				      enum dma_data_direction dir, unsigned long attrs);
void virtqueue_unmap_single_attrs(const struct virtqueue *_vq, dma_addr_t addr, size_t size,
				  enum dma_data_direction dir, unsigned long attrs);
int virtqueue_map_mapping_error(const struct virtqueue *_vq, dma_addr_t addr);
bool virtqueue_map_need_sync(const struct virtqueue *_vq, dma_addr_t addr);
void virtqueue_map_sync_single_range_for_cpu(const struct virtqueue *_vq, dma_addr_t addr,
					     unsigned long offset, size_t size,
					     enum dma_data_direction dir);
void virtqueue_map_sync_single_range_for_device(const struct virtqueue *_vq, dma_addr_t addr,
						unsigned long offset, size_t size,
						enum dma_data_direction dir);

/* Sin CONFIG_VIRTIO_DEBUG: stubs vacios */
static inline void virtio_debug_device_init(struct virtio_device *dev) { }
static inline void virtio_debug_device_exit(struct virtio_device *dev) { }
static inline void virtio_debug_device_filter_features(struct virtio_device *dev) { }
static inline void virtio_debug_init(void) { }
static inline void virtio_debug_exit(void) { }

#endif
