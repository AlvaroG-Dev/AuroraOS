// SPDX-License-Identifier: GPL-2.0-only
/*
 * dma_buf (K1-DMA, L4) enlazado con el PRIME de Aurora. Ver linux/dma-buf.h.
 *
 * Exportar: el dma_buf tiene un struct file de la capa (dmabuf->file). Al
 * pedir un fd, la capa mapea el dma_buf una vez para la CPU (attachment
 * interno sin IOMMU ni bounce) y crea un dmabuf de Aurora sobre esas paginas
 * (dmabuf_node_new_ext): el usuario lo puede pasar a /dev/dri
 * (PRIME_FD_TO_HANDLE), mapear o enviar por SCM_RIGHTS. Cada dmabuf de Aurora
 * guarda una referencia al dma_buf hasta que Aurora lo libera.
 *
 * Importar: dma_buf_get() de un fd de la capa o de un dmabuf de Aurora creado
 * por ella devuelve ese dma_buf; el de un dmabuf nativo de Aurora (un dumb
 * buffer exportado por PRIME) se envuelve en un dma_buf nuevo cuya sg_table
 * son las paginas del GEM. No se reutilizan envoltorios: dos dma_buf_get del
 * mismo fd nativo dan dos dma_buf con las mismas paginas.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/anon_inodes.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "../aurora.h"
#include "../lkpi_internal.h"
#include "../fs/fs.h"

/* lkpi_reservation_ww_class la define dma-resv.c de Linux (K7) */

/* "Dispositivo" de la CPU: attachment interno para sacar las paginas. */
static struct device lkpi_dmabuf_cpu_dev;
static u64 lkpi_dmabuf_cpu_mask = DMA_BIT_MASK(64);

static void lkpi_dma_buf_destroy(struct dma_buf *dmabuf);

/* ------------------------------------------------------- fichero */

static int lkpi_dma_buf_file_release(struct inode *inode, struct file *file)
{
	lkpi_dma_buf_destroy(file->private_data);
	return 0;
}

static int lkpi_dma_buf_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct dma_buf *dmabuf = file->private_data;

	if (!dmabuf->ops->mmap)
		return -EINVAL;
	if (vma->vm_pgoff + vma_pages(vma) > dmabuf->size >> PAGE_SHIFT)
		return -EINVAL;
	return dmabuf->ops->mmap(dmabuf, vma);
}

static loff_t lkpi_dma_buf_file_llseek(struct file *file, loff_t offset, int whence)
{
	struct dma_buf *dmabuf = file->private_data;

	if (offset != 0)
		return -EINVAL;
	if (whence == SEEK_END)
		return dmabuf->size;
	if (whence == SEEK_SET)
		return 0;
	return -EINVAL;
}

static __poll_t lkpi_dma_buf_file_poll(struct file *file, poll_table *pt)
{
	return EPOLLIN | EPOLLOUT;	/* sin fences hasta K7 */
}

static const struct file_operations lkpi_dma_buf_fops = {
	.release = lkpi_dma_buf_file_release,
	.mmap = lkpi_dma_buf_file_mmap,
	.llseek = lkpi_dma_buf_file_llseek,
	.poll = lkpi_dma_buf_file_poll,
};

/* --------------------------------------------------------- exportar */

struct dma_buf *lkpi_dma_buf_export(const struct dma_buf_export_info *exp_info)
{
	struct dma_buf *dmabuf;
	struct file *file;

