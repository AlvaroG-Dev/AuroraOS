// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver de prueba del hito de L4: /dev/lkpi_test (miscdevice), escrito
 * contra la API de Linux de la capa como un driver normal. Lo carga y lo
 * ejercita user/musl/apps/lkpi_test (interfaz en lkpi_test_uapi.h):
 *
 *  - read/write/llseek sobre 4 KiB de memoria (fixed_size_llseek);
 *  - ioctl: identidad de current, firmware, eventos con temporizador, espera
 *    interrumpible, dma_buf (exportar a un fd e importar uno de /dev/dri);
 *  - mmap: offset 0 = memoria coherente (dma_mmap_coherent); offset 1 MiB =
 *    paginas por fallo (vm_operations_struct con fault/open/close);
 *    LKPI_IOC_ZAP las quita (unmap_mapping_range) y se vuelven a pedir;
 *  - poll/epoll y fasync (SIGIO) con los eventos del temporizador;
 *  - debugfs: /sys/kernel/debug/lkpi_test/{events,status}.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/crc32.h>
#include <linux/debugfs.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include "lkpi_test_uapi.h"

struct lkpi_test_file {
	u32 seen;			/* eventos ya vistos por este fichero */
};

static struct {
	struct mutex lock;
	u8 mem[LKPI_TEST_MEM_SIZE];
	atomic_t events;
	u32 events_dbg;			/* copia para debugfs */
	wait_queue_head_t wq;
	struct fasync_struct *fasync;
	struct timer_list timer;
	void *dma;
	dma_addr_t dma_handle;
	struct page *fault_pages[LKPI_TEST_FAULT_PAGES];
	atomic_t opens, releases, vma_opens, vma_closes, faults;
	struct dentry *dbg;
	u64 dma_mask;
} lt;

static struct miscdevice lt_misc;

/* ------------------------------------------------------------ fichero */

static int lt_open(struct inode *inode, struct file *file)
{
	struct lkpi_test_file *f = kzalloc(sizeof(*f), GFP_KERNEL);

	if (!f)
		return -ENOMEM;
	f->seen = atomic_read(&lt.events);
	file->private_data = f;
	atomic_inc(&lt.opens);
	return 0;
}

static int lt_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	atomic_inc(&lt.releases);
	return 0;
}

static ssize_t lt_read(struct file *file, char __user *buf, size_t n, loff_t *ppos)
{
	ssize_t r;

	mutex_lock(&lt.lock);
	r = simple_read_from_buffer(buf, n, ppos, lt.mem, sizeof(lt.mem));
	mutex_unlock(&lt.lock);
	return r;
}

static ssize_t lt_write(struct file *file, const char __user *buf, size_t n, loff_t *ppos)
{
	ssize_t r;

	mutex_lock(&lt.lock);
	r = simple_write_to_buffer(lt.mem, sizeof(lt.mem), ppos, buf, n);
	mutex_unlock(&lt.lock);
	return r;
}

static loff_t lt_llseek(struct file *file, loff_t off, int whence)
{
	return fixed_size_llseek(file, off, whence, LKPI_TEST_MEM_SIZE);
}

static __poll_t lt_poll(struct file *file, poll_table *pt)
{
	struct lkpi_test_file *f = file->private_data;
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;

	poll_wait(file, &lt.wq, pt);
	if ((u32)atomic_read(&lt.events) != READ_ONCE(f->seen))
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

static int lt_fasync(int fd, struct file *file, int on)
{
	return fasync_helper(fd, file, on, &lt.fasync);
}

static void lt_timer_fn(struct timer_list *t)
{
	lt.events_dbg = atomic_inc_return(&lt.events);
	wake_up_interruptible(&lt.wq);
	kill_fasync(&lt.fasync, SIGIO, POLL_IN);
}

/* --------------------------------------------------------------- mmap */

static void lt_vma_open(struct vm_area_struct *vma)
{
	atomic_inc(&lt.vma_opens);
}

static void lt_vma_close(struct vm_area_struct *vma)
{
	atomic_inc(&lt.vma_closes);
}

static vm_fault_t lt_vma_fault(struct vm_fault *vmf)
{
	pgoff_t i = vmf->pgoff - LKPI_TEST_FAULT_PGOFF;

	if (i >= LKPI_TEST_FAULT_PAGES)
		return VM_FAULT_SIGBUS;
	atomic_inc(&lt.faults);
	get_page(lt.fault_pages[i]);
	vmf->page = lt.fault_pages[i];
	return 0;
}

static const struct vm_operations_struct lt_vm_ops = {
	.open = lt_vma_open,
	.close = lt_vma_close,
	.fault = lt_vma_fault,
};

static int lt_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (vma->vm_pgoff == 0)
		return dma_mmap_coherent(lt_misc.this_device, vma, lt.dma, lt.dma_handle,
					 LKPI_TEST_DMA_SIZE);
	if (vma->vm_pgoff == LKPI_TEST_FAULT_PGOFF) {
		if (vma_pages(vma) > LKPI_TEST_FAULT_PAGES)
			return -EINVAL;
		vma->vm_ops = &lt_vm_ops;
		vm_flags_set(vma, VM_DONTEXPAND);
		atomic_inc(&lt.vma_opens);	/* Linux no llama a ->open en el mmap */
		return 0;
	}
	return -EINVAL;
}

