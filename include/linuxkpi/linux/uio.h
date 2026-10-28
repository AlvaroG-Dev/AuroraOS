/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_UIO_H
#define _LKPI_LINUX_UIO_H
#include <linux/types.h>
struct iovec { void __user *iov_base; size_t iov_len; };
#endif
