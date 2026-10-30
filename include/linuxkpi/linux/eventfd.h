/* SPDX-License-Identifier: GPL-2.0-only */
/* eventfd: la capa no lo tiene; drm_syncobj (EVENTFD) recibe -ENOSYS. */
#ifndef _LKPI_LINUX_EVENTFD_H
#define _LKPI_LINUX_EVENTFD_H

#include <linux/err.h>
#include <linux/types.h>

struct eventfd_ctx;

static inline struct eventfd_ctx *eventfd_ctx_fdget(int fd)
{
	return ERR_PTR(-ENOSYS);
}

static inline void eventfd_ctx_put(struct eventfd_ctx *ctx) { }
static inline void eventfd_signal(struct eventfd_ctx *ctx) { }

#endif
