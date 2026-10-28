/* SPDX-License-Identifier: GPL-2.0-only */
/* seq_buf: buffer de texto de tamano fijo sobre memoria del llamante. */
#ifndef _LKPI_LINUX_SEQ_BUF_H
#define _LKPI_LINUX_SEQ_BUF_H

#include <linux/types.h>
#include <linux/string.h>
#include <linux/stdarg.h>
#include <linux/kernel.h>

struct seq_buf {
	char *buffer;
	size_t size;
	size_t len;
};

static inline void seq_buf_init(struct seq_buf *s, char *buf, unsigned int size)
{
	s->buffer = buf;
	s->size = size;
	s->len = 0;
	if (size)
		buf[0] = '\0';
}

#define DECLARE_SEQ_BUF(NAME, SIZE)						\
	char __seq_buf_buffer_##NAME[SIZE] = { 0 };				\
	struct seq_buf NAME = {							\
		.buffer = &__seq_buf_buffer_##NAME[0],				\
		.size = SIZE,							\
	}

static inline bool seq_buf_has_overflowed(struct seq_buf *s)
{
	return s->len > s->size;
}

static inline void seq_buf_clear(struct seq_buf *s)
{
	s->len = 0;
	if (s->size)
		s->buffer[0] = '\0';
}

static inline unsigned int seq_buf_used(struct seq_buf *s)
{
	return min(s->len, s->size);
}

/* Cadena terminada en cero (cortada si hubo desbordamiento) */
static inline const char *seq_buf_str(struct seq_buf *s)
{
	if (!s->size)
		return "";
	if (s->len < s->size)
		s->buffer[s->len] = '\0';
	else
		s->buffer[s->size - 1] = '\0';
	return s->buffer;
}

static inline int seq_buf_vprintf(struct seq_buf *s, const char *fmt, va_list args)
{
	int ret;

	if (s->len >= s->size)
		return -1;
	ret = vsnprintf(s->buffer + s->len, s->size - s->len, fmt, args);
	if (ret >= (int)(s->size - s->len)) {
		s->len = s->size + 1;	/* desbordado */
		return -1;
	}
	s->len += ret;
	return 0;
}

static inline int seq_buf_printf(struct seq_buf *s, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = seq_buf_vprintf(s, fmt, ap);
	va_end(ap);
	return ret;
}

#endif
