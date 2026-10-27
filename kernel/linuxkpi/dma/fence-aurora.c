// SPDX-License-Identifier: GPL-2.0-only
/*
 * K7 (L5): enlace entre dma_fence y los fences de Aurora (aur_fence, un
 * syncobj binario de kernel/drm/drm_syncobj.c).
 *
 *  - dma_fence -> aurora_fence: un callback del dma_fence senala el de
 *    Aurora; los sync_file/syncobj/dmabuf de Aurora ven asi el fence del
 *    driver de Linux.
 *  - aurora_fence -> dma_fence: un dma_fence envoltorio que se senala cuando
 *    lo hace el de Aurora (aur_fence_watch), para que los drivers de Linux
 *    esperen fences de sync_file de usuario y del fence implicito de los
 *    dmabuf nativos.
 *  - lkpi_dma_fence_signalled(): gancho del parche de dma-fence.c; despierta
 *    a los poll()/espera de Aurora sobre dmabuf cuyo dma_resv cambia.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/dma-fence.h>
#include <linux/slab.h>

#include "../aurora.h"
#include "../lkpi_internal.h"

/* ------------------------------------------- aurora_fence -> dma_fence */

struct lkpi_afence {
	struct dma_fence base;
	spinlock_t lock;
	struct aurora_fence *af;
};

static const char *lkpi_afence_driver_name(struct dma_fence *f)
{
	return "aurora";
}

static const char *lkpi_afence_timeline_name(struct dma_fence *f)
{
	return "sync_file";
}

static void lkpi_afence_fire(void *data)
{
	struct lkpi_afence *a = data;

	dma_fence_signal(&a->base);
	dma_fence_put(&a->base);
}

static bool lkpi_afence_enable_signaling(struct dma_fence *f)
{
	struct lkpi_afence *a = container_of(f, struct lkpi_afence, base);

	dma_fence_get(f);		/* la del observador */
	if (aurora_fence_watch(a->af, lkpi_afence_fire, a) == 0)
		return true;
	dma_fence_put(f);
	return false;			/* ya senalado (o sin memoria): se senala ahora */
}

static bool lkpi_afence_signaled(struct dma_fence *f)
{
	struct lkpi_afence *a = container_of(f, struct lkpi_afence, base);

	return aurora_fence_signaled(a->af);
}

static void lkpi_afence_release(struct dma_fence *f)
{
	struct lkpi_afence *a = container_of(f, struct lkpi_afence, base);

	aurora_fence_put(a->af);
	dma_fence_free(f);
}

static const struct dma_fence_ops lkpi_afence_ops = {
	.get_driver_name = lkpi_afence_driver_name,
	.get_timeline_name = lkpi_afence_timeline_name,
	.enable_signaling = lkpi_afence_enable_signaling,
	.signaled = lkpi_afence_signaled,
	.release = lkpi_afence_release,
};

struct dma_fence *lkpi_dma_fence_from_aurora(struct aurora_fence *af)
{
	struct lkpi_afence *a = kzalloc(sizeof(*a), GFP_KERNEL);

	if (!a)
		return NULL;
	aurora_fence_get(af);
	a->af = af;
	spin_lock_init(&a->lock);
	dma_fence_init(&a->base, &lkpi_afence_ops, &a->lock, dma_fence_context_alloc(1), 1);
	return &a->base;
}
EXPORT_SYMBOL(lkpi_dma_fence_from_aurora);

struct dma_fence *lkpi_dma_fence_from_aurora_fd(int fd)
{
	struct aurora_fence *af;
	struct dma_fence *f;

	if (aurora_fence_from_fd(fd, &af))
		return NULL;
	f = lkpi_dma_fence_from_aurora(af);
	aurora_fence_put(af);
	return f;
}
EXPORT_SYMBOL(lkpi_dma_fence_from_aurora_fd);

/* ------------------------------------------- dma_fence -> aurora_fence */

struct lkpi_dfwatch {
	struct dma_fence_cb cb;
	struct aurora_fence *af;
};

static void lkpi_dfwatch_fn(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct lkpi_dfwatch *w = container_of(cb, struct lkpi_dfwatch, cb);

	aurora_fence_signal(w->af);
	aurora_fence_put(w->af);
	kfree(w);
}

struct aurora_fence *lkpi_dma_fence_to_aurora(struct dma_fence *fence)
{
	struct aurora_fence *af = aurora_fence_create(0);
	struct lkpi_dfwatch *w;

	if (!af)
		return NULL;
	if (!fence || dma_fence_is_signaled(fence)) {
		aurora_fence_signal(af);
		return af;
	}
	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w) {
		aurora_fence_put(af);
		return NULL;
	}
	aurora_fence_get(af);		/* la del callback */
	w->af = af;
	if (dma_fence_add_callback(fence, &w->cb, lkpi_dfwatch_fn)) {
		/* se senalo entre medias */
		aurora_fence_signal(af);
		aurora_fence_put(af);
		kfree(w);
	}
	return af;
}
EXPORT_SYMBOL(lkpi_dma_fence_to_aurora);

int lkpi_dma_fence_to_aurora_fd(struct dma_fence *fence, bool cloexec)
{
	struct aurora_fence *af = lkpi_dma_fence_to_aurora(fence);
	int fd = -1, rc;

	if (!af)
		return -ENOMEM;
	rc = aurora_fence_to_fd(af, cloexec, &fd);
	aurora_fence_put(af);
	return rc ? rc : fd;
}
EXPORT_SYMBOL(lkpi_dma_fence_to_aurora_fd);

void lkpi_dma_fence_signalled(struct dma_fence *fence)
{
	aurora_fence_wake();
}
EXPORT_SYMBOL(lkpi_dma_fence_signalled);
