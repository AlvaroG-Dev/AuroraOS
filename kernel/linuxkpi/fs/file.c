// SPDX-License-Identifier: GPL-2.0-only
/*
 * Puente struct file / file_operations <-> VFS de Aurora (K8, L4).
 *
 * Cada struct file es un nodo del VFS de Aurora con lkpi_file_vops y el
 * fichero en node->priv. Los nodos de /dev y de debugfs empiezan con un
 * lkpi_vnode_src en priv: ->open crea el fichero (y llama a fops->open) y lo
 * pone en su lugar. Las referencias del fichero son las del nodo; cuando
 * Aurora suelta la ultima, ->close hace el release de Linux.
 *
 * Aurora llama a read_fd/write_fd/llseek con el fd (buffer de usuario y su
 * offset), a setfl en F_SETFL (O_NONBLOCK, O_ASYNC: fasync), a ioctl/poll
 * por nodo y a mmap_dev con un VMA_DEV ya creado (mm/mmap.c). Los
 * -ERESTART* de las esperas interrumpidas salen como -EINTR.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/anon_inodes.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/hardirq.h>
#include <linux/cdev.h>

#include "../aurora.h"
#include "../lkpi_internal.h"
#include "fs.h"

static inline struct aurora_wait_queue *lkpi_file_wq(struct file *f)
{
	return (struct aurora_wait_queue *)f->__lkpi_wq;
}

int lkpi_errno_user(long rc)
{
	switch (rc) {
	case -ERESTARTSYS:
	case -ERESTARTNOINTR:
	case -ERESTARTNOHAND:
	case -ERESTART_RESTARTBLOCK:
		return -EINTR;
	case -ENOIOCTLCMD:
		return -ENOTTY;
	}
	return (int)rc;
}
EXPORT_SYMBOL(lkpi_errno_user);

static fmode_t lkpi_fmode(unsigned int flags)
{
	fmode_t m = FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE;

	switch (flags & O_ACCMODE) {
	case O_RDONLY:
		m |= FMODE_READ;
		break;
	case O_WRONLY:
		m |= FMODE_WRITE;
		break;
	default:
		m |= FMODE_READ | FMODE_WRITE;
	}
	return m;
}

void lkpi_address_space_init(struct address_space *mapping, struct inode *host)
{
	mapping->host = host;
	mutex_init(&mapping->i_mmap_mutex);
	INIT_LIST_HEAD(&mapping->i_mmap);
	mapping->nrpages = 0;
	mapping->gfp_mask = GFP_HIGHUSER;
	mapping->flags = 0;
	mapping->__lkpi_pages = NULL;
	mapping->__lkpi_cap = 0;
	mutex_init(&mapping->__lkpi_pages_lock);
}
EXPORT_SYMBOL(lkpi_address_space_init);

static void lkpi_inode_init(struct inode *inode)
{
	memset(inode, 0, sizeof(*inode));
	atomic_set(&inode->i_count, 1);
	spin_lock_init(&inode->i_lock);
	init_rwsem(&inode->i_rwsem);
	lkpi_address_space_init(&inode->i_data, inode);
	inode->i_mapping = &inode->i_data;
}

struct inode *lkpi_alloc_anon_inode(struct super_block *sb)
{
	static atomic_t ino = ATOMIC_INIT(0x10000);
	struct inode *inode = kmalloc(sizeof(*inode), GFP_KERNEL);

	if (!inode)
		return ERR_PTR(-ENOMEM);
	lkpi_inode_init(inode);
	inode->i_sb = sb;
	inode->i_ino = atomic_inc_return(&ino);
	inode->i_mode = 0600;
	return inode;
}
EXPORT_SYMBOL(lkpi_alloc_anon_inode);

void lkpi_iput(struct inode *inode)
{
	if (inode && atomic_dec_and_test(&inode->i_count))
		kfree(inode);
}
EXPORT_SYMBOL(lkpi_iput);

struct file *lkpi_file_alloc(const struct file_operations *fops, unsigned int flags)
{
	struct file *f = kzalloc(sizeof(*f), GFP_KERNEL);

	if (!f)
		return NULL;
	f->f_op = fops;
	f->f_flags = flags & ~O_CLOEXEC;
	f->f_mode = lkpi_fmode(flags);	/* FMODE_LSEEK: lo ajusta quien abre */
	mutex_init(&f->f_pos_lock);
	spin_lock_init(&f->f_lock);
	INIT_LIST_HEAD(&f->__lkpi_polls);
	spin_lock_init(&f->__lkpi_poll_lock);
	aurora_wait_queue_init(lkpi_file_wq(f));
	lkpi_inode_init(&f->__lkpi_inode);
	f->f_inode = &f->__lkpi_inode;
	f->f_mapping = f->f_inode->i_mapping;
	return f;
}