/* ------------------------------------------------------------ dma_buf */

struct lt_exp {
	struct page *pages;
	unsigned int npages;
};

static struct sg_table *lt_exp_map(struct dma_buf_attachment *a, enum dma_data_direction dir)
{
	struct lt_exp *x = a->dmabuf->priv;
	struct page *pages[LKPI_TEST_DMABUF_PAGES];
	struct sg_table *sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	unsigned int i;

	if (!sgt)
		return ERR_PTR(-ENOMEM);
	for (i = 0; i < x->npages; i++)
		pages[i] = x->pages + i;
	if (sg_alloc_table_from_pages(sgt, pages, x->npages, 0, x->npages * PAGE_SIZE,
				      GFP_KERNEL) ||
	    dma_map_sgtable(a->dev, sgt, dir, 0)) {
		kfree(sgt);
		return ERR_PTR(-ENOMEM);
	}
	return sgt;
}

static void lt_exp_unmap(struct dma_buf_attachment *a, struct sg_table *sgt,
			 enum dma_data_direction dir)
{
	dma_unmap_sgtable(a->dev, sgt, dir, 0);
	sg_free_table(sgt);
	kfree(sgt);
}

static void lt_exp_release(struct dma_buf *db)
{
	struct lt_exp *x = db->priv;

	__free_pages(x->pages, get_order(x->npages * PAGE_SIZE));
	kfree(x);
}

static int lt_exp_mmap(struct dma_buf *db, struct vm_area_struct *vma)
{
	struct lt_exp *x = db->priv;

	return remap_pfn_range(vma, vma->vm_start, page_to_pfn(x->pages) + vma->vm_pgoff,
			       vma->vm_end - vma->vm_start, vma->vm_page_prot);
}

static int lt_exp_vmap(struct dma_buf *db, struct iosys_map *map)
{
	iosys_map_set_vaddr(map, page_address(((struct lt_exp *)db->priv)->pages));
	return 0;
}

static const struct dma_buf_ops lt_exp_ops = {
	.map_dma_buf = lt_exp_map,
	.unmap_dma_buf = lt_exp_unmap,
	.release = lt_exp_release,
	.mmap = lt_exp_mmap,
	.vmap = lt_exp_vmap,
};

static int lt_dmabuf_export(struct lkpi_test_dmabuf *a)
{
	DEFINE_DMA_BUF_EXPORT_INFO(info);
	struct dma_buf *db;
	struct lt_exp *x;
	unsigned int i;
	u8 *p;
	int fd;

	x = kzalloc(sizeof(*x), GFP_KERNEL);
	if (!x)
		return -ENOMEM;
	x->npages = LKPI_TEST_DMABUF_PAGES;
	x->pages = alloc_pages(GFP_KERNEL, get_order(x->npages * PAGE_SIZE));
	if (!x->pages) {
		kfree(x);
		return -ENOMEM;
	}
	p = page_address(x->pages);
	for (i = 0; i < x->npages * PAGE_SIZE; i++)
		p[i] = LKPI_TEST_PATTERN(a->seed, i);
	info.ops = &lt_exp_ops;
	info.size = x->npages * PAGE_SIZE;
	info.flags = O_RDWR;
	info.priv = x;
	db = dma_buf_export(&info);
	if (IS_ERR(db)) {
		lt_exp_release(&(struct dma_buf){ .priv = x });
		return PTR_ERR(db);
	}
	fd = dma_buf_fd(db, O_CLOEXEC);
	dma_buf_put(db);	/* la referencia la guarda el fd */
	if (fd < 0)
		return fd;
	a->fd = fd;
	a->size = info.size;
	return 0;
}