	if (WARN_ON(!exp_info->priv || !exp_info->ops || !exp_info->ops->map_dma_buf ||
		    !exp_info->ops->unmap_dma_buf || !exp_info->ops->release || !exp_info->size))
		return ERR_PTR(-EINVAL);
	dmabuf = kzalloc(sizeof(*dmabuf), GFP_KERNEL);
	if (!dmabuf)
		return ERR_PTR(-ENOMEM);
	dmabuf->priv = exp_info->priv;
	dmabuf->ops = exp_info->ops;
	dmabuf->size = exp_info->size;
	dmabuf->exp_name = exp_info->exp_name;
	dmabuf->owner = exp_info->owner;
	spin_lock_init(&dmabuf->name_lock);
	INIT_LIST_HEAD(&dmabuf->attachments);
	INIT_LIST_HEAD(&dmabuf->list_node);
	mutex_init(&dmabuf->__lkpi_lock);
	if (exp_info->resv) {
		dmabuf->resv = exp_info->resv;
	} else {
		dma_resv_init(&dmabuf->__lkpi_resv);
		dmabuf->resv = &dmabuf->__lkpi_resv;
	}
	file = lkpi_anon_inode_getfile("dmabuf", &lkpi_dma_buf_fops, dmabuf,
				       exp_info->flags | O_RDWR);
	if (IS_ERR(file)) {
		kfree(dmabuf);
		return ERR_CAST(file);
	}
	file->f_inode->i_size = dmabuf->size;
	file->f_mode |= FMODE_LSEEK;
	dmabuf->file = file;
	return dmabuf;
}
EXPORT_SYMBOL(lkpi_dma_buf_export);

static void lkpi_dma_buf_destroy(struct dma_buf *dmabuf)
{
	if (dmabuf->__lkpi_cpu_sgt)
		lkpi_dma_buf_unmap_attachment_unlocked(dmabuf->__lkpi_cpu_attach,
						       dmabuf->__lkpi_cpu_sgt, DMA_BIDIRECTIONAL);
	if (dmabuf->__lkpi_cpu_attach)
		lkpi_dma_buf_detach(dmabuf, dmabuf->__lkpi_cpu_attach);
	WARN(!list_empty(&dmabuf->attachments), "dma_buf liberado con attachments\n");
	WARN(dmabuf->vmapping_counter, "dma_buf liberado con vmap\n");
	dmabuf->ops->release(dmabuf);
	if (dmabuf->resv == &dmabuf->__lkpi_resv)
		dma_resv_fini(&dmabuf->__lkpi_resv);
	kfree(dmabuf);
}

void lkpi_dma_buf_put(struct dma_buf *dmabuf)
{
	if (!WARN_ON(!dmabuf || !dmabuf->file))
		fput(dmabuf->file);
}
EXPORT_SYMBOL(lkpi_dma_buf_put);

/* Aurora suelta el ultimo GEM sobre nuestras paginas. */
static void lkpi_dmabuf_anode_release(void *priv)
{
	lkpi_dma_buf_put(priv);
}

/*
 * Pagina de relleno para dma_bufs sin paginas accesibles por la CPU (memoria
 * de video: nvidia-drm no puede dar sg_table de vidmem). El dma_buf sigue
 * siendo valido para el GPU (se identifica por dmabuf->priv al reimportarlo);
 * el dmabuf de Aurora que lo envuelve apunta toda su extension a esta pagina.
 */
static u64 lkpi_dma_buf_scratch_phys(void)
{
	static u64 phys;
	struct page *pg;

	if (phys)
		return phys;
	pg = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!pg)
		return 0;
	phys = page_to_phys(pg);
	return phys;
}

