/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * dma_buf (K1-DMA, L4), enlazado con el PRIME de Aurora:
 *
 * - dma_buf_export(): crea el dma_buf y su struct file de la capa
 *   (dmabuf->file; get_dma_buf/dma_buf_put son sus referencias).
 * - dma_buf_fd(): el fd que ve el usuario es un dmabuf de Aurora sobre las
 *   paginas del exportador (obtenidas con un attachment interno de la CPU):
 *   sirve para PRIME_FD_TO_HANDLE en /dev/dri, mmap y SCM_RIGHTS. Mientras
 *   ese dmabuf viva, guarda una referencia al dma_buf.
 * - dma_buf_get(fd): un dma_buf de la capa, o uno que envuelve un dmabuf de
 *   Aurora (dumb buffer exportado por PRIME); el mismo fd da el mismo dma_buf.
 *
 * dma_resv es minimo (solo el cerrojo) hasta K7: no hay fences implicitos.
 */
#ifndef _LKPI_LINUX_DMA_BUF_H
#define _LKPI_LINUX_DMA_BUF_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/dma-direction.h>
#include <linux/dma-resv.h>
#include <linux/iosys-map.h>
#include <linux/scatterlist.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/dma-mapping.h>
#include <linux/dma-fence.h>
#include <linux/wait.h>

struct device;
struct dma_buf;
struct dma_buf_attachment;
struct module;

struct dma_buf_ops {
	int (*attach)(struct dma_buf *, struct dma_buf_attachment *);
	void (*detach)(struct dma_buf *, struct dma_buf_attachment *);
	int (*pin)(struct dma_buf_attachment *attach);
	void (*unpin)(struct dma_buf_attachment *attach);
	struct sg_table *(*map_dma_buf)(struct dma_buf_attachment *, enum dma_data_direction);
	void (*unmap_dma_buf)(struct dma_buf_attachment *, struct sg_table *,
			      enum dma_data_direction);
	void (*release)(struct dma_buf *);
	int (*begin_cpu_access)(struct dma_buf *, enum dma_data_direction);
	int (*end_cpu_access)(struct dma_buf *, enum dma_data_direction);
	int (*mmap)(struct dma_buf *, struct vm_area_struct *vma);
	int (*vmap)(struct dma_buf *dmabuf, struct iosys_map *map);
	void (*vunmap)(struct dma_buf *dmabuf, struct iosys_map *map);
};

struct dma_buf {
	size_t size;
	struct file *file;
	struct list_head attachments;
	const struct dma_buf_ops *ops;
	unsigned int vmapping_counter;
	struct iosys_map vmap_ptr;
	const char *exp_name;
	const char *name;
	spinlock_t name_lock;
	struct module *owner;
	struct list_head list_node;
	void *priv;
	struct dma_resv *resv;
	/* capa */
	struct dma_resv __lkpi_resv;	/* si el exportador no da la suya */
	struct mutex __lkpi_lock;
	void *__lkpi_anode;		/* dmabuf de Aurora de dma_buf_fd (o NULL) */
	struct dma_buf_attachment *__lkpi_cpu_attach;
	struct sg_table *__lkpi_cpu_sgt;
	void *__lkpi_import_node;	/* dmabuf de Aurora envuelto (dma_buf_get) */
};

struct dma_buf_attach_ops {
	bool allow_peer2peer;
	void (*move_notify)(struct dma_buf_attachment *attach);
};

struct dma_buf_attachment {
	struct dma_buf *dmabuf;
	struct device *dev;
	struct list_head node;
	struct sg_table *sgt;
	enum dma_data_direction dir;
	bool peer2peer;
	const struct dma_buf_attach_ops *importer_ops;
	void *importer_priv;
	void *priv;
};

struct dma_buf_export_info {
	const char *exp_name;
	struct module *owner;
	const struct dma_buf_ops *ops;
	size_t size;
	int flags;
	struct dma_resv *resv;
	void *priv;
};

#define DEFINE_DMA_BUF_EXPORT_INFO(name) \
	struct dma_buf_export_info name = { .exp_name = KBUILD_MODNAME, .owner = THIS_MODULE }

struct dma_buf *lkpi_dma_buf_export(const struct dma_buf_export_info *exp_info);
int lkpi_dma_buf_fd(struct dma_buf *dmabuf, int flags);
struct dma_buf *lkpi_dma_buf_get(int fd);
void lkpi_dma_buf_put(struct dma_buf *dmabuf);
struct dma_buf_attachment *lkpi_dma_buf_dynamic_attach(struct dma_buf *dmabuf,
						       struct device *dev,
						       const struct dma_buf_attach_ops *importer_ops,
						       void *importer_priv);