static int lt_dmabuf_import(struct lkpi_test_dmabuf *a)
{
	struct dma_buf_attachment *att;
	struct dma_buf *db = dma_buf_get(a->fd);
	struct sg_table *sgt;
	struct iosys_map map;
	size_t i;
	int rc;

	if (IS_ERR(db))
		return PTR_ERR(db);
	att = dma_buf_attach(db, lt_misc.this_device);
	if (IS_ERR(att)) {
		rc = PTR_ERR(att);
		goto put;
	}
	sgt = dma_buf_map_attachment_unlocked(att, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		rc = PTR_ERR(sgt);
		goto detach;
	}
	a->nents = sgt->nents;
	dma_buf_unmap_attachment_unlocked(att, sgt, DMA_BIDIRECTIONAL);
	rc = dma_buf_vmap_unlocked(db, &map);
	if (rc)
		goto detach;
	a->size = db->size;
	a->seed = ((u8 *)map.vaddr)[0];
	a->sum = 0;
	for (i = 0; i < db->size; i++)
		a->sum += ((u8 *)map.vaddr)[i];
	dma_buf_vunmap_unlocked(db, &map);
detach:
	dma_buf_detach(db, att);
put:
	dma_buf_put(db);
	return rc;
}

/* -------------------------------------------------------------- ioctl */

static long lt_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct lkpi_test_file *f = file->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case LKPI_IOC_INFO: {
		struct lkpi_test_info in = {};
		struct task_struct *me = current;

		in.pid = me->pid;
		in.tgid = me->tgid;
		in.uid = __kuid_val(current_uid());
		in.has_mm = me->mm != NULL;
		strscpy(in.comm, me->comm, sizeof(in.comm));
		in.opens = atomic_read(&lt.opens);
		in.releases = atomic_read(&lt.releases);
		in.vma_opens = atomic_read(&lt.vma_opens);
		in.vma_closes = atomic_read(&lt.vma_closes);
		in.faults = atomic_read(&lt.faults);
		in.events = atomic_read(&lt.events);
		in.dma_handle = lt.dma_handle;
		return copy_to_user(uarg, &in, sizeof(in)) ? -EFAULT : 0;
	}
	case LKPI_IOC_FIRMWARE: {
		struct lkpi_test_fw fw;
		const struct firmware *blob;

		if (copy_from_user(&fw, uarg, sizeof(fw)))
			return -EFAULT;
		fw.name[sizeof(fw.name) - 1] = '\0';
		fw.result = request_firmware(&blob, fw.name, lt_misc.this_device);
		fw.size = fw.crc32 = 0;
		if (!fw.result) {
			fw.size = blob->size;
			fw.crc32 = ~crc32_le(~0U, blob->data, blob->size);
			release_firmware(blob);
		}
		return copy_to_user(uarg, &fw, sizeof(fw)) ? -EFAULT : 0;
	}
	case LKPI_IOC_RAISE_LATER: {
		u32 ms;

		if (get_user(ms, (u32 __user *)uarg))
			return -EFAULT;
		mod_timer(&lt.timer, jiffies + msecs_to_jiffies(ms));
		return 0;
	}
	case LKPI_IOC_WAIT: {
		struct lkpi_test_wait w = {};
		int rc;

		/* Interrumpible: una senal la corta con -ERESTARTSYS (EINTR fuera). */
		rc = wait_event_interruptible(lt.wq, (u32)atomic_read(&lt.events) != f->seen);
		if (rc)
			return rc;
		w.events = f->seen = atomic_read(&lt.events);
		return copy_to_user(uarg, &w, sizeof(w)) ? -EFAULT : 0;
	}
	case LKPI_IOC_DMABUF_EXPORT:
	case LKPI_IOC_DMABUF_IMPORT: {
		struct lkpi_test_dmabuf d;
		int rc;

		if (copy_from_user(&d, uarg, sizeof(d)))
			return -EFAULT;
		rc = cmd == LKPI_IOC_DMABUF_EXPORT ? lt_dmabuf_export(&d) : lt_dmabuf_import(&d);
		if (rc)
			return rc;
		return copy_to_user(uarg, &d, sizeof(d)) ? -EFAULT : 0;
	}
	case LKPI_IOC_ZAP:
		unmap_mapping_range(file->f_mapping, 0, 0, 1);
		return 0;
	}
	return -ENOTTY;
}

