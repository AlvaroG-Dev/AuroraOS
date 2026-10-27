/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * seq_file (K6, L4). Implementacion de la capa: cada lectura desde el
 * offset 0 genera toda la salida (start/next/show/stop) en un buffer que
 * crece si hace falta, y las lecturas siguientes la sirven por offset. Basta
 * para debugfs y atributos de drivers (salidas de pocos KiB).
 */
#ifndef _LKPI_LINUX_SEQ_FILE_H
#define _LKPI_LINUX_SEQ_FILE_H

#include <linux/types.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/stdarg.h>

struct seq_operations;

struct seq_file {
	char *buf;
	size_t size;
	size_t from;
	size_t count;			/* bytes generados */
	size_t pad_until;
	loff_t index;
	loff_t read_pos;
	struct mutex lock;
	const struct seq_operations *op;
	int poll_event;
	const struct file *file;
	void *private;
};

struct seq_operations {
	void *(*start)(struct seq_file *m, loff_t *pos);
	void (*stop)(struct seq_file *m, void *v);
	void *(*next)(struct seq_file *m, void *v, loff_t *pos);
	int (*show)(struct seq_file *m, void *v);
};

#define SEQ_SKIP 1
#define SEQ_START_TOKEN ((void *)1)

int lkpi_seq_open(struct file *file, const struct seq_operations *op);
ssize_t lkpi_seq_read(struct file *file, char __user *buf, size_t size, loff_t *ppos);
loff_t lkpi_seq_lseek(struct file *file, loff_t offset, int whence);
int lkpi_seq_release(struct inode *inode, struct file *file);
int lkpi_single_open(struct file *file, int (*show)(struct seq_file *, void *), void *data);
int lkpi_single_open_size(struct file *file, int (*show)(struct seq_file *, void *), void *data,
			  size_t size);
int lkpi_single_release(struct inode *inode, struct file *file);
void *lkpi___seq_open_private(struct file *file, const struct seq_operations *ops, int psize);
int lkpi_seq_release_private(struct inode *inode, struct file *file);
__printf(2, 3) void lkpi_seq_printf(struct seq_file *m, const char *fmt, ...);
void lkpi_seq_vprintf(struct seq_file *m, const char *fmt, va_list args);
void lkpi_seq_puts(struct seq_file *m, const char *s);
void lkpi_seq_putc(struct seq_file *m, char c);
void lkpi_seq_write(struct seq_file *m, const void *data, size_t len);
void lkpi_seq_hex_dump(struct seq_file *m, const char *prefix_str, int prefix_type, int rowsize,
		       int groupsize, const void *buf, size_t len, bool ascii);

#define seq_open(f, o) lkpi_seq_open(f, o)
#define seq_read lkpi_seq_read
#define seq_lseek lkpi_seq_lseek
#define seq_release lkpi_seq_release
#define single_open(f, s, d) lkpi_single_open(f, s, d)
#define single_open_size(f, s, d, sz) lkpi_single_open_size(f, s, d, sz)
#define single_release lkpi_single_release
#define __seq_open_private(f, o, s) lkpi___seq_open_private(f, o, s)
#define seq_open_private(f, o, s) PTR_ERR_OR_ZERO(lkpi___seq_open_private(f, o, s))
#define seq_release_private lkpi_seq_release_private
#define seq_printf lkpi_seq_printf
#define seq_vprintf lkpi_seq_vprintf
#define seq_puts(m, s) lkpi_seq_puts(m, s)
#define seq_putc(m, c) lkpi_seq_putc(m, c)
#define seq_write(m, d, l) lkpi_seq_write(m, d, l)
#define seq_hex_dump lkpi_seq_hex_dump
static inline bool seq_has_overflowed(struct seq_file *m) { return m->count == m->size; }
static inline void seq_put_decimal_ull(struct seq_file *m, const char *delimiter,
				       unsigned long long num)
{
	lkpi_seq_printf(m, "%s%llu", delimiter, num);
}
static inline void seq_put_decimal_ll(struct seq_file *m, const char *delimiter, long long num)
{
	lkpi_seq_printf(m, "%s%lld", delimiter, num);
}

#define DEFINE_SEQ_ATTRIBUTE(__name) \
static int __name##_open(struct inode *inode, struct file *file) \
{ \
	int ret = seq_open(file, &__name##_sops); \
	if (!ret && inode->i_private) { \
		struct seq_file *seq_f = file->private_data; \
		seq_f->private = inode->i_private; \
	} \
	return ret; \
} \
static const struct file_operations __name##_fops = { \
	.owner = THIS_MODULE, \
	.open = __name##_open, \
	.read = seq_read, \
	.llseek = seq_lseek, \
	.release = seq_release, \
}

#define DEFINE_SHOW_ATTRIBUTE(__name) \
static int __name##_open(struct inode *inode, struct file *file) \
{ \
	return single_open(file, __name##_show, inode->i_private); \
} \
static const struct file_operations __name##_fops = { \
	.owner = THIS_MODULE, \
	.open = __name##_open, \
	.read = seq_read, \
	.llseek = seq_lseek, \
	.release = single_release, \
}

#define DEFINE_SHOW_STORE_ATTRIBUTE(__name) \
static int __name##_open(struct inode *inode, struct file *file) \
{ \
	return single_open(file, __name##_show, inode->i_private); \
} \
static const struct file_operations __name##_fops = { \
	.owner = THIS_MODULE, \
	.open = __name##_open, \
	.read = seq_read, \
	.write = __name##_write, \
	.llseek = seq_lseek, \
	.release = single_release, \
}

#endif
