/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Ficheros de los drivers (K8, L4): struct file, struct inode,
 * file_operations, numeros de dispositivo de caracteres, fasync y las
 * ayudas simple_* y *_llseek.
 *
 * Un struct file es un nodo del VFS de Aurora (vfs_ops de la capa: ver
 * kernel/linuxkpi/fs/file.c). Sus referencias son las del nodo: get_file()
 * toma una, fput() la suelta y la ultima llama a ->release. Cada apertura de
 * /dev/<x> crea un struct file y un inode propios (i_rdev, i_cdev).
 */
#ifndef _LKPI_LINUX_FS_H
#define _LKPI_LINUX_FS_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/kdev_t.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/atomic.h>
#include <linux/uidgid.h>
#include <linux/err.h>
#include <linux/fcntl.h>
#include <linux/ioctl.h>
#include <linux/mm_types.h>
#include <linux/rcupdate.h>
#include <linux/signal.h>

struct file;
struct inode;
struct poll_table_struct;
struct vm_area_struct;
struct module;
struct cdev;
struct seq_file;
struct kiocb;
struct iov_iter;
struct dir_context;
struct file_lock;
struct super_block;
struct vfsmount;
struct qstr {
	const unsigned char *name;
	unsigned int len;
};
/*
 * Vista publica de dentry: solo el nombre. La definicion completa es privada
 * de fs/debugfs.c (LKPI_DENTRY_PRIVATE) y empieza por el mismo campo.
 */
#ifndef LKPI_DENTRY_PRIVATE
struct dentry {
	struct qstr d_name;
};
#else
struct dentry;
#endif
struct path {
	struct vfsmount *mnt;
	struct dentry *dentry;
};

typedef unsigned int fmode_t;
#define FMODE_READ ((__force fmode_t)(1 << 0))
#define FMODE_WRITE ((__force fmode_t)(1 << 1))
#define FMODE_LSEEK ((__force fmode_t)(1 << 2))
#define FMODE_PREAD ((__force fmode_t)(1 << 3))
#define FMODE_PWRITE ((__force fmode_t)(1 << 4))
#define FMODE_EXEC ((__force fmode_t)(1 << 5))
#define FMODE_ATOMIC_POS ((__force fmode_t)(1 << 15))
#define FMODE_STREAM ((__force fmode_t)(1 << 21))
#define FMODE_NOWAIT ((__force fmode_t)(1 << 27))
#define FMODE_UNSIGNED_OFFSET ((__force fmode_t)(1 << 13))

#define MAX_LFS_FILESIZE ((loff_t)LLONG_MAX)
#define OFFSET_MAX ((loff_t)LLONG_MAX)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define SEEK_DATA 3
#define SEEK_HOLE 4
#define SEEK_MAX SEEK_HOLE

/* Paginas mapeadas de un fichero: los vm_area_struct vivos que lo mapean. */
struct address_space {
	const void *a_ops;
	struct inode *host;
	struct mutex i_mmap_mutex;
	struct list_head i_mmap;	/* vm_area_struct.__lkpi_mapping */
	unsigned long nrpages;
	void *private_data;
	gfp_t gfp_mask;
	unsigned long flags;		/* AS_* */
	struct page **__lkpi_pages;	/* shmem: paginas por indice (NULL = hueco) */
	unsigned long __lkpi_cap;
	struct mutex __lkpi_pages_lock;
};

struct inode {
	umode_t i_mode;
	unsigned int i_flags;
	kuid_t i_uid;
	kgid_t i_gid;
	unsigned long i_ino;
	dev_t i_rdev;
	loff_t i_size;
	atomic_t i_count;
	spinlock_t i_lock;
	struct rw_semaphore i_rwsem;
	struct super_block *i_sb;
	const struct file_operations *i_fop;
	struct address_space *i_mapping;
	struct address_space i_data;
	struct cdev *i_cdev;
	void *i_private;
};