void lkpi_file_free(struct file *f)
{
	kfree(f);
}

int lkpi_file_attach_node(struct file *f, const char *name, u32 vflags, u32 mode, u64 size)
{
	struct aurora_vfs_node *n;

	n = aurora_vfs_node_alloc(name, vflags, mode, 0, &lkpi_file_vops, f, size);
	if (!n)
		return -ENOMEM;
	f->__lkpi_node = n;
	return 0;
}

struct file *lkpi_file_of_node(struct aurora_vfs_node *n)
{
	struct file *f;

	if (!n || aurora_vfs_node_ops(n) != &lkpi_file_vops)
		return NULL;
	f = aurora_vfs_node_get_priv(n);
	if (!f || ((struct lkpi_vnode_src *)f)->magic == LKPI_VNODE_MAGIC)
		return NULL;
	return f;
}

struct file *lkpi_get_file(struct file *f)
{
	aurora_vfs_node_ref(f->__lkpi_node);
	return f;
}
EXPORT_SYMBOL(lkpi_get_file);

void lkpi_fput(struct file *f)
{
	if (f)
		aurora_vfs_node_free(f->__lkpi_node);
}
EXPORT_SYMBOL(lkpi_fput);

/* ------------------------------------------------------------ poll */

struct lkpi_poll_entry {
	struct list_head node;
	wait_queue_head_t *wqh;
	wait_queue_entry_t wait;
	struct file *file;
};

static int lkpi_poll_wake(wait_queue_entry_t *wait, unsigned int mode, int sync, void *key)
{
	struct lkpi_poll_entry *pe = container_of(wait, struct lkpi_poll_entry, wait);
	struct aurora_wait_queue *wq = lkpi_file_wq(pe->file);
	unsigned long flags;

	flags = aurora_spin_lock_irqsave(&wq->lock);
	aurora_wake_up_all_locked(wq);	/* tambien avisa a epoll */
	aurora_spin_unlock_irqrestore(&wq->lock, flags);
	return 0;
}

/* Cuelga una entrada de cada cola del driver la primera vez que se ve. */
static void lkpi_poll_qproc(struct file *file, wait_queue_head_t *wqh, poll_table *pt)
{
	struct lkpi_poll_entry *pe;
	unsigned long flags;

	spin_lock_irqsave(&file->__lkpi_poll_lock, flags);
	list_for_each_entry(pe, &file->__lkpi_polls, node)
		if (pe->wqh == wqh) {
			spin_unlock_irqrestore(&file->__lkpi_poll_lock, flags);
			return;
		}
	spin_unlock_irqrestore(&file->__lkpi_poll_lock, flags);
	pe = kzalloc(sizeof(*pe), GFP_KERNEL);
	if (!pe)
		return;
	pe->wqh = wqh;
	pe->file = file;
	init_waitqueue_func_entry(&pe->wait, lkpi_poll_wake);
	add_wait_queue(wqh, &pe->wait);
	spin_lock_irqsave(&file->__lkpi_poll_lock, flags);
	list_add(&pe->node, &file->__lkpi_polls);
	spin_unlock_irqrestore(&file->__lkpi_poll_lock, flags);
}

static void lkpi_poll_release(struct file *file)
{
	struct lkpi_poll_entry *pe, *n;

	list_for_each_entry_safe(pe, n, &file->__lkpi_polls, node) {
		remove_wait_queue(pe->wqh, &pe->wait);
		list_del(&pe->node);
		kfree(pe);
	}
}

/* ------------------------------------------------- operaciones de Aurora */

