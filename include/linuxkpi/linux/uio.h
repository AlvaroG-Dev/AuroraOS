/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_UIO_H
#define _LKPI_LINUX_UIO_H
#include <linux/types.h>
struct iovec { void __user *iov_base; size_t iov_len; };
enum iter_type { ITER_UBUF, ITER_IOVEC, ITER_KVEC, ITER_BVEC };
#define ITER_SOURCE	1
#define ITER_DEST	0
/* Solo el iterador de un unico buffer de usuario (write_iter de shmem de i915). */
struct iov_iter {
	u8 iter_type;
	bool data_source;
	size_t count;
	void __user *ubuf;
};
static inline void iov_iter_ubuf(struct iov_iter *i, unsigned int direction, void __user *buf, size_t count)
{
	i->iter_type = ITER_UBUF;
	i->data_source = direction;
	i->ubuf = buf;
	i->count = count;
}
#endif