/* Paginas del dma_buf para la CPU (una vez). Con __lkpi_lock tomado. */
static int lkpi_dma_buf_cpu_pages(struct dma_buf *dmabuf, u64 **out, u32 *npages)
{
	struct sg_page_iter piter;
	u32 n = 0, want = PAGE_ALIGN(dmabuf->size) >> PAGE_SHIFT;
	u64 *pages;

	if (!dmabuf->__lkpi_cpu_sgt) {
		struct dma_buf_attachment *a;
		struct sg_table *sgt = NULL;
		int err = 0;

		a = lkpi_dma_buf_dynamic_attach(dmabuf, &lkpi_dmabuf_cpu_dev, NULL, NULL);
		if (IS_ERR(a)) {
			err = PTR_ERR(a);
			a = NULL;
		} else {
			sgt = lkpi_dma_buf_map_attachment_unlocked(a, DMA_BIDIRECTIONAL);
			if (IS_ERR(sgt)) {
				err = PTR_ERR(sgt);
				lkpi_dma_buf_detach(dmabuf, a);
				a = NULL;
			}
		}
		if (err) {
			u64 sp = lkpi_dma_buf_scratch_phys();
			u32 i;

			pr_info("linuxkpi: dma_buf '%s' sin paginas de CPU (%d): vidmem, pagina de relleno\n",
				dmabuf->exp_name ? dmabuf->exp_name : "?", err);
			if (!sp)
				return err;
			pages = kvmalloc_array(want, sizeof(u64), GFP_KERNEL);
			if (!pages)
				return -ENOMEM;
			for (i = 0; i < want; i++)
				pages[i] = sp;
			*out = pages;
			*npages = want;
			return 0;
		}
		dmabuf->__lkpi_cpu_attach = a;
		dmabuf->__lkpi_cpu_sgt = sgt;
	}
	pages = kvmalloc_array(want, sizeof(u64), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;
	for_each_sgtable_page(dmabuf->__lkpi_cpu_sgt, &piter, 0) {
		if (n == want)
			break;
		pages[n++] = page_to_phys(sg_page_iter_page(&piter));
	}
	if (n != want) {
		kvfree(pages);
		return -EINVAL;	/* la sg_table no cubre el tamano */
	}
	*out = pages;
	*npages = n;
	return 0;
}

/* ------------------------------------- fence implicito <-> Aurora (K7) */

#define LKPI_DMA_BUF_SYNC_WRITE 2

/* EXPORT_SYNC_FILE: el fence (singleton) del uso pedido, para el sync_file */
static struct aurora_fence *lkpi_dmabuf_resv_export(void *priv, u32 flags)
{
	struct dma_buf *dmabuf = priv;
	struct dma_fence *fence = NULL;
	struct aurora_fence *af;

	if (dma_resv_get_singleton(dmabuf->resv,
				   dma_resv_usage_rw(!!(flags & LKPI_DMA_BUF_SYNC_WRITE)), &fence))
		return NULL;
	af = lkpi_dma_fence_to_aurora(fence);
	dma_fence_put(fence);
	return af;
}

/* IMPORT_SYNC_FILE: el fence del sync_file pasa al dma_resv */
static int lkpi_dmabuf_resv_import(void *priv, u32 flags, struct aurora_fence *af)
{
	struct dma_buf *dmabuf = priv;
	struct dma_fence *fence = lkpi_dma_fence_from_aurora(af);
	int rc;

	if (!fence)
		return -ENOMEM;
	rc = dma_resv_lock_interruptible(dmabuf->resv, NULL);
	if (!rc) {
		rc = dma_resv_reserve_fences(dmabuf->resv, 1);
		if (!rc)
			dma_resv_add_fence(dmabuf->resv, fence,
					   (flags & LKPI_DMA_BUF_SYNC_WRITE) ?
					   DMA_RESV_USAGE_WRITE : DMA_RESV_USAGE_READ);
		dma_resv_unlock(dmabuf->resv);
	}
	dma_fence_put(fence);
	return rc;
}

/* poll: POLLIN espera a los escritores; POLLOUT, a lectores y escritores */
static int lkpi_dmabuf_resv_poll(void *priv, short events)
{
	struct dma_buf *dmabuf = priv;
	int ready = 0;

	if ((events & 1) && dma_resv_test_signaled(dmabuf->resv, DMA_RESV_USAGE_WRITE))
		ready |= 1;
	if ((events & 4) && dma_resv_test_signaled(dmabuf->resv, DMA_RESV_USAGE_READ))
		ready |= 4;
	return ready;
}

static const struct aurora_dmabuf_resv_ops lkpi_dmabuf_resv_ops = {
	.export_fence = lkpi_dmabuf_resv_export,
	.import_fence = lkpi_dmabuf_resv_import,
	.poll = lkpi_dmabuf_resv_poll,
};

/*
 * Un dma_buf que envuelve un dmabuf nativo de Aurora toma su fence implicito
 * al importarlo (no hay aviso posterior: se muestrea en dma_buf_get).
 */
static void lkpi_dma_buf_sample_native_fence(struct dma_buf *dmabuf)
{
	struct aurora_fence *af;
	struct dma_fence *fence;

	if (aurora_dmabuf_node_export_fence(dmabuf->__lkpi_import_node, 3, &af))
		return;
	if (!aurora_fence_signaled(af)) {
		fence = lkpi_dma_fence_from_aurora(af);
		if (fence) {
			dma_resv_lock(dmabuf->resv, NULL);
			if (!dma_resv_reserve_fences(dmabuf->resv, 1))
				dma_resv_add_fence(dmabuf->resv, fence, DMA_RESV_USAGE_WRITE);
			dma_resv_unlock(dmabuf->resv);
			dma_fence_put(fence);
		}
	}
	aurora_fence_put(af);
}

/* Instala en `fd` (ya reservado) un dmabuf de Aurora que envuelve `dmabuf`. */
static int lkpi_dma_buf_install(int fd, struct dma_buf *dmabuf)
{
	struct aurora_vfs_node *n;
	u64 *pages;
	u32 npages;
	int rc;

	mutex_lock(&dmabuf->__lkpi_lock);
	rc = lkpi_dma_buf_cpu_pages(dmabuf, &pages, &npages);
	mutex_unlock(&dmabuf->__lkpi_lock);
	if (rc)
		return rc;
	get_dma_buf(dmabuf);	/* la del dmabuf de Aurora */
	n = aurora_dmabuf_node_new_ext(npages, pages, lkpi_dmabuf_anode_release, dmabuf);
	kvfree(pages);
	if (!n) {
		lkpi_dma_buf_put(dmabuf);
		return -ENOMEM;
	}
	aurora_dmabuf_node_set_resv_ops(n, lkpi_dmabuf_anode_release, &lkpi_dmabuf_resv_ops);
	rc = aurora_vfs_fd_install_node(fd, n, O_RDWR);
	if (rc)
		aurora_vfs_node_free(n);
	return rc;
}

int lkpi_dma_buf_fd(struct dma_buf *dmabuf, int flags)
{
	int fd, rc;

	if (!dmabuf || !dmabuf->file)
		return -EINVAL;
	fd = aurora_vfs_fd_reserve(!!(flags & O_CLOEXEC));
	if (fd < 0)
		return fd;
	rc = lkpi_dma_buf_install(fd, dmabuf);
	if (rc) {
		aurora_vfs_fd_unreserve(fd);
		return rc;
	}
	return fd;
}
EXPORT_SYMBOL(lkpi_dma_buf_fd);

/*
 * fd_install(fd, dmabuf->file), como hace drm_prime: el fd del usuario ha de
 * ser el dmabuf de Aurora (ioctl, fences, SCM_RIGHTS al compositor), no el
 * fichero de la capa. Consume la referencia del llamante sobre el fichero.
 */
bool lkpi_dma_buf_fd_install(unsigned int fd, struct file *file)
{
	struct dma_buf *dmabuf;
	int rc;

	if (file->f_op != &lkpi_dma_buf_fops)
		return false;
	dmabuf = file->private_data;
	rc = lkpi_dma_buf_install((int)fd, dmabuf);
	if (rc)
		pr_warn("linuxkpi: fd_install de un dma_buf: %d\n", rc);
	lkpi_fput(file);
	return true;
}

/* ------------------------------------- importar un dmabuf de Aurora */

struct lkpi_aurora_dmabuf {
	struct aurora_vfs_node *node;	/* con referencia */
	u32 npages;
	const u64 *pages;		/* del GEM: estables mientras viva el nodo */
};

static struct sg_table *lkpi_adb_map(struct dma_buf_attachment *attach,
				     enum dma_data_direction dir)
{
	struct lkpi_aurora_dmabuf *a = attach->dmabuf->priv;
	struct page **pages;
	struct sg_table *sgt;
	u32 i;
	int rc;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	pages = kvmalloc_array(a->npages, sizeof(*pages), GFP_KERNEL);
	if (!sgt || !pages) {
		rc = -ENOMEM;
		goto err;
	}
	for (i = 0; i < a->npages; i++)
		pages[i] = pfn_to_page(a->pages[i] >> PAGE_SHIFT);
	rc = sg_alloc_table_from_pages(sgt, pages, a->npages, 0,
				       (unsigned long)a->npages << PAGE_SHIFT, GFP_KERNEL);
	if (rc)
		goto err;
	rc = dma_map_sgtable(attach->dev, sgt, dir, 0);
	if (rc) {
		sg_free_table(sgt);
		goto err;
	}
	kvfree(pages);
	return sgt;
err:
	kvfree(pages);
	kfree(sgt);
	return ERR_PTR(rc);
}

static void lkpi_adb_unmap(struct dma_buf_attachment *attach, struct sg_table *sgt,
			   enum dma_data_direction dir)
{
	dma_unmap_sgtable(attach->dev, sgt, dir, 0);
	sg_free_table(sgt);
	kfree(sgt);
}

static void lkpi_adb_release(struct dma_buf *dmabuf)
{
	struct lkpi_aurora_dmabuf *a = dmabuf->priv;

	aurora_vfs_node_free(a->node);
	kfree(a);
}

static int lkpi_adb_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct lkpi_aurora_dmabuf *a = dmabuf->priv;
	unsigned long i, n = vma_pages(vma);
	int rc;

	if (vma->vm_pgoff + n > a->npages)
		return -EINVAL;
	for (i = 0; i < n; i++) {
		rc = remap_pfn_range(vma, vma->vm_start + (i << PAGE_SHIFT),
				     a->pages[vma->vm_pgoff + i] >> PAGE_SHIFT, PAGE_SIZE,
				     vma->vm_page_prot);
		if (rc)
			return rc;
	}
	return 0;
}