void lkpi_dma_buf_detach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach);
struct sg_table *lkpi_dma_buf_map_attachment(struct dma_buf_attachment *attach,
					     enum dma_data_direction dir);
void lkpi_dma_buf_unmap_attachment(struct dma_buf_attachment *attach, struct sg_table *sgt,
				   enum dma_data_direction dir);
struct sg_table *lkpi_dma_buf_map_attachment_unlocked(struct dma_buf_attachment *attach,
						      enum dma_data_direction dir);
void lkpi_dma_buf_unmap_attachment_unlocked(struct dma_buf_attachment *attach,
					    struct sg_table *sgt, enum dma_data_direction dir);
int lkpi_dma_buf_begin_cpu_access(struct dma_buf *dmabuf, enum dma_data_direction dir);
int lkpi_dma_buf_end_cpu_access(struct dma_buf *dmabuf, enum dma_data_direction dir);
int lkpi_dma_buf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma, unsigned long pgoff);
int lkpi_dma_buf_vmap(struct dma_buf *dmabuf, struct iosys_map *map);
void lkpi_dma_buf_vunmap(struct dma_buf *dmabuf, struct iosys_map *map);
int lkpi_dma_buf_pin(struct dma_buf_attachment *attach);
void lkpi_dma_buf_unpin(struct dma_buf_attachment *attach);

#define dma_buf_export(i) lkpi_dma_buf_export(i)
#define dma_buf_fd(b, f) lkpi_dma_buf_fd(b, f)
#define dma_buf_get(fd) lkpi_dma_buf_get(fd)
#define dma_buf_put(b) lkpi_dma_buf_put(b)
#define dma_buf_dynamic_attach(b, d, o, p) lkpi_dma_buf_dynamic_attach(b, d, o, p)
#define dma_buf_attach(b, d) lkpi_dma_buf_dynamic_attach(b, d, NULL, NULL)
#define dma_buf_detach(b, a) lkpi_dma_buf_detach(b, a)
#define dma_buf_map_attachment(a, d) lkpi_dma_buf_map_attachment(a, d)
#define dma_buf_unmap_attachment(a, s, d) lkpi_dma_buf_unmap_attachment(a, s, d)
#define dma_buf_map_attachment_unlocked(a, d) lkpi_dma_buf_map_attachment_unlocked(a, d)
#define dma_buf_unmap_attachment_unlocked(a, s, d) \
	lkpi_dma_buf_unmap_attachment_unlocked(a, s, d)
#define dma_buf_begin_cpu_access(b, d) lkpi_dma_buf_begin_cpu_access(b, d)
#define dma_buf_end_cpu_access(b, d) lkpi_dma_buf_end_cpu_access(b, d)
#define dma_buf_mmap(b, v, p) lkpi_dma_buf_mmap(b, v, p)
#define dma_buf_vmap(b, m) lkpi_dma_buf_vmap(b, m)
#define dma_buf_vunmap(b, m) lkpi_dma_buf_vunmap(b, m)
#define dma_buf_pin(a) lkpi_dma_buf_pin(a)
#define dma_buf_unpin(a) lkpi_dma_buf_unpin(a)
static inline int dma_buf_vmap_unlocked(struct dma_buf *dmabuf, struct iosys_map *map)
{
	int rc;

	dma_resv_lock(dmabuf->resv, NULL);
	rc = lkpi_dma_buf_vmap(dmabuf, map);
	dma_resv_unlock(dmabuf->resv);
	return rc;
}
static inline void dma_buf_vunmap_unlocked(struct dma_buf *dmabuf, struct iosys_map *map)
{
	dma_resv_lock(dmabuf->resv, NULL);
	lkpi_dma_buf_vunmap(dmabuf, map);
	dma_resv_unlock(dmabuf->resv);
}
static inline void get_dma_buf(struct dma_buf *dmabuf) { get_file(dmabuf->file); }
static inline bool dma_buf_is_dynamic(struct dma_buf *dmabuf) { return !!dmabuf->ops->pin; }
static inline bool dma_buf_attachment_is_dynamic(struct dma_buf_attachment *attach)
{
	return !!attach->importer_ops;
}
#define dma_buf_move_notify(b) do { (void)(b); } while (0)

#endif