static struct file *lkpi_vnode_file(struct aurora_vfs_node *n)
{
	struct file *f = aurora_vfs_node_get_priv(n);

	if (!f || ((struct lkpi_vnode_src *)f)->magic == LKPI_VNODE_MAGIC)
		return NULL;
	return f;
}

static struct file *lkpi_fd_file(struct aurora_fd *afd)
{
	struct file *f = lkpi_vnode_file(aurora_vfs_fd_node(afd));

	if (f) {
		/* O_NONBLOCK/FASYNC vienen del fd (F_SETFL); el resto, de open. */
		unsigned int fl = aurora_vfs_fd_flags(afd);
		f->f_flags = (f->f_flags & ~(O_NONBLOCK | FASYNC)) | (fl & (O_NONBLOCK | FASYNC));
		WRITE_ONCE(f->__lkpi_owner, aurora_vfs_fd_owner(afd));
	}
	return f;
}

static int lkpi_vop_open(struct aurora_vfs_node *n, int flags)
{
	struct lkpi_vnode_src *src = aurora_vfs_node_get_priv(n);
	struct file *f;

	if (!src || src->magic != LKPI_VNODE_MAGIC)
		return 0;	/* ya es un fichero (no deberia pasar) */
	f = src->open(src, flags);
	if (IS_ERR(f))
		return lkpi_errno_user(PTR_ERR(f));
	f->__lkpi_node = n;
	aurora_vfs_node_set_priv(n, f);
	return 0;
}

static int lkpi_vop_close(struct aurora_vfs_node *n)
{
	struct file *f = lkpi_vnode_file(n);

	if (!f)
		return 0;
	aurora_vfs_node_set_priv(n, NULL);
	lkpi_poll_release(f);
	if ((f->f_flags & FASYNC) && f->f_op && f->f_op->fasync)
		f->f_op->fasync(-1, f, 0);
	if (f->f_op && f->f_op->release)
		f->f_op->release(f->f_inode, f);
	if (f->f_inode != &f->__lkpi_inode)
		lkpi_iput(f->f_inode);
	else if (f->f_inode->i_cdev)
		kobject_put(&f->f_inode->i_cdev->kobj);
	lkpi_file_free(f);
	return 0;
}

static s64 lkpi_vop_read_fd(struct aurora_fd *afd, void *ubuf, size_t count, u64 *pos)
{
	struct file *f = lkpi_fd_file(afd);
	loff_t p;
	ssize_t r;

	if (!f)
		return -EBADF;
	if (!(f->f_mode & FMODE_READ))
		return -EBADF;
	if (!f->f_op->read)
		return -EINVAL;
	p = (loff_t)*pos;
	f->f_pos = p;
	r = f->f_op->read(f, ubuf, count, &p);
	if (r >= 0)
		*pos = (u64)p;
	return r < 0 ? lkpi_errno_user(r) : r;
}

static s64 lkpi_vop_write_fd(struct aurora_fd *afd, const void *ubuf, size_t count, u64 *pos)
{
	struct file *f = lkpi_fd_file(afd);
	loff_t p;
	ssize_t r;

	if (!f)
		return -EBADF;
	if (!(f->f_mode & FMODE_WRITE))
		return -EBADF;
	if (!f->f_op->write)
		return -EINVAL;
	p = (loff_t)*pos;
	f->f_pos = p;
	r = f->f_op->write(f, ubuf, count, &p);
	if (r >= 0)
		*pos = (u64)p;
	return r < 0 ? lkpi_errno_user(r) : r;
}

static s64 lkpi_vop_llseek(struct aurora_fd *afd, s64 off, int whence, u64 cur)
{
	struct file *f = lkpi_fd_file(afd);
	loff_t r;

	if (!f)
		return -EBADF;
	if (!(f->f_mode & FMODE_LSEEK) || !f->f_op->llseek)
		return -ESPIPE;
	f->f_pos = (loff_t)cur;
	r = f->f_op->llseek(f, off, whence);
	return r < 0 ? lkpi_errno_user(r) : r;
}