static int lkpi_adb_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct lkpi_aurora_dmabuf *a = dmabuf->priv;
	unsigned long *pfns;
	void *va;
	u32 i;

	pfns = kvmalloc_array(a->npages, sizeof(*pfns), GFP_KERNEL);
	if (!pfns)
		return -ENOMEM;
	for (i = 0; i < a->npages; i++)
		pfns[i] = a->pages[i] >> PAGE_SHIFT;
	va = vmap_pfn(pfns, a->npages, PAGE_KERNEL);
	kvfree(pfns);
	if (!va)
		return -ENOMEM;
	iosys_map_set_vaddr(map, va);
	return 0;
}

static void lkpi_adb_vunmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	vunmap(map->vaddr);
}

static const struct dma_buf_ops lkpi_aurora_dmabuf_ops = {
	.map_dma_buf = lkpi_adb_map,
	.unmap_dma_buf = lkpi_adb_unmap,
	.release = lkpi_adb_release,
	.mmap = lkpi_adb_mmap,
	.vmap = lkpi_adb_vmap,
	.vunmap = lkpi_adb_vunmap,
};

struct dma_buf *lkpi_dma_buf_get(int fd)
{
	struct aurora_fd *afd = aurora_vfs_fd_get_current(fd);
	struct aurora_vfs_node *node;
	struct dma_buf *dmabuf;
	struct file *f;

