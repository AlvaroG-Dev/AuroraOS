// SPDX-License-Identifier: GPL-2.0-only
/* seq_file (K6, L4). Ver linux/seq_file.h: la salida se genera entera. */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/seq_file.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/uaccess.h>
#include <linux/printk.h>

#define LKPI_SEQ_MAX (8U << 20)

int lkpi_seq_open(struct file *file, const struct seq_operations *op)
{
	struct seq_file *p = kzalloc(sizeof(*p), GFP_KERNEL);

	if (!p)
		return -ENOMEM;
	mutex_init(&p->lock);
	p->op = op;
	p->file = file;
	file->private_data = p;
	return 0;
}
EXPORT_SYMBOL(lkpi_seq_open);

/* Genera toda la salida; repite con un buffer mayor si se desborda. */
static int lkpi_seq_fill(struct seq_file *m)
{
	size_t size = m->size ? m->size : PAGE_SIZE;

	for (;;) {
		loff_t pos = 0;
		void *p;
		int err = 0;

		if (!m->buf || m->size < size) {
			kvfree(m->buf);
			m->buf = kvmalloc(size, GFP_KERNEL);
			if (!m->buf) {
				m->size = 0;
				return -ENOMEM;
			}
			m->size = size;
		}
		m->count = 0;
		m->index = 0;
		p = m->op->start(m, &pos);
		while (p && !IS_ERR(p)) {
			size_t before = m->count;

			err = m->op->show(m, p);
			if (err < 0)
				break;
			if (err == SEQ_SKIP)
				m->count = before;
			if (seq_has_overflowed(m))
				break;
			p = m->op->next(m, p, &pos);
		}
		m->op->stop(m, p);
		if (IS_ERR(p))
			return PTR_ERR(p);
		if (err < 0)
			return err;
		if (!seq_has_overflowed(m))
			return 0;
		if (size >= LKPI_SEQ_MAX)
			return -ENOMEM;
		size *= 2;
	}
}

ssize_t lkpi_seq_read(struct file *file, char __user *buf, size_t size, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	ssize_t ret;
	int err;

	mutex_lock(&m->lock);
	if (*ppos == 0 || !m->buf) {
		err = lkpi_seq_fill(m);
		if (err) {
			mutex_unlock(&m->lock);
			return err;
		}
	}
	ret = simple_read_from_buffer(buf, size, ppos, m->buf, m->count);
	m->read_pos = *ppos;
	mutex_unlock(&m->lock);
	return ret;
}
EXPORT_SYMBOL(lkpi_seq_read);

loff_t lkpi_seq_lseek(struct file *file, loff_t offset, int whence)
{
	struct seq_file *m = file->private_data;
	loff_t pos;

	mutex_lock(&m->lock);
	switch (whence) {
	case SEEK_CUR:
		pos = file->f_pos + offset;
		break;
	case SEEK_SET:
		pos = offset;
		break;
	default:
		pos = -EINVAL;
	}
	if (pos < 0 && pos != -EINVAL)
		pos = -EINVAL;
	if (pos >= 0)
		file->f_pos = pos;
	mutex_unlock(&m->lock);
	return pos;
}
EXPORT_SYMBOL(lkpi_seq_lseek);

int lkpi_seq_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;

	kvfree(m->buf);
	kfree(m);
	return 0;
}
EXPORT_SYMBOL(lkpi_seq_release);

static void *lkpi_single_start(struct seq_file *p, loff_t *pos)
{
	return *pos ? NULL : SEQ_START_TOKEN;
}

static void *lkpi_single_next(struct seq_file *p, void *v, loff_t *pos)
{
	++*pos;
	return NULL;
}

static void lkpi_single_stop(struct seq_file *p, void *v)
{
}

int lkpi_single_open(struct file *file, int (*show)(struct seq_file *, void *), void *data)
{
	struct seq_operations *op = kmalloc(sizeof(*op), GFP_KERNEL);
	int res;

	if (!op)
		return -ENOMEM;
	op->start = lkpi_single_start;
	op->next = lkpi_single_next;
	op->stop = lkpi_single_stop;
	op->show = show;
	res = lkpi_seq_open(file, op);
	if (res) {
		kfree(op);
		return res;
	}
	((struct seq_file *)file->private_data)->private = data;
	return 0;
}
EXPORT_SYMBOL(lkpi_single_open);