static int lkpi_vop_setfl(struct aurora_fd *afd, int newflags)
{
	struct file *f = lkpi_fd_file(afd);
	unsigned int nf = (unsigned int)newflags;
	int rc;

	if (!f)
		return -EBADF;
	if ((nf ^ f->f_flags) & FASYNC) {
		if (!f->f_op->fasync)
			return -EINVAL;	/* como Linux: sin fasync no hay O_ASYNC */
		rc = f->f_op->fasync(-1, f, !!(nf & FASYNC));
		if (rc < 0)
			return lkpi_errno_user(rc);
	}
	f->f_flags = (f->f_flags & ~(O_NONBLOCK | FASYNC)) | (nf & (O_NONBLOCK | FASYNC));
	return 0;
}


static s64 lkpi_vop_ioctl(struct aurora_vfs_node *n, unsigned long req, u64 arg)
{
	struct file *f = lkpi_vnode_file(n);
	long r;

	if (!f)
		return -EBADF;
	if (!f->f_op->unlocked_ioctl)
		return -ENOTTY;
	r = f->f_op->unlocked_ioctl(f, (unsigned int)req, (unsigned long)arg);
	if (r >= 0 && ((req >> 8) & 0xff) == 'F' && ((req >> 16) & 0x3fff) >= 0x20) {
		/*
		 * Diagnostico NVIDIA: el ioctl de nvidiactl devuelve 0 aunque el RM rechace
		 * la peticion; el error va en el campo `status` de NVOS54 (RM_CONTROL 0x2a:
		 * cmd en +8, status en +0x1c) o NVOS64 (RM_ALLOC 0x2b: clase en +0xc,
		 * status en +0x28). Maximo 300 lineas (todas las llamadas, no solo las fallidas).
		 */
		static int lkpi_rm_fail_logged;
		unsigned long nr = req & 0xff;
		if (lkpi_rm_fail_logged < 300 && (nr == 0x2a || nr == 0x2b)) {
			u32 w[12] = {0};
			unsigned long sz = (req >> 16) & 0x3fff;
			if (sz > sizeof(w))
				sz = sizeof(w);
			if (!copy_from_user(w, (void __user *)arg, sz)) {
				u32 st = nr == 0x2a ? w[7] : w[10];
				{
					/* se registran todas las llamadas (tambien status=0) mientras no se sepa cual falla */
					lkpi_rm_fail_logged++;
					pr_info("lkpi: RM %s status=0x%x %s=0x%x hClient=0x%x hObj=0x%x\n",
						nr == 0x2a ? "CONTROL" : "ALLOC", st,
						nr == 0x2a ? "cmd" : "class", nr == 0x2a ? w[2] : w[3],
						w[0], w[1]);
				}
			}
		}
	}
	if (((req >> 8) & 0xff) == 'F' && (req & 0xff) != 0x2a && (req & 0xff) != 0x2b) {
		/* Diagnostico NVIDIA: resto de ioctl de nvidiactl/nvidia0 (REGISTER_FD, NUMA_INFO...). */
		static int lkpi_nv_ioctl_logged;
		if (lkpi_nv_ioctl_logged < 200) {
			lkpi_nv_ioctl_logged++;
			pr_info("lkpi: nv ioctl nr=0x%lx size=0x%lx ret=%ld\n", req & 0xff, (req >> 16) & 0x3fff, r);
		}
	}
	if (r < 0) {
		/* Diagnostico: ioctl fallidos (libs de NVIDIA, nvidia-smi...). Maximo 128 lineas. */
		static int lkpi_ioctl_fail_logged;
		if (lkpi_ioctl_fail_logged < 128) {
			lkpi_ioctl_fail_logged++;
			pr_info("lkpi: ioctl fallido cmd=0x%lx (dir=%lu tipo='%c' nr=0x%lx size=%lu) ret=%ld\n",
				req, (req >> 30) & 3, (char)((req >> 8) & 0xff), req & 0xff,
				(req >> 16) & 0x3fff, r);
		}
	}
	return r < 0 ? lkpi_errno_user(r) : r;
}