	if (!afd)
		return ERR_PTR(-EBADF);
	node = aurora_vfs_fd_node(afd);
	f = lkpi_file_of_node(node);
	if (f && f->f_op == &lkpi_dma_buf_fops) {
		dmabuf = f->private_data;
		get_dma_buf(dmabuf);
	} else if ((dmabuf = aurora_dmabuf_node_ext_priv(node, lkpi_dmabuf_anode_release))) {
		get_dma_buf(dmabuf);
	} else {
		DEFINE_DMA_BUF_EXPORT_INFO(info);
		struct lkpi_aurora_dmabuf *a;
		const u64 *pages;
		u32 npages;

		if (aurora_dmabuf_node_pages(node, &npages, &pages)) {
			aurora_vfs_fd_put_ref(afd);
			return ERR_PTR(-EINVAL);
		}
		a = kzalloc(sizeof(*a), GFP_KERNEL);
		if (!a) {
			aurora_vfs_fd_put_ref(afd);
			return ERR_PTR(-ENOMEM);
		}
		aurora_vfs_node_ref(node);
		a->node = node;
		a->npages = npages;
		a->pages = pages;
		info.exp_name = "aurora-prime";
		info.ops = &lkpi_aurora_dmabuf_ops;
		info.size = (size_t)npages << PAGE_SHIFT;
		info.flags = O_RDWR;
		info.priv = a;
		dmabuf = lkpi_dma_buf_export(&info);
		if (IS_ERR(dmabuf)) {
			aurora_vfs_node_free(node);
			kfree(a);
		} else {
			dmabuf->__lkpi_import_node = node;
			lkpi_dma_buf_sample_native_fence(dmabuf);
		}
	}
	aurora_vfs_fd_put_ref(afd);
	return dmabuf;
}
EXPORT_SYMBOL(lkpi_dma_buf_get);

