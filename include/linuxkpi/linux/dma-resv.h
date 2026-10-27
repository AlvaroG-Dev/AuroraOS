/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * dma_resv MINIMO (L4): solo el cerrojo ww_mutex de reservation_ww_class.
 * Los fences (dma_resv_add_fence, iteradores, wait) llegan con K7 (L5).
 */
#ifndef _LKPI_LINUX_DMA_RESV_H
#define _LKPI_LINUX_DMA_RESV_H

#include <linux/ww_mutex.h>

extern struct ww_class lkpi_reservation_ww_class;
#define reservation_ww_class lkpi_reservation_ww_class

struct dma_resv {
	struct ww_mutex lock;
};

static inline void dma_resv_init(struct dma_resv *obj)
{
	ww_mutex_init(&obj->lock, &reservation_ww_class);
}
static inline void dma_resv_fini(struct dma_resv *obj) { ww_mutex_destroy(&obj->lock); }
static inline int dma_resv_lock(struct dma_resv *obj, struct ww_acquire_ctx *ctx)
{
	return ww_mutex_lock(&obj->lock, ctx);
}
static inline int dma_resv_lock_interruptible(struct dma_resv *obj, struct ww_acquire_ctx *ctx)
{
	return ww_mutex_lock_interruptible(&obj->lock, ctx);
}
static inline bool dma_resv_trylock(struct dma_resv *obj)
{
	return ww_mutex_trylock(&obj->lock, NULL);
}
static inline void dma_resv_unlock(struct dma_resv *obj) { ww_mutex_unlock(&obj->lock); }
static inline bool dma_resv_is_locked(struct dma_resv *obj) { return ww_mutex_is_locked(&obj->lock); }
#define dma_resv_assert_held(obj) do { (void)(obj); } while (0)
#define dma_resv_held(obj) dma_resv_is_locked(obj)

#endif