static int lkpi_vop_poll(struct aurora_vfs_node *n, short events)
{
	struct file *f = lkpi_vnode_file(n);
	poll_table pt;

	if (!f)
		return POLLNVAL;
	if (!f->f_op->poll)
		return DEFAULT_POLLMASK;
	init_poll_funcptr(&pt, lkpi_poll_qproc);
	pt._key = (__poll_t)(unsigned short)events | EPOLLERR | EPOLLHUP;
	/* Como Linux: solo lo pedido, mas los errores (Aurora no filtra). */
	return (int)(f->f_op->poll(f, &pt) & pt._key & 0xffff);
}

static struct aurora_wait_queue *lkpi_vop_poll_wq(struct aurora_vfs_node *n)
{
	struct file *f = lkpi_vnode_file(n);

	return f ? lkpi_file_wq(f) : NULL;
}

static int lkpi_vop_mmap_dev(struct aurora_fd *afd, struct aurora_process *proc,
			     struct aurora_vma *vma, u64 prot, u64 flags)
{
	struct file *f = lkpi_fd_file(afd);

	if (!f)
		return -EBADF;
	return lkpi_file_mmap(f, proc, vma, prot, flags);
}

const struct aurora_vfs_ops lkpi_file_vops = {
	.open = lkpi_vop_open,
	.close = lkpi_vop_close,
	.poll = lkpi_vop_poll,
	.poll_wq = lkpi_vop_poll_wq,
	.ioctl = lkpi_vop_ioctl,
	.read_fd = lkpi_vop_read_fd,
	.write_fd = lkpi_vop_write_fd,
	.llseek = lkpi_vop_llseek,
	.setfl = lkpi_vop_setfl,
	.mmap_dev = lkpi_vop_mmap_dev,
};

/* -------------------------------------------------------------- fds */

struct fd lkpi_fdget(unsigned int fd)
{
	struct aurora_fd *afd = aurora_vfs_fd_get_current((int)fd);
	struct fd r = { NULL, NULL };
	struct file *f;

	if (!afd)
		return r;
	f = lkpi_file_of_node(aurora_vfs_fd_node(afd));
	if (!f) {
		aurora_vfs_fd_put_ref(afd);
		return r;
	}
	lkpi_fd_file(afd);
	r.file = f;
	r.__lkpi_afd = afd;
	return r;
}
EXPORT_SYMBOL(lkpi_fdget);

void lkpi_fdput(struct fd f)
{
	if (f.__lkpi_afd)
		aurora_vfs_fd_put_ref(f.__lkpi_afd);
}
EXPORT_SYMBOL(lkpi_fdput);

struct file *lkpi_fget(unsigned int fd)
{
	struct fd f = lkpi_fdget(fd);
	struct file *file = f.file;

	if (file)
		lkpi_get_file(file);
	lkpi_fdput(f);
	return file;
}
EXPORT_SYMBOL(lkpi_fget);

int lkpi_get_unused_fd_flags(unsigned int flags)
{
	return aurora_vfs_fd_reserve(!!(flags & O_CLOEXEC));
}
EXPORT_SYMBOL(lkpi_get_unused_fd_flags);

void lkpi_put_unused_fd(unsigned int fd)
{
	aurora_vfs_fd_unreserve((int)fd);
}
EXPORT_SYMBOL(lkpi_put_unused_fd);

void lkpi_fd_install(unsigned int fd, struct file *file)
{
	/* Consume la referencia del llamante; si el hueco ya no esta, se suelta. */
	if (lkpi_dma_buf_fd_install(fd, file))
		return;
	if (aurora_vfs_fd_install_node((int)fd, file->__lkpi_node, (int)file->f_flags))
		lkpi_fput(file);
}
EXPORT_SYMBOL(lkpi_fd_install);

struct file *lkpi_anon_inode_getfile(const char *name, const struct file_operations *fops,
				     void *priv, int flags)
{
	struct file *f = lkpi_file_alloc(fops, (unsigned int)flags);
	int rc;

	if (!f)
		return ERR_PTR(-ENOMEM);
	f->private_data = priv;
	f->f_inode->i_mode = AURORA_S_IFREG | 0600;
	if (!fops || !fops->llseek)
		f->f_mode &= ~FMODE_LSEEK;
	rc = lkpi_file_attach_node(f, name, AURORA_VFS_FILE, AURORA_S_IFREG | 0600, 0);
	if (rc) {
		lkpi_file_free(f);
		return ERR_PTR(rc);
	}
	return f;
}
EXPORT_SYMBOL(lkpi_anon_inode_getfile);