/* ------------------------------------------------------ attachments */

struct dma_buf_attachment *lkpi_dma_buf_dynamic_attach(struct dma_buf *dmabuf,
						       struct device *dev,
						       const struct dma_buf_attach_ops *importer_ops,
						       void *importer_priv)
{
	struct dma_buf_attachment *attach;
	int rc;

	if (WARN_ON(!dmabuf || !dev))
		return ERR_PTR(-EINVAL);
	attach = kzalloc(sizeof(*attach), GFP_KERNEL);
	if (!attach)
		return ERR_PTR(-ENOMEM);
	attach->dev = dev;
	attach->dmabuf = dmabuf;
	attach->importer_ops = importer_ops;
	attach->importer_priv = importer_priv;
	attach->peer2peer = importer_ops && importer_ops->allow_peer2peer;
	if (dmabuf->ops->attach) {
		rc = dmabuf->ops->attach(dmabuf, attach);
		if (rc) {
			kfree(attach);
			return ERR_PTR(rc);
		}
	}
	dma_resv_lock(dmabuf->resv, NULL);
	list_add(&attach->node, &dmabuf->attachments);
	dma_resv_unlock(dmabuf->resv);
	return attach;
}
EXPORT_SYMBOL(lkpi_dma_buf_dynamic_attach);

void lkpi_dma_buf_detach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach)
{
	if (WARN_ON(!dmabuf || !attach || dmabuf != attach->dmabuf))
		return;
	dma_resv_lock(dmabuf->resv, NULL);
	list_del(&attach->node);
	dma_resv_unlock(dmabuf->resv);
	if (dmabuf->ops->detach)
		dmabuf->ops->detach(dmabuf, attach);
	kfree(attach);
}
EXPORT_SYMBOL(lkpi_dma_buf_detach);

int lkpi_dma_buf_pin(struct dma_buf_attachment *attach)
{
	struct dma_buf *dmabuf = attach->dmabuf;

	return dmabuf->ops->pin ? dmabuf->ops->pin(attach) : 0;
}
EXPORT_SYMBOL(lkpi_dma_buf_pin);

void lkpi_dma_buf_unpin(struct dma_buf_attachment *attach)
{
	struct dma_buf *dmabuf = attach->dmabuf;

	if (dmabuf->ops->unpin)
		dmabuf->ops->unpin(attach);
}
EXPORT_SYMBOL(lkpi_dma_buf_unpin);

/* Con dmabuf->resv tomado (como Linux). */
struct sg_table *lkpi_dma_buf_map_attachment(struct dma_buf_attachment *attach,
					     enum dma_data_direction dir)
{
	struct sg_table *sgt;

	if (WARN_ON(!attach || !attach->dmabuf))
		return ERR_PTR(-EINVAL);
	sgt = attach->dmabuf->ops->map_dma_buf(attach, dir);
	if (!sgt)
		sgt = ERR_PTR(-ENOMEM);
	return sgt;
}
EXPORT_SYMBOL(lkpi_dma_buf_map_attachment);

void lkpi_dma_buf_unmap_attachment(struct dma_buf_attachment *attach, struct sg_table *sgt,
				   enum dma_data_direction dir)
{
	if (WARN_ON(!attach || !attach->dmabuf || !sgt))
		return;
	attach->dmabuf->ops->unmap_dma_buf(attach, sgt, dir);
}
EXPORT_SYMBOL(lkpi_dma_buf_unmap_attachment);

struct sg_table *lkpi_dma_buf_map_attachment_unlocked(struct dma_buf_attachment *attach,
						      enum dma_data_direction dir)
{
	struct sg_table *sgt;

