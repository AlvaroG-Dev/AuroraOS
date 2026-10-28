/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_RELAY_H
#define _LKPI_LINUX_RELAY_H
/* relay (canal de log de GuC hacia debugfs): no existe; relay_open() falla y el log queda en memoria. */
#include <linux/types.h>
#include <linux/fs.h>
struct rchan_buf { size_t subbufs_produced; size_t subbufs_consumed; size_t padding[1]; };
struct rchan;
struct rchan_callbacks {
	int (*subbuf_start)(struct rchan_buf *buf, void *subbuf, void *prev_subbuf);
	struct dentry *(*create_buf_file)(const char *filename, struct dentry *parent, umode_t mode,
					  struct rchan_buf *buf, int *is_global);
	int (*remove_buf_file)(struct dentry *dentry);
};
struct rchan {
	size_t subbuf_size;
	size_t n_subbufs;
	void *private_data;
	const struct rchan_callbacks *cb;
	struct rchan_buf *buf[1];
};
static inline struct rchan *relay_open(const char *base_filename, struct dentry *parent, size_t subbuf_size,
				       size_t n_subbufs, const struct rchan_callbacks *cb, void *private_data)
{
	return NULL;
}
static inline void relay_close(struct rchan *chan) {}
static inline void relay_flush(struct rchan *chan) {}
static inline void *relay_reserve(struct rchan *chan, size_t length) { return NULL; }
static inline void relay_write(struct rchan *chan, const void *data, size_t length) {}
static inline int relay_buf_full(struct rchan_buf *buf) { return 1; }
extern const struct file_operations relay_file_operations;
#endif