int lkpi_anon_inode_getfd(const char *name, const struct file_operations *fops, void *priv,
			  int flags)
{
	struct file *f;
	int fd = lkpi_get_unused_fd_flags(flags);

	if (fd < 0)
		return fd;
	f = lkpi_anon_inode_getfile(name, fops, priv, flags);
	if (IS_ERR(f)) {
		lkpi_put_unused_fd(fd);
		return PTR_ERR(f);
	}
	lkpi_fd_install(fd, f);
	return fd;
}
EXPORT_SYMBOL(lkpi_anon_inode_getfd);

/* ----------------------------------------------------------- fasync */

static DEFINE_SPINLOCK(lkpi_fasync_lock);

int lkpi_fasync_helper(int fd, struct file *filp, int on, struct fasync_struct **fapp)
{
	struct fasync_struct *fa, **fp, *nfa = NULL;
	unsigned long flags;

	if (on) {
		nfa = kzalloc(sizeof(*nfa), GFP_KERNEL);
		if (!nfa)
			return -ENOMEM;
	}
	spin_lock_irqsave(&lkpi_fasync_lock, flags);
	for (fp = fapp; (fa = *fp) != NULL; fp = &fa->fa_next) {
		if (fa->fa_file != filp)
			continue;
		if (on) {
			fa->fa_fd = fd;
			spin_unlock_irqrestore(&lkpi_fasync_lock, flags);
			kfree(nfa);
			return 0;
		}
		*fp = fa->fa_next;
		spin_unlock_irqrestore(&lkpi_fasync_lock, flags);
		kfree(fa);
		return 1;
	}
	if (on) {
		nfa->magic = FASYNC_MAGIC;
		nfa->fa_file = filp;
		nfa->fa_fd = fd;
		nfa->fa_next = *fapp;
		*fapp = nfa;
	}
	spin_unlock_irqrestore(&lkpi_fasync_lock, flags);
	return on ? 1 : 0;
}
EXPORT_SYMBOL(lkpi_fasync_helper);

/* La senal se manda desde contexto de tarea (toma cerrojos de procesos). */
struct lkpi_sigio_work {
	struct work_struct work;
	struct file *file;
	int sig;
};

static void lkpi_sigio_send(struct file *f, int sig)
{
	int owner = READ_ONCE(f->__lkpi_owner);

	if (owner && (f->f_flags & FASYNC))
		aurora_vfs_send_sigio(owner, sig);
}

static void lkpi_sigio_workfn(struct work_struct *w)
{
	struct lkpi_sigio_work *sw = container_of(w, struct lkpi_sigio_work, work);

	lkpi_sigio_send(sw->file, sw->sig);
	lkpi_fput(sw->file);
	kfree(sw);
}

void lkpi_kill_fasync(struct fasync_struct **fp, int sig, int band)
{
	struct fasync_struct *fa;
	unsigned long flags;

	(void)band;
	/* Puede llegar desde una IRQ: la senal sale de un work. */
	spin_lock_irqsave(&lkpi_fasync_lock, flags);
	for (fa = *fp; fa; fa = fa->fa_next) {
		struct lkpi_sigio_work *sw = kmalloc(sizeof(*sw), GFP_ATOMIC);

		if (!sw)
			continue;
		INIT_WORK(&sw->work, lkpi_sigio_workfn);
		sw->file = lkpi_get_file(fa->fa_file);
		sw->sig = sig;
		schedule_work(&sw->work);
	}
	spin_unlock_irqrestore(&lkpi_fasync_lock, flags);
}
EXPORT_SYMBOL(lkpi_kill_fasync);

/* ------------------------------------------------- llseek y simple_* */

loff_t lkpi_vfs_setpos(struct file *file, loff_t offset, loff_t maxsize)
{
	if (offset < 0 && !(file->f_mode & FMODE_UNSIGNED_OFFSET))
		return -EINVAL;
	if (offset > maxsize)
		return -EINVAL;
	file->f_pos = offset;
	return offset;
}
EXPORT_SYMBOL(lkpi_vfs_setpos);