	dma_resv_lock(attach->dmabuf->resv, NULL);
	sgt = lkpi_dma_buf_map_attachment(attach, dir);
	dma_resv_unlock(attach->dmabuf->resv);
	return sgt;
}
EXPORT_SYMBOL(lkpi_dma_buf_map_attachment_unlocked);

void lkpi_dma_buf_unmap_attachment_unlocked(struct dma_buf_attachment *attach,
					    struct sg_table *sgt, enum dma_data_direction dir)
{
	dma_resv_lock(attach->dmabuf->resv, NULL);
	lkpi_dma_buf_unmap_attachment(attach, sgt, dir);
	dma_resv_unlock(attach->dmabuf->resv);
}
EXPORT_SYMBOL(lkpi_dma_buf_unmap_attachment_unlocked);

/* ------------------------------------------------ CPU, mmap y vmap */

int lkpi_dma_buf_begin_cpu_access(struct dma_buf *dmabuf, enum dma_data_direction dir)
{
	return dmabuf->ops->begin_cpu_access ? dmabuf->ops->begin_cpu_access(dmabuf, dir) : 0;
}
EXPORT_SYMBOL(lkpi_dma_buf_begin_cpu_access);

int lkpi_dma_buf_end_cpu_access(struct dma_buf *dmabuf, enum dma_data_direction dir)
{
	return dmabuf->ops->end_cpu_access ? dmabuf->ops->end_cpu_access(dmabuf, dir) : 0;
}
EXPORT_SYMBOL(lkpi_dma_buf_end_cpu_access);

int lkpi_dma_buf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma, unsigned long pgoff)
{
	if (!dmabuf || !vma || !dmabuf->ops->mmap)
		return -EINVAL;
	if (pgoff + vma_pages(vma) < pgoff ||
	    pgoff + vma_pages(vma) > dmabuf->size >> PAGE_SHIFT)
		return -EINVAL;
	vma->vm_pgoff = pgoff;
	return dmabuf->ops->mmap(dmabuf, vma);
}
EXPORT_SYMBOL(lkpi_dma_buf_mmap);

/* Con dmabuf->resv tomado. Un solo mapeo compartido por todos los usuarios. */
int lkpi_dma_buf_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct iosys_map ptr;
	int rc;

	iosys_map_clear(map);
	if (WARN_ON(!dmabuf) || !dmabuf->ops->vmap)
		return -EINVAL;
	if (dmabuf->vmapping_counter) {
		dmabuf->vmapping_counter++;
		*map = dmabuf->vmap_ptr;
		return 0;
	}
	rc = dmabuf->ops->vmap(dmabuf, &ptr);
	if (rc)
		return rc;
	dmabuf->vmap_ptr = ptr;
	dmabuf->vmapping_counter = 1;
	*map = ptr;
	return 0;
}
EXPORT_SYMBOL(lkpi_dma_buf_vmap);

void lkpi_dma_buf_vunmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	if (WARN_ON(!dmabuf || !dmabuf->vmapping_counter) ||
	    WARN_ON(!iosys_map_is_equal(&dmabuf->vmap_ptr, map)))
		return;
	if (--dmabuf->vmapping_counter == 0) {
		if (dmabuf->ops->vunmap)
			dmabuf->ops->vunmap(dmabuf, map);
		iosys_map_clear(&dmabuf->vmap_ptr);
	}
}
EXPORT_SYMBOL(lkpi_dma_buf_vunmap);

static void lkpi_dmabuf_cpu_dev_release(struct device *dev)
{
}

int lkpi_dmabuf_init(void)
{
	lkpi_dmabuf_cpu_dev.release = lkpi_dmabuf_cpu_dev_release;
	device_initialize(&lkpi_dmabuf_cpu_dev);
	dev_set_name(&lkpi_dmabuf_cpu_dev, "lkpi-dmabuf-cpu");
	lkpi_dmabuf_cpu_dev.dma_mask = &lkpi_dmabuf_cpu_mask;
	lkpi_dmabuf_cpu_dev.coherent_dma_mask = DMA_BIT_MASK(64);
	return 0;
}

void lkpi_dmabuf_exit(void)
{
	put_device(&lkpi_dmabuf_cpu_dev);
}