int lkpi_single_open_size(struct file *file, int (*show)(struct seq_file *, void *), void *data,
			  size_t size)
{
	int rc = lkpi_single_open(file, show, data);

	if (!rc) {
		struct seq_file *m = file->private_data;

		m->buf = kvmalloc(size, GFP_KERNEL);
		m->size = m->buf ? size : 0;
	}
	return rc;
}
EXPORT_SYMBOL(lkpi_single_open_size);

int lkpi_single_release(struct inode *inode, struct file *file)
{
	const struct seq_operations *op = ((struct seq_file *)file->private_data)->op;
	int res = lkpi_seq_release(inode, file);

	kfree(op);
	return res;
}
EXPORT_SYMBOL(lkpi_single_release);

void *lkpi___seq_open_private(struct file *file, const struct seq_operations *ops, int psize)
{
	void *priv = kzalloc(psize, GFP_KERNEL);
	int rc;

	if (!priv)
		return ERR_PTR(-ENOMEM);
	rc = lkpi_seq_open(file, ops);
	if (rc) {
		kfree(priv);
		return ERR_PTR(rc);
	}
	((struct seq_file *)file->private_data)->private = priv;
	return priv;
}
EXPORT_SYMBOL(lkpi___seq_open_private);

int lkpi_seq_release_private(struct inode *inode, struct file *file)
{
	kfree(((struct seq_file *)file->private_data)->private);
	return lkpi_seq_release(inode, file);
}
EXPORT_SYMBOL(lkpi_seq_release_private);

void lkpi_seq_vprintf(struct seq_file *m, const char *f, va_list args)
{
	int len;

	if (m->count < m->size) {
		len = vsnprintf(m->buf + m->count, m->size - m->count, f, args);
		if (m->count + len < m->size) {
			m->count += len;
			return;
		}
	}
	m->count = m->size;	/* desbordado: lkpi_seq_fill repite con mas sitio */
}
EXPORT_SYMBOL(lkpi_seq_vprintf);

void lkpi_seq_printf(struct seq_file *m, const char *f, ...)
{
	va_list args;

	va_start(args, f);
	lkpi_seq_vprintf(m, f, args);
	va_end(args);
}
EXPORT_SYMBOL(lkpi_seq_printf);

void lkpi_seq_write(struct seq_file *m, const void *data, size_t len)
{
	if (m->count + len < m->size) {
		memcpy(m->buf + m->count, data, len);
		m->count += len;
		return;
	}
	m->count = m->size;
}
EXPORT_SYMBOL(lkpi_seq_write);

void lkpi_seq_puts(struct seq_file *m, const char *s)
{
	lkpi_seq_write(m, s, strlen(s));
}
EXPORT_SYMBOL(lkpi_seq_puts);

void lkpi_seq_putc(struct seq_file *m, char c)
{
	lkpi_seq_write(m, &c, 1);
}
EXPORT_SYMBOL(lkpi_seq_putc);

void lkpi_seq_hex_dump(struct seq_file *m, const char *prefix_str, int prefix_type, int rowsize,
		       int groupsize, const void *buf, size_t len, bool ascii)
{
	const u8 *p = buf;
	size_t i, j;

	if (rowsize != 16 && rowsize != 32)
		rowsize = 16;
	for (i = 0; i < len; i += rowsize) {
		lkpi_seq_puts(m, prefix_str);
		if (prefix_type == DUMP_PREFIX_OFFSET)
			lkpi_seq_printf(m, "%.8zx: ", i);
		else if (prefix_type == DUMP_PREFIX_ADDRESS)
			lkpi_seq_printf(m, "%p: ", p + i);
		for (j = 0; j < (size_t)rowsize && i + j < len; j++)
			lkpi_seq_printf(m, "%02x ", p[i + j]);
		if (ascii)
			for (j = 0; j < (size_t)rowsize && i + j < len; j++)
				lkpi_seq_putc(m, (p[i + j] >= 32 && p[i + j] < 127) ? p[i + j] : '.');
		lkpi_seq_putc(m, '\n');
	}
}
EXPORT_SYMBOL(lkpi_seq_hex_dump);