loff_t lkpi_generic_file_llseek_size(struct file *file, loff_t offset, int whence,
				     loff_t maxsize, loff_t eof)
{
	switch (whence) {
	case SEEK_END:
		offset += eof;
		break;
	case SEEK_CUR:
		if (offset == 0)
			return file->f_pos;
		offset += file->f_pos;
		break;
	case SEEK_DATA:
		if (offset >= eof)
			return -ENXIO;
		break;
	case SEEK_HOLE:
		if (offset >= eof)
			return -ENXIO;
		offset = eof;
		break;
	case SEEK_SET:
		break;
	default:
		return -EINVAL;
	}
	return lkpi_vfs_setpos(file, offset, maxsize);
}
EXPORT_SYMBOL(lkpi_generic_file_llseek_size);

loff_t lkpi_default_llseek(struct file *file, loff_t offset, int whence)
{
	return lkpi_generic_file_llseek_size(file, offset, whence, MAX_LFS_FILESIZE,
					     i_size_read(file_inode(file)));
}
EXPORT_SYMBOL(lkpi_default_llseek);

loff_t lkpi_noop_llseek(struct file *file, loff_t offset, int whence)
{
	return file->f_pos;
}
EXPORT_SYMBOL(lkpi_noop_llseek);

loff_t lkpi_fixed_size_llseek(struct file *file, loff_t offset, int whence, loff_t size)
{
	return lkpi_generic_file_llseek_size(file, offset, whence, size, size);
}
EXPORT_SYMBOL(lkpi_fixed_size_llseek);

loff_t lkpi_no_seek_end_llseek(struct file *file, loff_t offset, int whence)
{
	if (whence != SEEK_SET && whence != SEEK_CUR)
		return -EINVAL;
	return lkpi_generic_file_llseek_size(file, offset, whence, OFFSET_MAX, 0);
}
EXPORT_SYMBOL(lkpi_no_seek_end_llseek);

int lkpi_nonseekable_open(struct inode *inode, struct file *filp)
{
	filp->f_mode &= ~(FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE);
	return 0;
}
EXPORT_SYMBOL(lkpi_nonseekable_open);

int lkpi_stream_open(struct inode *inode, struct file *filp)
{
	filp->f_mode &= ~(FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE | FMODE_ATOMIC_POS);
	filp->f_mode |= FMODE_STREAM;
	return 0;
}
EXPORT_SYMBOL(lkpi_stream_open);

ssize_t lkpi_simple_read_from_buffer(void __user *to, size_t count, loff_t *ppos,
				     const void *from, size_t available)
{
	loff_t pos = *ppos;
	size_t ret;

	if (pos < 0)
		return -EINVAL;
	if (pos >= (loff_t)available || !count)
		return 0;
	if (count > available - pos)
		count = available - pos;
	ret = copy_to_user(to, from + pos, count);
	if (ret == count)
		return -EFAULT;
	count -= ret;
	*ppos = pos + count;
	return count;
}
EXPORT_SYMBOL(lkpi_simple_read_from_buffer);

ssize_t lkpi_simple_write_to_buffer(void *to, size_t available, loff_t *ppos,
				    const void __user *from, size_t count)
{
	loff_t pos = *ppos;
	size_t res;

	if (pos < 0)
		return -EINVAL;
	if (pos >= (loff_t)available || !count)
		return 0;
	if (count > available - pos)
		count = available - pos;
	res = copy_from_user(to + pos, from, count);
	if (res == count)
		return -EFAULT;
	count -= res;
	*ppos = pos + count;
	return count;
}
EXPORT_SYMBOL(lkpi_simple_write_to_buffer);

ssize_t lkpi_memory_read_from_buffer(void *to, size_t count, loff_t *ppos, const void *from,
				     size_t available)
{
	loff_t pos = *ppos;

	if (pos < 0)
		return -EINVAL;
	if (pos >= (loff_t)available)
		return 0;
	if (count > available - pos)
		count = available - pos;
	memcpy(to, from + pos, count);
	*ppos = pos + count;
	return count;
}
EXPORT_SYMBOL(lkpi_memory_read_from_buffer);