struct fasync_struct {
	rwlock_t fa_lock;
	int magic;
	int fa_fd;
	struct fasync_struct *fa_next;
	struct file *fa_file;
	struct rcu_head fa_rcu;
};
#define FASYNC_MAGIC 0x4601

struct file {
	const struct file_operations *f_op;
	struct inode *f_inode;
	struct address_space *f_mapping;
	struct path f_path;
	unsigned int f_flags;
	fmode_t f_mode;
	loff_t f_pos;
	struct mutex f_pos_lock;
	spinlock_t f_lock;
	void *private_data;
	/* capa */
	void *__lkpi_node;		/* vfs_node_t de Aurora */
	int __lkpi_owner;		/* F_SETOWN del fd (SIGIO de kill_fasync) */
	u64 __lkpi_wq[4];		/* wait_queue_t de Aurora para poll/epoll */
	struct list_head __lkpi_polls;	/* entradas colgadas de las colas del driver */
	spinlock_t __lkpi_poll_lock;
	bool __lkpi_released;
	struct inode __lkpi_inode;	/* inode propio (si no es de debugfs/anon) */
};

struct file_operations {
	struct module *owner;
	unsigned int fop_flags;
	loff_t (*llseek)(struct file *, loff_t, int);
	ssize_t (*read)(struct file *, char __user *, size_t, loff_t *);
	ssize_t (*write)(struct file *, const char __user *, size_t, loff_t *);
	ssize_t (*read_iter)(struct kiocb *, struct iov_iter *);
	ssize_t (*write_iter)(struct kiocb *, struct iov_iter *);
	int (*iterate_shared)(struct file *, struct dir_context *);
	__poll_t (*poll)(struct file *, struct poll_table_struct *);
	long (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
	long (*compat_ioctl)(struct file *, unsigned int, unsigned long);
	int (*mmap)(struct file *, struct vm_area_struct *);
	int (*open)(struct inode *, struct file *);
	int (*flush)(struct file *, void *id);
	int (*release)(struct inode *, struct file *);
	int (*fsync)(struct file *, loff_t, loff_t, int datasync);
	int (*fasync)(int, struct file *, int);
	int (*lock)(struct file *, int, struct file_lock *);
	unsigned long (*get_unmapped_area)(struct file *, unsigned long, unsigned long,
					   unsigned long, unsigned long);
	int (*check_flags)(int);
	void (*show_fdinfo)(struct seq_file *m, struct file *f);
	unsigned long mmap_supported_flags;
};
#define FOP_BUFFER_RASYNC ((__force unsigned int)(1 << 0))
#define FOP_BUFFER_WASYNC ((__force unsigned int)(1 << 1))
#define FOP_MMAP_SYNC ((__force unsigned int)(1 << 2))
#define FOP_DIO_PARALLEL_WRITE ((__force unsigned int)(1 << 3))
#define FOP_HUGE_PAGES ((__force unsigned int)(1 << 4))
#define FOP_UNSIGNED_OFFSET ((__force unsigned int)(1 << 5))

static inline struct inode *file_inode(const struct file *f) { return f->f_inode; }
static inline unsigned int iminor(const struct inode *inode) { return MINOR(inode->i_rdev); }
static inline unsigned int imajor(const struct inode *inode) { return MAJOR(inode->i_rdev); }
static inline loff_t i_size_read(const struct inode *inode) { return READ_ONCE(inode->i_size); }
static inline void i_size_write(struct inode *inode, loff_t i_size) { WRITE_ONCE(inode->i_size, i_size); }
static inline void inode_lock(struct inode *inode) { down_write(&inode->i_rwsem); }
static inline void inode_unlock(struct inode *inode) { up_write(&inode->i_rwsem); }
static inline void inode_lock_shared(struct inode *inode) { down_read(&inode->i_rwsem); }
static inline void inode_unlock_shared(struct inode *inode) { up_read(&inode->i_rwsem); }
#define file_count(f) 1L

/* Referencias del fichero = referencias del nodo de Aurora. */
struct file *lkpi_get_file(struct file *f);
void lkpi_fput(struct file *f);
static inline struct file *get_file(struct file *f) { return lkpi_get_file(f); }
#define fput(f) lkpi_fput(f)
static inline struct file *get_file_rcu(struct file **f) { return lkpi_get_file(*f); }

/* llseek */
loff_t lkpi_vfs_setpos(struct file *file, loff_t offset, loff_t maxsize);
loff_t lkpi_generic_file_llseek_size(struct file *file, loff_t offset, int whence,
				     loff_t maxsize, loff_t eof);
loff_t lkpi_default_llseek(struct file *file, loff_t offset, int whence);
loff_t lkpi_noop_llseek(struct file *file, loff_t offset, int whence);
loff_t lkpi_fixed_size_llseek(struct file *file, loff_t offset, int whence, loff_t size);
loff_t lkpi_no_seek_end_llseek(struct file *file, loff_t offset, int whence);
#define vfs_setpos(f, o, m) lkpi_vfs_setpos(f, o, m)
#define generic_file_llseek_size(f, o, w, m, e) lkpi_generic_file_llseek_size(f, o, w, m, e)
#define generic_file_llseek lkpi_default_llseek
#define default_llseek lkpi_default_llseek
#define noop_llseek lkpi_noop_llseek
#define fixed_size_llseek(f, o, w, s) lkpi_fixed_size_llseek(f, o, w, s)
#define no_seek_end_llseek lkpi_no_seek_end_llseek
int lkpi_nonseekable_open(struct inode *inode, struct file *filp);
int lkpi_stream_open(struct inode *inode, struct file *filp);
#define nonseekable_open(i, f) lkpi_nonseekable_open(i, f)
#define stream_open(i, f) lkpi_stream_open(i, f)
static inline int simple_open(struct inode *inode, struct file *file)
{
	if (inode->i_private)
		file->private_data = inode->i_private;
	return 0;
}
ssize_t lkpi_simple_read_from_buffer(void __user *to, size_t count, loff_t *ppos,
				     const void *from, size_t available);
ssize_t lkpi_simple_write_to_buffer(void *to, size_t available, loff_t *ppos,
				    const void __user *from, size_t count);
ssize_t lkpi_memory_read_from_buffer(void *to, size_t count, loff_t *ppos,
				     const void *from, size_t available);
#define simple_read_from_buffer(t, c, p, f, a) lkpi_simple_read_from_buffer(t, c, p, f, a)
#define simple_write_to_buffer(t, a, p, f, c) lkpi_simple_write_to_buffer(t, a, p, f, c)
#define memory_read_from_buffer(t, c, p, f, a) lkpi_memory_read_from_buffer(t, c, p, f, a)
#define compat_ptr_ioctl NULL

/* Numeros de dispositivo de caracteres. */
#define CHRDEV_MAJOR_MAX 512
#define CHRDEV_MAJOR_DYN_END 234
int lkpi_alloc_chrdev_region(dev_t *dev, unsigned int baseminor, unsigned int count,
			     const char *name);
int lkpi_register_chrdev_region(dev_t from, unsigned int count, const char *name);
void lkpi_unregister_chrdev_region(dev_t from, unsigned int count);
int lkpi___register_chrdev(unsigned int major, unsigned int baseminor, unsigned int count,
			   const char *name, const struct file_operations *fops);
void lkpi___unregister_chrdev(unsigned int major, unsigned int baseminor, unsigned int count,
			      const char *name);
#define alloc_chrdev_region(d, b, c, n) lkpi_alloc_chrdev_region(d, b, c, n)
#define register_chrdev_region(f, c, n) lkpi_register_chrdev_region(f, c, n)
#define unregister_chrdev_region(f, c) lkpi_unregister_chrdev_region(f, c)
#define __register_chrdev(ma, bm, c, n, f) lkpi___register_chrdev(ma, bm, c, n, f)
#define __unregister_chrdev(ma, bm, c, n) lkpi___unregister_chrdev(ma, bm, c, n)
static inline int register_chrdev(unsigned int major, const char *name,
				  const struct file_operations *fops)
{
	return lkpi___register_chrdev(major, 0, 256, name, fops);
}
static inline void unregister_chrdev(unsigned int major, const char *name)
{
	lkpi___unregister_chrdev(major, 0, 256, name);
}

/* fasync: SIGIO al dueno (F_SETOWN) de cada fichero con O_ASYNC. */
int lkpi_fasync_helper(int fd, struct file *filp, int on, struct fasync_struct **fapp);
void lkpi_kill_fasync(struct fasync_struct **fp, int sig, int band);
#define fasync_helper(fd, f, on, fa) lkpi_fasync_helper(fd, f, on, fa)
#define kill_fasync(fp, sig, band) lkpi_kill_fasync(fp, sig, band)
#define POLL_IN 1
#define POLL_OUT 2
#define POLL_MSG 3
#define POLL_ERR 4
#define POLL_PRI 5
#define POLL_HUP 6

/* unmap_mapping_range: quita las PTE de los VMA que mapean ese rango. */
void lkpi_unmap_mapping_range(struct address_space *mapping, loff_t holebegin,
			      loff_t holelen, int even_cows);
#define unmap_mapping_range(m, b, l, e) lkpi_unmap_mapping_range(m, b, l, e)
void lkpi_address_space_init(struct address_space *mapping, struct inode *host);
#define address_space_init_once(m) lkpi_address_space_init(m, NULL)
struct inode *lkpi_alloc_anon_inode(struct super_block *sb);
void lkpi_iput(struct inode *inode);
static inline void ihold(struct inode *inode) { atomic_inc(&inode->i_count); }
#define iput(i) lkpi_iput(i)
/* sin modulos descargables por fichero: fops_get/put no cuentan referencias */
#define fops_get(fops) (fops)
#define fops_put(fops) do { (void)(fops); } while (0)
#define replace_fops(f, fops) do { (f)->f_op = (fops); } while (0)
struct inode *lkpi_d_inode(struct dentry *d);
#define d_inode(d) lkpi_d_inode(d)
#define alloc_anon_inode(sb) lkpi_alloc_anon_inode(sb)

/* Clonar un fichero abierto (leases de DRM): no soportado en la capa */
static inline struct file *file_clone_open(struct file *file)
{
	return ERR_PTR(-EOPNOTSUPP);
}

#include <linux/uaccess.h>


#include <linux/uio.h>
#define MAX_RW_COUNT (INT_MAX & PAGE_MASK)
struct kiocb {
	struct file *ki_filp;
	loff_t ki_pos;
	int ki_flags;
};
static inline void init_sync_kiocb(struct kiocb *kiocb, struct file *filp)
{
	kiocb->ki_filp = filp;
	kiocb->ki_pos = 0;
	kiocb->ki_flags = 0;
}
static inline bool force_o_largefile(void) { return true; }
static inline struct file *get_file_active(struct file **f) { return *f; }
/* mmap de un fichero de otro llamante en el espacio de usuario: sin vm_mmap hasta L9 (userptr/GEM legacy). */
static inline unsigned long vm_mmap(struct file *file, unsigned long addr, unsigned long len,
				    unsigned long prot, unsigned long flag, unsigned long offset)
{
	return -ENODEV;
}
struct vm_area_struct;
static inline int vfs_mmap(struct file *file, struct vm_area_struct *vma) { return -ENODEV; }
ssize_t lkpi_kernel_write(struct file *file, const void *buf, size_t count, loff_t *pos);
#define kernel_write lkpi_kernel_write
static inline int filp_close(struct file *filp, void *id) { return 0; }
#endif