static const struct file_operations lt_fops = {
	.owner = THIS_MODULE,
	.open = lt_open,
	.release = lt_release,
	.read = lt_read,
	.write = lt_write,
	.llseek = lt_llseek,
	.poll = lt_poll,
	.fasync = lt_fasync,
	.mmap = lt_mmap,
	.unlocked_ioctl = lt_ioctl,
};

static struct miscdevice lt_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "lkpi_test",
	.fops = &lt_fops,
	.mode = 0666,
};

/* ------------------------------------------------------------ debugfs */

static int lt_status_show(struct seq_file *s, void *unused)
{
	seq_printf(s, "opens=%d releases=%d faults=%d events=%d\n", atomic_read(&lt.opens),
		   atomic_read(&lt.releases), atomic_read(&lt.faults), atomic_read(&lt.events));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lt_status);

/* --------------------------------------------------------- init/exit */

static int __init lt_init(void)
{
	unsigned int i, j;
	int rc;

	mutex_init(&lt.lock);
	init_waitqueue_head(&lt.wq);
	timer_setup(&lt.timer, lt_timer_fn, 0);
	for (i = 0; i < LKPI_TEST_MEM_SIZE; i++)
		lt.mem[i] = LKPI_TEST_PATTERN(0x11, i);
	for (i = 0; i < LKPI_TEST_FAULT_PAGES; i++) {
		u8 *p;

		lt.fault_pages[i] = alloc_page(GFP_KERNEL);
		if (!lt.fault_pages[i]) {
			rc = -ENOMEM;
			goto err_pages;
		}
		p = page_address(lt.fault_pages[i]);
		for (j = 0; j < PAGE_SIZE; j++)
			p[j] = LKPI_TEST_PATTERN(0x40 + i, j);
	}
	rc = misc_register(&lt_misc);
	if (rc)
		goto err_pages;
	lt.dma_mask = DMA_BIT_MASK(64);
	lt_misc.this_device->dma_mask = &lt.dma_mask;
	lt_misc.this_device->coherent_dma_mask = DMA_BIT_MASK(32);
	lt.dma = dma_alloc_coherent(lt_misc.this_device, LKPI_TEST_DMA_SIZE, &lt.dma_handle,
				    GFP_KERNEL);
	if (!lt.dma) {
		rc = -ENOMEM;
		goto err_misc;
	}
	for (i = 0; i < LKPI_TEST_DMA_SIZE; i++)
		((u8 *)lt.dma)[i] = LKPI_TEST_PATTERN(0x5A, i);
	lt.dbg = debugfs_create_dir("lkpi_test", NULL);
	if (!IS_ERR(lt.dbg)) {
		debugfs_create_u32("events", 0444, lt.dbg, &lt.events_dbg);
		debugfs_create_file("status", 0444, lt.dbg, NULL, &lt_status_fops);
	}
	pr_info("lkpi_test: /dev/lkpi_test listo (misc %d:%d)\n", MISC_MAJOR, lt_misc.minor);
	return 0;

err_misc:
	misc_deregister(&lt_misc);
err_pages:
	for (i = 0; i < LKPI_TEST_FAULT_PAGES; i++)
		if (lt.fault_pages[i])
			__free_page(lt.fault_pages[i]);
	return rc;
}

static void __exit lt_exit(void)
{
	unsigned int i;

	debugfs_remove(lt.dbg);
	timer_delete_sync(&lt.timer);
	misc_deregister(&lt_misc);
	dma_free_coherent(NULL, LKPI_TEST_DMA_SIZE, lt.dma, lt.dma_handle);
	for (i = 0; i < LKPI_TEST_FAULT_PAGES; i++)
		__free_page(lt.fault_pages[i]);
}

module_init(lt_init);
module_exit(lt_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Aurora LinuxKPI: driver de prueba del hito de L4");
