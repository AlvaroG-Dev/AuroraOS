// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pruebas de L6 (K9-ttm): un driver de memoria de dispositivo simulado sobre
 * lkpi_drm.kur + lkpi_ttm.kur. El "VRAM" son 4 MiB de RAM contiguos que se
 * mapean como iomem WC y los gestiona drm_buddy; el "GTT" es un gestor de
 * rangos TT sobre paginas del sistema (ttm_pool); SYSTEM es el de TTM. Los
 * movimientos VRAM <-> GTT los ejecuta un "motor de copia" simulado detras de
 * un drm_gpu_scheduler (fences reales: s_fence y un fence de hardware que
 * senala un trabajo diferido), o ttm_bo_move_memcpy() como camino sincrono.
 *
 * Cubre: drm_buddy, creacion/mapeo (kmap/vmap) de BO, evicion por falta de
 * VRAM, evict_all, swap a shmem (ttm_backup), dma_resv con fences del
 * planificador, gpu-sched (orden, dependencias, creditos, hang), el shrinker
 * de TTM y GEM shmem a escala. Con KT_SMP=1 corre ademas el estres con varios
 * hilos compitiendo por el VRAM.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-resv.h>
#include <linux/gpu_buddy.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/platform_device.h>
#include <linux/shrinker.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_vma_manager.h>
#include <drm/gpu_scheduler.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_range_manager.h>
#include <drm/ttm/ttm_resource.h>
#include <drm/ttm/ttm_tt.h>
#include "lkpi_test.h"

#define KT_VRAM_ORDER	10			/* 4 MiB: el maximo de alloc_pages */
#define KT_VRAM_SIZE	(PAGE_SIZE << KT_VRAM_ORDER)
#define KT_GTT_PAGES	(16 * 256)		/* 16 MiB de apertura GTT */
#define KT_FENCE_LAT_MS	1

struct kt_dev;

/* ------------------------------------------------------------ VRAM */

struct kt_vram_mgr {
	struct ttm_resource_manager man;
	struct gpu_buddy mm;
	struct mutex lock;
};

struct kt_vram_res {
	struct ttm_resource base;
	struct list_head blocks;
};

/* ------------------------------------------------------------ motor */

struct kt_hwf {
	struct dma_fence f;
	spinlock_t lock;
	struct list_head node;
	struct kt_job *job;
};

struct kt_engine {
	spinlock_t lock;
	struct list_head q;
	struct delayed_work work;
	u64 ctx;
	atomic64_t seqno;
	atomic_t order;		/* orden real de ejecucion */
	atomic_t hang_next;	/* el motor "cuelga" la proxima pasada */
	atomic_t executed;
	atomic_t timedout;
	unsigned int lat_ms;
	struct kt_dev *kd;
};

struct kt_job {
	struct drm_sched_job base;
	struct kt_dev *kd;
	/* copia de un BO entre VRAM y GTT */
	struct ttm_buffer_object *bo;
	struct ttm_tt *ttm;
	/*
	 * Lo que el motor necesita de los recursos, copiado al crear el trabajo:
	 * una evicion segmentada libera el recurso de origen al instante (TTM
	 * solo retiene el gestor con su fence `move`), asi que el trabajo no
	 * puede leer la estructura despues.
	 */
	bool to_vram;
	unsigned long vram_page, npages;
	/* trabajo "de computo" con contador */
	atomic_t *counter;
	int expected_order;
	int exec_order;
	int err;
};

struct kt_dev {
	struct drm_device drm;
	struct ttm_device bdev;
	struct kt_vram_mgr vram;
	struct page *vram_pages;
	phys_addr_t vram_base;
	u8 __iomem *vram_va;
	struct kt_engine eng;
	struct drm_gpu_scheduler sched;
	struct drm_sched_entity ent;
	bool sched_ok;
	bool async_moves;
	enum ttm_caching tt_caching;
	atomic_t moves_async, moves_sync, moves_null;
	atomic_t bos;
};

static struct kt_dev *kd_of(struct ttm_device *bdev)
{
	return container_of(bdev, struct kt_dev, bdev);
}

static int kt_vram_alloc(struct ttm_resource_manager *man, struct ttm_buffer_object *bo,
			 const struct ttm_place *place, struct ttm_resource **res)
{
	struct kt_vram_mgr *mgr = container_of(man, struct kt_vram_mgr, man);
	struct kt_vram_res *r;
	u64 start = (u64)place->fpfn << PAGE_SHIFT;
	u64 end = place->lpfn ? (u64)place->lpfn << PAGE_SHIFT : mgr->mm.size;
	unsigned long flags = GPU_BUDDY_CONTIGUOUS_ALLOCATION;
	struct gpu_buddy_block *blk;
	int err;

	if (place->fpfn || place->lpfn)
		flags |= GPU_BUDDY_RANGE_ALLOCATION;
	if (place->flags & TTM_PL_FLAG_TOPDOWN)
		flags |= GPU_BUDDY_TOPDOWN_ALLOCATION;
	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	INIT_LIST_HEAD(&r->blocks);
	ttm_resource_init(bo, place, &r->base);
	mutex_lock(&mgr->lock);
	err = gpu_buddy_alloc_blocks(&mgr->mm, start, end, bo->base.size, PAGE_SIZE, &r->blocks,
				     flags);
	mutex_unlock(&mgr->lock);
	if (err) {
		ttm_resource_fini(man, &r->base);
		kfree(r);
		return err;
	}
	blk = list_first_entry(&r->blocks, struct gpu_buddy_block, link);
	r->base.start = gpu_buddy_block_offset(blk) >> PAGE_SHIFT;
	*res = &r->base;
	return 0;
}

static void kt_vram_free(struct ttm_resource_manager *man, struct ttm_resource *res)
{
	struct kt_vram_mgr *mgr = container_of(man, struct kt_vram_mgr, man);
	struct kt_vram_res *r = container_of(res, struct kt_vram_res, base);

	mutex_lock(&mgr->lock);
	gpu_buddy_free_list(&mgr->mm, &r->blocks, 0);
	mutex_unlock(&mgr->lock);
	ttm_resource_fini(man, res);
	kfree(r);
}

static const struct ttm_resource_manager_func kt_vram_func = {
	.alloc = kt_vram_alloc,
	.free = kt_vram_free,
};

/* ------------------------------------------------------------ ttm_device */

static struct ttm_tt *kt_tt_create(struct ttm_buffer_object *bo, uint32_t page_flags)
{
	struct ttm_tt *tt = kzalloc(sizeof(*tt), GFP_KERNEL);

	if (!tt)
		return NULL;
	if (ttm_tt_init(tt, bo, page_flags, kd_of(bo->bdev)->tt_caching, 0)) {
		kfree(tt);
		return NULL;
	}
	return tt;
}

static int kt_tt_populate(struct ttm_device *bdev, struct ttm_tt *tt, struct ttm_operation_ctx *ctx)
{
	return ttm_pool_alloc(&bdev->pool, tt, ctx);
}

static void kt_tt_unpopulate(struct ttm_device *bdev, struct ttm_tt *tt)
{
	ttm_pool_free(&bdev->pool, tt);
}

static void kt_tt_destroy(struct ttm_device *bdev, struct ttm_tt *tt)
{
	ttm_tt_fini(tt);
	kfree(tt);
}

static void kt_evict_flags(struct ttm_buffer_object *bo, struct ttm_placement *p)
{
	static const struct ttm_place to_tt = { .mem_type = TTM_PL_TT };
	static const struct ttm_place to_sys = { .mem_type = TTM_PL_SYSTEM };

	p->num_placement = 1;
	p->placement = bo->resource->mem_type == TTM_PL_VRAM ? &to_tt : &to_sys;
}

static int kt_io_mem_reserve(struct ttm_device *bdev, struct ttm_resource *mem)
{
	if (mem->mem_type != TTM_PL_VRAM)
		return 0;
	mem->bus.offset = kd_of(bdev)->vram_base + ((phys_addr_t)mem->start << PAGE_SHIFT);
	mem->bus.is_iomem = true;
	mem->bus.caching = ttm_write_combined;
	return 0;
}

static struct kt_hwf *kt_hwf_of(struct dma_fence *f)
{
	return container_of(f, struct kt_hwf, f);
}

static const char *kt_hw_driver(struct dma_fence *f) { return "kt_ttm"; }
static const char *kt_hw_timeline(struct dma_fence *f) { return "engine0"; }

static void kt_hw_release(struct dma_fence *f)
{
	kfree(kt_hwf_of(f));
}

static const struct dma_fence_ops kt_hw_ops = {
	.get_driver_name = kt_hw_driver,
	.get_timeline_name = kt_hw_timeline,
	.release = kt_hw_release,
};

/*
 * Copia del motor: VRAM <-> paginas del TT, pagina a pagina. El "motor" ve el
 * VRAM por su propio mapeo WC (kd->vram_va), como un DMA que no pasa por los
 * iteradores de TTM.
 */
static void kt_do_copy(struct kt_job *j)
{
	struct kt_dev *kd = j->kd;
	u8 *vram = kd->vram_va + (j->vram_page << PAGE_SHIFT);
	unsigned long i;

	if (!j->ttm || !j->ttm->pages) {
		j->err = -EINVAL;
		return;
	}
	for (i = 0; i < j->npages; i++) {
		struct page *pg = j->ttm->pages[i];

		if (!pg) {
			j->err = -EFAULT;
			return;
		}
		if (j->to_vram)
			memcpy_toio(vram + i * PAGE_SIZE, page_address(pg), PAGE_SIZE);
		else
			memcpy_fromio(page_address(pg), vram + i * PAGE_SIZE, PAGE_SIZE);
	}
}

static void kt_engine_work(struct work_struct *w)
{
	struct kt_engine *e = container_of(to_delayed_work(w), struct kt_engine, work);
	struct kt_hwf *hw;

	for (;;) {
		struct kt_job *j;

		spin_lock_irq(&e->lock);
		hw = list_first_entry_or_null(&e->q, struct kt_hwf, node);
		if (hw)
			list_del_init(&hw->node);
		spin_unlock_irq(&e->lock);
		if (!hw)
			return;
		j = hw->job;
		if (atomic_cmpxchg(&e->hang_next, 1, 0)) {
			/* "Cuelga": no senala; el temporizador del planificador actua. */
			dma_fence_put(&hw->f);
			continue;
		}
		j->exec_order = atomic_inc_return(&e->order);
		if (j->bo)
			kt_do_copy(j);
		if (j->counter)
			atomic_inc(j->counter);
		if (j->err)
			dma_fence_set_error(&hw->f, j->err);
		atomic_inc(&e->executed);
		dma_fence_signal(&hw->f);
		dma_fence_put(&hw->f);
	}
}

static struct dma_fence *kt_run_job(struct drm_sched_job *sj)
{
	struct kt_job *j = container_of(sj, struct kt_job, base);
	struct kt_engine *e = &j->kd->eng;
	struct kt_hwf *hw = kzalloc(sizeof(*hw), GFP_KERNEL);

	if (!hw)
		return ERR_PTR(-ENOMEM);
	spin_lock_init(&hw->lock);
	INIT_LIST_HEAD(&hw->node);
	hw->job = j;
	dma_fence_init(&hw->f, &kt_hw_ops, &hw->lock, e->ctx, atomic64_inc_return(&e->seqno));
	dma_fence_get(&hw->f);		/* la del motor */
	spin_lock_irq(&e->lock);
	list_add_tail(&hw->node, &e->q);
	spin_unlock_irq(&e->lock);
	schedule_delayed_work(&e->work, msecs_to_jiffies(e->lat_ms));
	return &hw->f;
}

static enum drm_gpu_sched_stat kt_timedout_job(struct drm_sched_job *sj)
{
	struct kt_job *j = container_of(sj, struct kt_job, base);
	struct kt_dev *kd = j->kd;

	atomic_inc(&kd->eng.timedout);
	drm_sched_stop(&kd->sched, sj);
	/*
	 * El hardware esta "colgado": se descarta el trabajo. El fence de
	 * hardware esta en la lista del motor o ya se solto; si sigue vivo se
	 * senala con error para que el trabajo termine.
	 */
	if (sj->s_fence->parent && !dma_fence_is_signaled(sj->s_fence->parent)) {
		dma_fence_set_error(sj->s_fence->parent, -ETIME);
		dma_fence_signal(sj->s_fence->parent);
	}
	drm_sched_start(&kd->sched, 0);
	return DRM_GPU_SCHED_STAT_RESET;
}

static void kt_free_job(struct drm_sched_job *sj)
{
	struct kt_job *j = container_of(sj, struct kt_job, base);

	drm_sched_job_cleanup(sj);
	kfree(j);
}

static const struct drm_sched_backend_ops kt_sched_ops = {
	.run_job = kt_run_job,
	.timedout_job = kt_timedout_job,
	.free_job = kt_free_job,
};

static struct kt_job *kt_job_new(struct kt_dev *kd)
{
	struct kt_job *j = kzalloc(sizeof(*j), GFP_KERNEL);

	if (!j)
		return NULL;
	j->kd = kd;
	if (drm_sched_job_init(&j->base, &kd->ent, 1, NULL, 0)) {
		kfree(j);
		return NULL;
	}
	return j;
}

/* arm + push; devuelve una referencia al fence "finished". */
static struct dma_fence *kt_job_push(struct kt_job *j)
{
	struct dma_fence *f;

	drm_sched_job_arm(&j->base);
	f = dma_fence_get(&j->base.s_fence->finished);
	drm_sched_entity_push_job(&j->base);
	return f;
}

static int kt_move(struct ttm_buffer_object *bo, bool evict, struct ttm_operation_ctx *ctx,
		   struct ttm_resource *new_mem, struct ttm_place *hop)
{
	struct kt_dev *kd = kd_of(bo->bdev);
	struct ttm_resource *old = bo->resource;
	struct kt_job *j;
	struct dma_fence *f;
	int ret;

	if (!old || (old->mem_type == TTM_PL_SYSTEM && !bo->ttm)) {
		atomic_inc(&kd->moves_null);
		ttm_bo_move_null(bo, new_mem);
		return 0;
	}
	if ((old->mem_type == TTM_PL_SYSTEM && new_mem->mem_type == TTM_PL_TT) ||
	    (old->mem_type == TTM_PL_TT && new_mem->mem_type == TTM_PL_SYSTEM)) {
		/* SYSTEM <-> GTT: las mismas paginas, solo cambia el recurso */
		if (new_mem->mem_type == TTM_PL_TT) {
			ret = ttm_bo_populate(bo, ctx);
			if (ret)
				return ret;
		}
		atomic_inc(&kd->moves_null);
		ttm_bo_move_null(bo, new_mem);
		return 0;
	}
	if (old->mem_type == TTM_PL_VRAM && new_mem->mem_type == TTM_PL_SYSTEM) {
		hop->mem_type = TTM_PL_TT;
		hop->flags = TTM_PL_FLAG_TEMPORARY;
		hop->fpfn = hop->lpfn = 0;
		return -EMULTIHOP;
	}
	if (old->mem_type == TTM_PL_SYSTEM && new_mem->mem_type == TTM_PL_VRAM) {
		hop->mem_type = TTM_PL_TT;
		hop->flags = TTM_PL_FLAG_TEMPORARY;
		hop->fpfn = hop->lpfn = 0;
		return -EMULTIHOP;
	}
	if (!kd->async_moves) {
		atomic_inc(&kd->moves_sync);
		return ttm_bo_move_memcpy(bo, ctx, new_mem);
	}
	/* VRAM <-> GTT con el motor de copia detras del planificador */
	if (bo->ttm && (new_mem->mem_type == TTM_PL_TT || ttm_tt_is_swapped(bo->ttm))) {
		ret = ttm_bo_populate(bo, ctx);
		if (ret)
			return ret;
	}
	j = kt_job_new(kd);
	if (!j)
		return -ENOMEM;
	j->bo = bo;
	j->ttm = bo->ttm;
	j->to_vram = new_mem->mem_type == TTM_PL_VRAM;
	j->vram_page = j->to_vram ? new_mem->start : old->start;
	j->npages = PFN_UP(new_mem->size);
	f = kt_job_push(j);
	atomic_inc(&kd->moves_async);
	ret = ttm_bo_move_accel_cleanup(bo, f, evict, true, new_mem);
	dma_fence_put(f);
	return ret;
}

static const struct ttm_device_funcs kt_ttm_funcs = {
	.ttm_tt_create = kt_tt_create,
	.ttm_tt_populate = kt_tt_populate,
	.ttm_tt_unpopulate = kt_tt_unpopulate,
	.ttm_tt_destroy = kt_tt_destroy,
	.eviction_valuable = ttm_bo_eviction_valuable,
	.evict_flags = kt_evict_flags,
	.move = kt_move,
	.io_mem_reserve = kt_io_mem_reserve,
};

DEFINE_DRM_GEM_FOPS(kt_fops);

static const struct drm_driver kt_driver = {
	.driver_features = DRIVER_GEM,
	.fops = &kt_fops,
	.name = "kt_ttm",
	.desc = "LinuxKPI TTM test",
	.major = 1,
	.minor = 0,
};

/* ------------------------------------------------------------ buffer objects */

struct kt_bo {
	struct ttm_buffer_object bo;
	u32 seed;
};

static void kt_bo_destroy(struct ttm_buffer_object *tbo)
{
	struct kt_dev *kd = kd_of(tbo->bdev);

	drm_gem_object_release(&tbo->base);
	kfree(container_of(tbo, struct kt_bo, bo));
	atomic_dec(&kd->bos);
}

static const struct ttm_place kt_pl_vram = { .mem_type = TTM_PL_VRAM };
static const struct ttm_place kt_pl_tt = { .mem_type = TTM_PL_TT };
static const struct ttm_place kt_pl_sys = { .mem_type = TTM_PL_SYSTEM };
static const struct ttm_place kt_pl_vram_tt[] = {
	{ .mem_type = TTM_PL_VRAM }, { .mem_type = TTM_PL_TT },
};

static struct ttm_placement kt_p_vram = { 1, &kt_pl_vram };
static struct ttm_placement kt_p_tt = { 1, &kt_pl_tt };
static struct ttm_placement kt_p_sys = { 1, &kt_pl_sys };
static struct ttm_placement kt_p_vram_tt = { 2, kt_pl_vram_tt };

/* El BO se crea reservado y validado; se devuelve sin reservar. */
static struct kt_bo *kt_bo_create_type(struct kt_dev *kd, size_t size, struct ttm_placement *p,
				       enum ttm_bo_type type, int *errp)
{
	struct kt_bo *b = kzalloc(sizeof(*b), GFP_KERNEL);
	int ret;

	if (!b) {
		*errp = -ENOMEM;
		return NULL;
	}
	drm_gem_private_object_init(&kd->drm, &b->bo.base, size);
	atomic_inc(&kd->bos);
	ret = ttm_bo_init_validate(&kd->bdev, &b->bo, type, p, 1, false, NULL, NULL, kt_bo_destroy);
	if (ret) {
		/* ttm_bo_init_validate ya llamo a destroy */
		*errp = ret;
		return NULL;
	}
	*errp = 0;
	return b;
}

static struct kt_bo *kt_bo_create(struct kt_dev *kd, size_t size, struct ttm_placement *p,
				  int *errp)
{
	return kt_bo_create_type(kd, size, p, ttm_bo_type_kernel, errp);
}

static int kt_bo_validate(struct kt_bo *b, struct ttm_placement *p)
{
	struct ttm_operation_ctx ctx = { .interruptible = false, .no_wait_gpu = false };
	int ret;

	dma_resv_lock(b->bo.base.resv, NULL);
	ret = ttm_bo_validate(&b->bo, p, &ctx);
	dma_resv_unlock(b->bo.base.resv);
	return ret;
}

/* Recorre el BO con ttm_bo_kmap; op 0 = rellenar, 1 = comprobar (devuelve fallos). */
static long kt_bo_rw(struct kt_bo *b, u32 seed, int check)
{
	struct ttm_operation_ctx ctx = { .interruptible = false };
	struct ttm_bo_kmap_obj map;
	unsigned long npages = b->bo.base.size >> PAGE_SHIFT, pg;
	long bad = 0;
	int ret;

	dma_resv_lock(b->bo.base.resv, NULL);
	ret = ttm_bo_wait_ctx(&b->bo, &ctx);
	if (ret) {
		dma_resv_unlock(b->bo.base.resv);
		return -1000000 - ret;
	}
	for (pg = 0; pg < npages; pg += 16) {
		unsigned long n = min(16UL, npages - pg), i;
		bool iomem;
		u32 *v;

		ret = ttm_bo_kmap(&b->bo, pg, n, &map);
		if (ret) {
			bad = -2000000 - ret;
			break;
		}
		v = ttm_kmap_obj_virtual(&map, &iomem);
		for (i = 0; i < n * PAGE_SIZE / 4; i += 61) {
			u32 want = seed ^ (u32)(pg * 1031 + i * 2654435761U);

			if (!check) {
				if (iomem)
					writel(want, (void __iomem *)&v[i]);
				else
					v[i] = want;
			} else {
				u32 got = iomem ? readl((void __iomem *)&v[i]) : v[i];

				if (got != want)
					bad++;
			}
		}
		ttm_bo_kunmap(&map);
	}
	dma_resv_unlock(b->bo.base.resv);
	return bad;
}

static void kt_bo_free(struct kt_bo *b)
{
	ttm_bo_put(&b->bo);
}

/* --------------------------------------------------------------- setup */

static struct platform_device *kt_pdev;
static struct kt_dev *kt;

static int kt_setup(void)
{
	struct kt_dev *kd;
	int ret;

	kt_pdev = platform_device_register_simple("kt-ttm", -1, NULL, 0);
	if (IS_ERR(kt_pdev))
		return PTR_ERR(kt_pdev);
	kd = devm_drm_dev_alloc(&kt_pdev->dev, &kt_driver, struct kt_dev, drm);
	if (IS_ERR(kd))
		return PTR_ERR(kd);
	kt = kd;
	kd->vram_pages = alloc_pages(GFP_KERNEL | __GFP_ZERO, KT_VRAM_ORDER);
	if (!kd->vram_pages)
		return -ENOMEM;
	kd->vram_base = page_to_phys(kd->vram_pages);
	kd->vram_va = ioremap_wc(kd->vram_base, KT_VRAM_SIZE);
	if (!kd->vram_va)
		return -ENOMEM;

	ret = ttm_device_init(&kd->bdev, &kt_ttm_funcs, kd->drm.dev, kd->drm.anon_inode->i_mapping,
			      kd->drm.vma_offset_manager, false, false);
	if (ret)
		return ret;
	/* VRAM con drm_buddy */
	mutex_init(&kd->vram.lock);
	ret = gpu_buddy_init(&kd->vram.mm, KT_VRAM_SIZE, PAGE_SIZE);
	if (ret)
		return ret;
	ttm_resource_manager_init(&kd->vram.man, &kd->bdev, KT_VRAM_SIZE);
	kd->vram.man.func = &kt_vram_func;
	ttm_set_driver_manager(&kd->bdev, TTM_PL_VRAM, &kd->vram.man);
	ttm_resource_manager_set_used(&kd->vram.man, true);
	/* GTT: apertura de 16 MiB sobre paginas del sistema */
	ret = ttm_range_man_init(&kd->bdev, TTM_PL_TT, true, KT_GTT_PAGES);
	if (ret)
		return ret;

	/* planificador y motor */
	spin_lock_init(&kd->eng.lock);
	INIT_LIST_HEAD(&kd->eng.q);
	INIT_DELAYED_WORK(&kd->eng.work, kt_engine_work);
	kd->eng.ctx = dma_fence_context_alloc(1);
	kd->eng.lat_ms = KT_FENCE_LAT_MS;
	kd->eng.kd = kd;
	{
		struct drm_sched_init_args args = {
			.ops = &kt_sched_ops,
			.num_rqs = DRM_SCHED_PRIORITY_COUNT,
			.credit_limit = 16,
			.hang_limit = 0,
			.timeout = msecs_to_jiffies(200),
			.name = "kt_gfx",
			.dev = kd->drm.dev,
		};
		struct drm_gpu_scheduler *list[1] = { &kd->sched };

		ret = drm_sched_init(&kd->sched, &args);
		if (ret)
			return ret;
		ret = drm_sched_entity_init(&kd->ent, DRM_SCHED_PRIORITY_NORMAL, list, 1, NULL);
		if (ret) {
			drm_sched_fini(&kd->sched);
			return ret;
		}
		kd->sched_ok = true;
	}
	return 0;
}

static void kt_teardown(void)
{
	struct kt_dev *kd = kt;

	if (!kd)
		return;
	if (kd->sched_ok) {
		drm_sched_entity_destroy(&kd->ent);
		drm_sched_fini(&kd->sched);
	}
	cancel_delayed_work_sync(&kd->eng.work);
	ttm_resource_manager_evict_all(&kd->bdev, &kd->vram.man);
	ttm_resource_manager_set_used(&kd->vram.man, false);
	ttm_resource_manager_cleanup(&kd->vram.man);
	ttm_set_driver_manager(&kd->bdev, TTM_PL_VRAM, NULL);
	gpu_buddy_fini(&kd->vram.mm);
	ttm_range_man_fini(&kd->bdev, TTM_PL_TT);
	ttm_device_fini(&kd->bdev);
	if (kd->vram_va)
		iounmap(kd->vram_va);
	if (kd->vram_pages)
		__free_pages(kd->vram_pages, KT_VRAM_ORDER);
	platform_device_unregister(kt_pdev);
	kt = NULL;
}

/* --------------------------------------------------------------- pruebas */

static void test_buddy(void)
{
	struct gpu_buddy mm;
	LIST_HEAD(a);
	LIST_HEAD(b);
	LIST_HEAD(c);
	struct gpu_buddy_block *blk;
	u64 sz = 1ULL << 20;
	u64 total = 0;
	int ret;

	KT_EQ(gpu_buddy_init(&mm, sz, PAGE_SIZE), 0);
	KT_EQ(mm.size, sz);
	KT_EQ(mm.avail, sz);
	KT_EQ(gpu_buddy_alloc_blocks(&mm, 0, sz, 256 * 1024, PAGE_SIZE, &a, 0), 0);
	KT_EQ(mm.avail, sz - 256 * 1024);
	ret = gpu_buddy_alloc_blocks(&mm, 0, sz, 512 * 1024, PAGE_SIZE, &b,
				     GPU_BUDDY_CONTIGUOUS_ALLOCATION);
	KT_EQ(ret, 0);
	blk = list_first_entry(&b, struct gpu_buddy_block, link);
	KT_EQ(gpu_buddy_block_size(&mm, blk), 512 * 1024);
	/* quedan 256 KiB: 512 KiB contiguos ya no caben */
	ret = gpu_buddy_alloc_blocks(&mm, 0, sz, 512 * 1024, PAGE_SIZE, &c,
				     GPU_BUDDY_CONTIGUOUS_ALLOCATION);
	KT_EQ(ret, -ENOSPC);
	ret = gpu_buddy_alloc_blocks(&mm, 0, sz, 256 * 1024, PAGE_SIZE, &c,
				     GPU_BUDDY_CONTIGUOUS_ALLOCATION);
	KT_EQ(ret, 0);
	KT_EQ(mm.avail, 0);
	gpu_buddy_free_list(&mm, &b, 0);
	KT_EQ(mm.avail, 512 * 1024);
	/* rango fijo: dos bloques de 128 KiB dentro de [512K, 1M) */
	gpu_buddy_free_list(&mm, &a, 0);
	gpu_buddy_free_list(&mm, &c, 0);
	KT_EQ(mm.avail, sz);
	KT_EQ(gpu_buddy_alloc_blocks(&mm, 512 * 1024, sz, 128 * 1024, PAGE_SIZE, &a,
				     GPU_BUDDY_RANGE_ALLOCATION), 0);
	list_for_each_entry(blk, &a, link) {
		KT_ASSERT(gpu_buddy_block_offset(blk) >= 512 * 1024, "bloque fuera del rango");
		total += gpu_buddy_block_size(&mm, blk);
	}
	KT_EQ(total, 128 * 1024);
	/* topdown */
	KT_EQ(gpu_buddy_alloc_blocks(&mm, 0, sz, 4096, PAGE_SIZE, &c, GPU_BUDDY_TOPDOWN_ALLOCATION), 0);
	blk = list_first_entry(&c, struct gpu_buddy_block, link);
	KT_ASSERT(gpu_buddy_block_offset(blk) >= sz - 512 * 1024, "topdown: %llx",
		  gpu_buddy_block_offset(blk));
	gpu_buddy_free_list(&mm, &a, 0);
	gpu_buddy_free_list(&mm, &c, 0);
	KT_EQ(mm.avail, sz);
	/* fragmentacion: 256 paginas sueltas y todo se recompone al liberar */
	{
		LIST_HEAD(many);
		int i;

		for (i = 0; i < 256; i++)
			if (gpu_buddy_alloc_blocks(&mm, 0, sz, PAGE_SIZE, PAGE_SIZE, &many, 0))
				break;
		KT_EQ(i, 256);
		KT_EQ(mm.avail, sz - 256 * PAGE_SIZE);
		gpu_buddy_free_list(&mm, &many, 0);
	}
	KT_EQ(mm.avail, sz);
	KT_EQ(gpu_buddy_alloc_blocks(&mm, 0, sz, sz, PAGE_SIZE, &a, GPU_BUDDY_CONTIGUOUS_ALLOCATION), 0);
	gpu_buddy_free_list(&mm, &a, 0);
	gpu_buddy_fini(&mm);
}

static void test_bo_basic(struct kt_dev *kd)
{
	struct kt_bo *b;
	int err;
	long bad;

	b = kt_bo_create(kd, 256 * 1024, &kt_p_vram, &err);
	KT_ASSERT(b, "ttm_bo_init_validate VRAM: %d", err);
	if (!b)
		return;
	KT_EQ(b->bo.resource->mem_type, TTM_PL_VRAM);
	KT_ASSERT(kd->vram.man.usage >= 256 * 1024, "uso del gestor VRAM");
	KT_EQ(kt_bo_rw(b, 0x1234, 0), 0);
	bad = kt_bo_rw(b, 0x1234, 1);
	KT_EQ(bad, 0);
	/* a GTT, de vuelta a VRAM y a SYSTEM: el contenido sigue */
	KT_EQ(kt_bo_validate(b, &kt_p_tt), 0);
	KT_EQ(b->bo.resource->mem_type, TTM_PL_TT);
	KT_EQ(kt_bo_rw(b, 0x1234, 1), 0);
	KT_EQ(kt_bo_validate(b, &kt_p_vram), 0);
	KT_EQ(b->bo.resource->mem_type, TTM_PL_VRAM);
	KT_EQ(kt_bo_rw(b, 0x1234, 1), 0);
	KT_EQ(kt_bo_validate(b, &kt_p_sys), 0);	/* VRAM -> SYSTEM pasa por el hop a TT */
	KT_EQ(b->bo.resource->mem_type, TTM_PL_SYSTEM);
	KT_EQ(kt_bo_rw(b, 0x1234, 1), 0);
	/* vmap */
	{
		struct iosys_map m;
		u32 *p;

		KT_EQ(kt_bo_validate(b, &kt_p_vram), 0);
		dma_resv_lock(b->bo.base.resv, NULL);
		KT_EQ(ttm_bo_wait_ctx(&b->bo, &(struct ttm_operation_ctx){ }), 0);
		err = ttm_bo_vmap(&b->bo, &m);
		KT_EQ(err, 0);
		if (!err) {
			KT_ASSERT(m.is_iomem, "VRAM se mapea como iomem");
			p = m.vaddr_iomem;
			writel(0xfeedf00d, &p[3]);
			KT_EQ(readl(&p[3]), 0xfeedf00d);
			ttm_bo_vunmap(&b->bo, &m);
		}
		dma_resv_unlock(b->bo.base.resv);
	}
	/* pin: no se evicta */
	dma_resv_lock(b->bo.base.resv, NULL);
	ttm_bo_pin(&b->bo);
	dma_resv_unlock(b->bo.base.resv);
	/* un BO fijado no esta en las LRU: evict_all lo deja en su sitio */
	KT_EQ(ttm_resource_manager_evict_all(&kd->bdev, &kd->vram.man), 0);
	KT_EQ(b->bo.resource->mem_type, TTM_PL_VRAM);
	dma_resv_lock(b->bo.base.resv, NULL);
	ttm_bo_unpin(&b->bo);
	dma_resv_unlock(b->bo.base.resv);
	KT_EQ(ttm_resource_manager_evict_all(&kd->bdev, &kd->vram.man), 0);
	KT_ASSERT(b->bo.resource->mem_type != TTM_PL_VRAM, "evict_all saco el BO");
	KT_EQ(kt_bo_rw(b, 0x1234, 1), 0);	/* el contenido sobrevive a la evicion */
	kt_bo_free(b);
}

#define KT_MAXBOS 64

static void test_eviction(struct kt_dev *kd)
{
	struct kt_bo *bos[KT_MAXBOS] = { };
	size_t sz = 512 * 1024;	/* 8 caben en 4 MiB */
	int i, n = 0, err, in_vram = 0, in_tt = 0;
	struct kt_bo *extra;

	/* 12 BO de 512 KiB que piden VRAM y, si no hay, GTT: 8 en VRAM y 4 en GTT */
	for (i = 0; i < 12; i++) {
		bos[n] = kt_bo_create(kd, sz, &kt_p_vram_tt, &err);
		KT_ASSERT(bos[n], "crear BO %d: %d", i, err);
		if (!bos[n])
			break;
		KT_EQ(kt_bo_rw(bos[n], 0x1000 + i, 0), 0);
		n++;
	}
	for (i = 0; i < n; i++) {
		if (bos[i]->bo.resource->mem_type == TTM_PL_VRAM)
			in_vram++;
		else if (bos[i]->bo.resource->mem_type == TTM_PL_TT)
			in_tt++;
	}
	KT_EQ(in_vram, 8);
	KT_EQ(in_tt, 4);
	/* un BO que solo admite VRAM fuerza la evicion de uno de los residentes */
	extra = kt_bo_create(kd, sz, &kt_p_vram, &err);
	KT_ASSERT(extra, "BO solo-VRAM con VRAM llena: %d", err);
	if (extra) {
		int now = 0;

		KT_EQ(extra->bo.resource->mem_type, TTM_PL_VRAM);
		KT_EQ(kt_bo_rw(extra, 0x7777, 0), 0);
		for (i = 0; i < n; i++)
			if (bos[i]->bo.resource->mem_type == TTM_PL_VRAM)
				now++;
		KT_EQ(now, 7);		/* uno salio a GTT */
		/* ninguno perdio su contenido (los que se movieron incluidos) */
		for (i = 0; i < n; i++)
			KT_EQ(kt_bo_rw(bos[i], 0x1000 + i, 1), 0);
		KT_EQ(kt_bo_rw(extra, 0x7777, 1), 0);
		/* llevar uno de GTT a VRAM evicta a otro; todo sigue intacto */
		for (i = 0; i < n; i++)
			if (bos[i]->bo.resource->mem_type == TTM_PL_TT)
				break;
		if (i < n) {
			KT_EQ(kt_bo_validate(bos[i], &kt_p_vram), 0);
			KT_EQ(bos[i]->bo.resource->mem_type, TTM_PL_VRAM);
		}
		for (i = 0; i < n; i++)
			KT_EQ(kt_bo_rw(bos[i], 0x1000 + i, 1), 0);
		KT_EQ(kt_bo_rw(extra, 0x7777, 1), 0);
		kt_bo_free(extra);
	}
	/* con todo liberado, VRAM vuelve a estar entera */
	for (i = 0; i < n; i++)
		kt_bo_free(bos[i]);
	KT_EQ(kd->vram.mm.avail, KT_VRAM_SIZE);
	KT_EQ(kd->vram.man.usage, 0);
}

static void test_swap(struct kt_dev *kd)
{
	struct ttm_operation_ctx ctx = { .interruptible = false };
	struct kt_bo *b;
	int err;
	long swapped;

	b = kt_bo_create(kd, 128 * 1024, &kt_p_tt, &err);
	KT_ASSERT(b, "BO en GTT: %d", err);
	if (!b)
		return;
	KT_EQ(kt_bo_rw(b, 0xabcd, 0), 0);
	KT_ASSERT(ttm_tt_is_populated(b->bo.ttm), "TT poblada");
	/* todo lo que haya en LRU se manda a shmem */
	swapped = ttm_device_swapout(&kd->bdev, &ctx, GFP_KERNEL);
	KT_ASSERT(swapped > 0, "ttm_device_swapout: %ld", swapped);
	KT_ASSERT(b->bo.ttm && ttm_tt_is_swapped(b->bo.ttm), "TT expulsada a shmem");
	/* leerlo la trae de vuelta, intacta */
	KT_EQ(kt_bo_rw(b, 0xabcd, 1), 0);
	KT_ASSERT(!ttm_tt_is_swapped(b->bo.ttm) || ttm_tt_is_populated(b->bo.ttm), "TT recuperada");
	kt_bo_free(b);
}

static void test_async_moves(struct kt_dev *kd)
{
	struct kt_bo *b;
	struct dma_fence *f;
	int err, moves_before;
	struct ttm_operation_ctx ctx = { .interruptible = false };

	kd->async_moves = true;
	moves_before = atomic_read(&kd->moves_async);
	b = kt_bo_create(kd, 256 * 1024, &kt_p_vram, &err);
	KT_ASSERT(b, "BO VRAM: %d", err);
	if (!b) {
		kd->async_moves = false;
		return;
	}
	KT_EQ(kt_bo_rw(b, 0x5555, 0), 0);
	/* VRAM -> GTT por el motor: el fence aparece en la dma_resv antes de acabar */
	KT_EQ(kt_bo_validate(b, &kt_p_tt), 0);
	KT_EQ(b->bo.resource->mem_type, TTM_PL_TT);
	KT_ASSERT(atomic_read(&kd->moves_async) > moves_before, "movimiento por el planificador");
	/* el movimiento dejo un fence KERNEL en la dma_resv del BO (si no acabo ya) */
	dma_resv_lock(b->bo.base.resv, NULL);
	err = dma_resv_get_singleton(b->bo.base.resv, DMA_RESV_USAGE_KERNEL, &f);
	dma_resv_unlock(b->bo.base.resv);
	KT_EQ(err, 0);
	if (f) {
		KT_ASSERT(dma_fence_wait_timeout(f, false, 2 * HZ) > 0, "fence del movimiento");
		KT_ASSERT(dma_fence_is_signaled(f), "fence senalado tras esperar");
		dma_fence_put(f);
	}
	/* esperar al fence del movimiento */
	dma_resv_lock(b->bo.base.resv, NULL);
	err = ttm_bo_wait_ctx(&b->bo, &ctx);
	dma_resv_unlock(b->bo.base.resv);
	KT_EQ(err, 0);
	KT_ASSERT(dma_resv_test_signaled(b->bo.base.resv, DMA_RESV_USAGE_BOOKKEEP),
		  "dma_resv sin fences pendientes");
	KT_EQ(kt_bo_rw(b, 0x5555, 1), 0);
	/* y de vuelta, esta vez por evict_all (evicion segun TTM) */
	KT_EQ(kt_bo_validate(b, &kt_p_vram), 0);
	KT_EQ(kt_bo_rw(b, 0x5555, 1), 0);
	KT_EQ(ttm_resource_manager_evict_all(&kd->bdev, &kd->vram.man), 0);
	KT_EQ(kt_bo_rw(b, 0x5555, 1), 0);
	kt_bo_free(b);
	kd->async_moves = false;
}

/* ------------------------------------------------------- gpu-sched */

static void test_sched(struct kt_dev *kd)
{
	enum { N = 40 };
	struct dma_fence *fences[N];
	struct kt_job *jobs[N];
	atomic_t counter = ATOMIC_INIT(0);
	struct dma_fence *gate;
	struct kt_hwf *gw;
	int i, executed0 = atomic_read(&kd->eng.executed);

	/* 1. trabajos del mismo entity: se ejecutan en orden y todos senalan */
	for (i = 0; i < N; i++) {
		jobs[i] = kt_job_new(kd);
		KT_ASSERT(jobs[i], "kt_job_new %d", i);
		if (!jobs[i])
			return;
		jobs[i]->counter = &counter;
		/* guardar el orden esperado: lo lee el motor al ejecutar */
		jobs[i]->expected_order = i + 1;
		fences[i] = kt_job_push(jobs[i]);
	}
	for (i = 0; i < N; i++) {
		long r = dma_fence_wait_timeout(fences[i], false, 5 * HZ);

		KT_ASSERT(r > 0, "job %d no termina (%ld)", i, r);
	}
	KT_EQ(atomic_read(&counter), N);
	KT_EQ(atomic_read(&kd->eng.executed) - executed0, N);
	/* finished se senala en orden de seqno (mismo entity, FIFO) */
	for (i = 0; i < N; i++) {
		KT_ASSERT(dma_fence_is_signaled(fences[i]), "finished %d senalado", i);
		KT_EQ(fences[i]->error, 0);
		dma_fence_put(fences[i]);
	}

	/* 2. dependencia: el trabajo no corre hasta que se senala la barrera */
	gw = kzalloc(sizeof(*gw), GFP_KERNEL);
	KT_ASSERT(gw, "barrera");
	if (!gw)
		return;
	spin_lock_init(&gw->lock);
	dma_fence_init(&gw->f, &kt_hw_ops, &gw->lock, dma_fence_context_alloc(1), 1);
	gate = &gw->f;
	{
		struct kt_job *j = kt_job_new(kd);
		struct dma_fence *f;
		atomic_t c2 = ATOMIC_INIT(0);

		KT_ASSERT(j, "job con dependencia");
		if (j) {
			j->counter = &c2;
			KT_EQ(drm_sched_job_add_dependency(&j->base, dma_fence_get(gate)), 0);
			f = kt_job_push(j);
			msleep(60);
			KT_EQ(atomic_read(&c2), 0);	/* bloqueado por la barrera */
			KT_ASSERT(!dma_fence_is_signaled(f), "finished no senalado antes de tiempo");
			dma_fence_signal(gate);
			KT_ASSERT(dma_fence_wait_timeout(f, false, 5 * HZ) > 0, "job tras la barrera");
			KT_EQ(atomic_read(&c2), 1);
			dma_fence_put(f);
		}
	}
	dma_fence_put(gate);

	/* 3. hang: el motor no senala y el temporizador del planificador actua */
	{
		struct kt_job *j = kt_job_new(kd);
		struct dma_fence *f;
		int t0 = atomic_read(&kd->eng.timedout);

		KT_ASSERT(j, "job que cuelga");
		if (j) {
			atomic_set(&kd->eng.hang_next, 1);
			f = kt_job_push(j);
			KT_ASSERT(dma_fence_wait_timeout(f, false, 5 * HZ) > 0, "el job colgado termina");
			KT_ASSERT(atomic_read(&kd->eng.timedout) > t0, "timedout_job se llamo");
			/*
			 * drm_sched_stop() suelta el fence de hardware del trabajo
			 * culpable y drm_sched_start() lo da por cancelado.
			 */
			KT_EQ(f->error, -ECANCELED);
			dma_fence_put(f);
		}
	}
	/* y el planificador sigue vivo */
	{
		struct kt_job *j = kt_job_new(kd);
		struct dma_fence *f;
		atomic_t c3 = ATOMIC_INIT(0);

		if (j) {
			j->counter = &c3;
			f = kt_job_push(j);
			KT_ASSERT(dma_fence_wait_timeout(f, false, 5 * HZ) > 0, "job tras el reset");
			KT_EQ(atomic_read(&c3), 1);
			dma_fence_put(f);
		}
	}
}

/* ------------------------------------------------------- estres */

#ifdef KT_SMP
#define KT_THREADS	4
#else
#define KT_THREADS	1
#endif
#define KT_SLOTS	10
#ifdef KT_SMP
#define KT_ROUNDS	400
#else
#define KT_ROUNDS	200
#endif

struct kt_worker {
	struct kt_dev *kd;
	int id;
	u64 rng;
	atomic_t bad, done, creates, validates;
	struct task_struct *task;
};

static struct completion kt_stress_done;
static atomic_t kt_stress_left;

static int kt_stress_fn(void *data)
{
	struct kt_worker *w = data;
	struct kt_bo *slot[KT_SLOTS] = { };
	u32 seed[KT_SLOTS] = { };
	int r;

	for (r = 0; r < KT_ROUNDS; r++) {
		int i = kt_rand(&w->rng) % KT_SLOTS;
		int op = kt_rand(&w->rng) % 6;
		int err;

		if (!slot[i]) {
			size_t sz = (size_t)(1 + kt_rand(&w->rng) % 16) * 32 * 1024;
			struct ttm_placement *p = (kt_rand(&w->rng) & 1) ? &kt_p_vram : &kt_p_vram_tt;

			slot[i] = kt_bo_create(w->kd, sz, p, &err);
			if (!slot[i]) {
				if (err != -ENOMEM && err != -ENOSPC)
					atomic_inc(&w->bad);
				continue;
			}
			seed[i] = (u32)kt_rand(&w->rng);
			if (kt_bo_rw(slot[i], seed[i], 0))
				atomic_inc(&w->bad);
			atomic_inc(&w->creates);
			continue;
		}
		switch (op) {
		case 0:
			kt_bo_free(slot[i]);
			slot[i] = NULL;
			break;
		case 1:
		case 2:
			err = kt_bo_validate(slot[i], op == 1 ? &kt_p_vram : &kt_p_tt);
			if (err && err != -ENOMEM && err != -ENOSPC && err != -EBUSY)
				atomic_inc(&w->bad);
			atomic_inc(&w->validates);
			if (kt_bo_rw(slot[i], seed[i], 1))
				atomic_inc(&w->bad);
			break;
		default:
			if (kt_bo_rw(slot[i], seed[i], 1))
				atomic_inc(&w->bad);
			break;
		}
	}
	for (r = 0; r < KT_SLOTS; r++) {
		if (!slot[r])
			continue;
		if (kt_bo_rw(slot[r], seed[r], 1))
			atomic_inc(&w->bad);
		kt_bo_free(slot[r]);
	}
	atomic_set(&w->done, 1);
	if (atomic_dec_and_test(&kt_stress_left))
		complete(&kt_stress_done);
	/* esperar a que el principal nos recoja */
	while (!kthread_should_stop())
		msleep(5);
	return 0;
}

static void test_stress(struct kt_dev *kd, bool async)
{
	struct kt_worker *w;
	int i, bad = 0;

	w = kcalloc(KT_THREADS, sizeof(*w), GFP_KERNEL);
	KT_ASSERT(w, "kcalloc");
	if (!w)
		return;
	kd->async_moves = async;
	init_completion(&kt_stress_done);
	atomic_set(&kt_stress_left, KT_THREADS);
	for (i = 0; i < KT_THREADS; i++) {
		w[i].kd = kd;
		w[i].id = i;
		w[i].rng = 0x9E3779B97F4A7C15ULL * (i + 1 + (async ? 100 : 0));
		w[i].task = kthread_run(kt_stress_fn, &w[i], "kt_ttm%d", i);
		KT_ASSERT(!IS_ERR(w[i].task), "kthread_run");
	}
	if (!wait_for_completion_timeout(&kt_stress_done, 120 * HZ))
		KT_ASSERT(0, "estres %s: hilos sin terminar", async ? "async" : "sync");
	for (i = 0; i < KT_THREADS; i++) {
		if (IS_ERR(w[i].task))
			continue;
		kthread_stop(w[i].task);
		bad += atomic_read(&w[i].bad);
	}
	KT_EQ(bad, 0);
	{
		int creates = 0, validates = 0;

		for (i = 0; i < KT_THREADS; i++) {
			creates += atomic_read(&w[i].creates);
			validates += atomic_read(&w[i].validates);
		}
		KT_ASSERT(creates > 20 && validates > 10, "estres hizo trabajo: %d creados, %d validate",
			  creates, validates);
	}
	kd->async_moves = false;
	kfree(w);
	/* sin BO vivos, VRAM entera */
	KT_EQ(atomic_read(&kd->bos), 0);
	KT_EQ(kd->vram.mm.avail, KT_VRAM_SIZE);
	KT_EQ(kd->vram.man.usage, 0);
}

/* ------------------------------------------------------- GEM shmem */

static void test_gem_shmem(struct kt_dev *kd)
{
	enum { N = 32 };
	struct drm_gem_shmem_object *o[N] = { };
	int i, bad = 0;
	u64 rng = 0x1234567;

	for (i = 0; i < N; i++) {
		size_t sz = PAGE_SIZE * (1 + kt_rand(&rng) % 16);
		struct iosys_map m;

		o[i] = drm_gem_shmem_create(&kd->drm, sz);
		if (IS_ERR(o[i])) {
			o[i] = NULL;
			bad++;
			continue;
		}
		if (drm_gem_vmap(&o[i]->base, &m)) {
			bad++;
			continue;
		}
		memset(m.vaddr, i, sz);
		((u32 *)m.vaddr)[sz / 4 - 1] = 0xc0ffee00 + i;
		drm_gem_vunmap(&o[i]->base, &m);
	}
	KT_EQ(bad, 0);
	/* releer en orden inverso tras haber creado todos */
	for (i = N - 1; i >= 0; i--) {
		struct iosys_map m;

		if (!o[i])
			continue;
		if (drm_gem_vmap(&o[i]->base, &m)) {
			bad++;
			continue;
		}
		if (((u8 *)m.vaddr)[0] != (u8)i || ((u32 *)m.vaddr)[o[i]->base.size / 4 - 1] != 0xc0ffee00 + i)
			bad++;
		drm_gem_vunmap(&o[i]->base, &m);
		drm_gem_object_put(&o[i]->base);
	}
	KT_EQ(bad, 0);
}

/* ------------------------------------------------------- shrinker de TTM */

static void test_ttm_shrinker(struct kt_dev *kd)
{
	struct kt_bo *bos[8];
	int i, n = 0, err;
	unsigned long freed;

	/* paginas WC: al liberarse vuelven al pool global que recorre el shrinker */
	kd->tt_caching = ttm_write_combined;
	for (i = 0; i < 8; i++) {
		bos[n] = kt_bo_create(kd, 256 * 1024, &kt_p_tt, &err);
		if (!bos[n])
			break;
		KT_EQ(kt_bo_rw(bos[n], 0x600 + i, 0), 0);
		n++;
	}
	KT_EQ(n, 8);
	for (i = 0; i < n; i++)
		KT_EQ(kt_bo_rw(bos[i], 0x600 + i, 1), 0);
	for (i = 0; i < n; i++)
		kt_bo_free(bos[i]);
	/* el shrinker global de la capa recorre al de TTM y suelta las del pool */
	{
		unsigned long total = 0;

		do {
			freed = lkpi_shrink_slab(GFP_KERNEL, 1024);
			total += freed;
		} while (freed);
		/* los 8 BO eran 8 x 64 paginas, todas WC y todas vuelvieron al pool */
		KT_ASSERT(total >= 8 * 64, "lkpi_shrink_slab libero %lu paginas", total);
	}
	KT_EQ(lkpi_shrink_slab(GFP_KERNEL, 1024), 0);	/* y ya no queda nada que soltar */
	kd->tt_caching = ttm_cached;
}


#ifdef KT_DEVICE
/* --------------------------------------------------- /dev/lkpi_ttm (L6) */

#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include "lkpi_test_ttm_uapi.h"

struct ktd_file {
	struct mutex lock;
	struct idr bos;			/* handle -> struct kt_bo */
	struct kt_bo *ballast[64];
	unsigned int nballast;
};

static struct ttm_placement *ktd_placement(u32 dom)
{
	switch (dom) {
	case LKPI_TTM_DOM_VRAM:
		return &kt_p_vram;
	case LKPI_TTM_DOM_GTT:
		return &kt_p_tt;
	case LKPI_TTM_DOM_VRAM_GTT:
		return &kt_p_vram_tt;
	}
	return NULL;
}

static int ktd_open(struct inode *inode, struct file *file)
{
	struct ktd_file *f = kzalloc(sizeof(*f), GFP_KERNEL);

	if (!f)
		return -ENOMEM;
	mutex_init(&f->lock);
	idr_init(&f->bos);
	file->private_data = f;
	/* como drm_open(): los PTE de los BO cuelgan del address_space del dispositivo */
	file->f_mapping = kt->drm.anon_inode->i_mapping;
	return 0;
}

static int ktd_release(struct inode *inode, struct file *file)
{
	struct ktd_file *f = file->private_data;
	struct kt_bo *b;
	int id;
	unsigned int i;

	idr_for_each_entry(&f->bos, b, id)
		kt_bo_free(b);
	idr_destroy(&f->bos);
	for (i = 0; i < f->nballast; i++)
		kt_bo_free(f->ballast[i]);
	kfree(f);
	return 0;
}

static struct kt_bo *ktd_lookup(struct ktd_file *f, u32 handle)
{
	return idr_find(&f->bos, handle);
}

static long ktd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ktd_file *f = file->private_data;
	void __user *uarg = (void __user *)arg;
	long ret = 0;

	mutex_lock(&f->lock);
	switch (cmd) {
	case LKPI_TTM_IOC_CREATE: {
		struct lkpi_ttm_create c;
		struct ttm_placement *p;
		struct kt_bo *b;
		int err, id;

		if (copy_from_user(&c, uarg, sizeof(c))) {
			ret = -EFAULT;
			break;
		}
		p = ktd_placement(c.domain);
		if (!p || !c.size || c.size > (1u << 20)) {
			ret = -EINVAL;
			break;
		}
		b = kt_bo_create_type(kt, PAGE_ALIGN(c.size), p, ttm_bo_type_device, &err);
		if (!b) {
			ret = err;
			break;
		}
		id = idr_alloc(&f->bos, b, 1, 0, GFP_KERNEL);
		if (id < 0) {
			kt_bo_free(b);
			ret = id;
			break;
		}
		c.handle = id;
		c.offset = drm_vma_node_offset_addr(&b->bo.base.vma_node);
		if (copy_to_user(uarg, &c, sizeof(c)))
			ret = -EFAULT;
		break;
	}
	case LKPI_TTM_IOC_MOVE: {
		struct lkpi_ttm_move m;
		struct ttm_placement *p;
		struct kt_bo *b;

		if (copy_from_user(&m, uarg, sizeof(m))) {
			ret = -EFAULT;
			break;
		}
		b = ktd_lookup(f, m.handle);
		p = ktd_placement(m.domain);
		ret = (!b || !p) ? -EINVAL : kt_bo_validate(b, p);
		break;
	}
	case LKPI_TTM_IOC_INFO: {
		struct lkpi_ttm_info i;
		struct kt_bo *b;

		if (copy_from_user(&i, uarg, sizeof(i))) {
			ret = -EFAULT;
			break;
		}
		b = ktd_lookup(f, i.handle);
		if (!b) {
			ret = -EINVAL;
			break;
		}
		dma_resv_lock(b->bo.base.resv, NULL);
		i.mem_type = b->bo.resource ? b->bo.resource->mem_type : 0;
		dma_resv_unlock(b->bo.base.resv);
		i.usage_vram = kt->vram.man.usage;
		i.size = b->bo.base.size;
		if (copy_to_user(uarg, &i, sizeof(i)))
			ret = -EFAULT;
		break;
	}
	case LKPI_TTM_IOC_FILL:
	case LKPI_TTM_IOC_CHECK: {
		struct lkpi_ttm_pattern pt;
		struct kt_bo *b;

		if (copy_from_user(&pt, uarg, sizeof(pt))) {
			ret = -EFAULT;
			break;
		}
		b = ktd_lookup(f, pt.handle);
		if (!b) {
			ret = -EINVAL;
			break;
		}
		pt.bad = kt_bo_rw(b, pt.seed, cmd == LKPI_TTM_IOC_CHECK);
		if (cmd == LKPI_TTM_IOC_CHECK && copy_to_user(uarg, &pt, sizeof(pt)))
			ret = -EFAULT;
		break;
	}
	case LKPI_TTM_IOC_DESTROY: {
		u32 h;
		struct kt_bo *b;

		if (copy_from_user(&h, uarg, sizeof(h))) {
			ret = -EFAULT;
			break;
		}
		b = idr_remove(&f->bos, h);
		if (!b)
			ret = -EINVAL;
		else
			kt_bo_free(b);
		break;
	}
	case LKPI_TTM_IOC_PRESSURE: {
		struct lkpi_ttm_pressure pr;
		unsigned int i;

		if (copy_from_user(&pr, uarg, sizeof(pr))) {
			ret = -EFAULT;
			break;
		}
		for (i = 0; i < f->nballast; i++)
			kt_bo_free(f->ballast[i]);
		f->nballast = 0;
		for (i = 0; i < pr.count && i < ARRAY_SIZE(f->ballast); i++) {
			int err;
			struct kt_bo *b = kt_bo_create(kt, 256 * 1024, &kt_p_vram, &err);

			if (!b) {
				ret = err;
				break;
			}
			f->ballast[f->nballast++] = b;
		}
		break;
	}
	default:
		ret = -ENOTTY;
	}
	mutex_unlock(&f->lock);
	return ret;
}

static int ktd_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct drm_vma_offset_node *node;
	struct ttm_buffer_object *bo = NULL;
	int ret;

	drm_vma_offset_lock_lookup(kt->bdev.vma_manager);
	node = drm_vma_offset_exact_lookup_locked(kt->bdev.vma_manager, vma->vm_pgoff,
						  vma_pages(vma));
	if (node) {
		bo = container_of(node, struct ttm_buffer_object, base.vma_node);
		if (!kref_get_unless_zero(&bo->kref))
			bo = NULL;
	}
	drm_vma_offset_unlock_lookup(kt->bdev.vma_manager);
	if (!bo)
		return -EINVAL;
	ret = ttm_bo_mmap_obj(vma, bo);
	ttm_bo_put(bo);
	return ret;
}

static const struct file_operations ktd_fops = {
	.owner = THIS_MODULE,
	.open = ktd_open,
	.release = ktd_release,
	.unlocked_ioctl = ktd_ioctl,
	.mmap = ktd_mmap,
};

static struct miscdevice ktd_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "lkpi_ttm",
	.fops = &ktd_fops,
	.mode = 0666,
};

static int __init test_ttm_dev_init(void)
{
	int ret = kt_setup();

	if (!ret)
		ret = misc_register(&ktd_misc);
	if (ret) {
		kt_teardown();
		return ret;
	}
	return 0;
}

static void __exit test_ttm_dev_exit(void)
{
	misc_deregister(&ktd_misc);
	kt_teardown();
}

module_init(test_ttm_dev_init);
module_exit(test_ttm_dev_exit);
MODULE_LICENSE("GPL");
#else

/* --------------------------------------------------------------- init */

static int __init test_ttm_init(void)
{
	int ret = kt_setup();

	KT_EQ(ret, 0);
	if (ret)
		goto out;
	pr_info("lkpi_test_ttm: %s\n", "test_buddy()");
	test_buddy();
	pr_info("lkpi_test_ttm: %s\n", "test_bo_basic(kt)");
	test_bo_basic(kt);
	pr_info("lkpi_test_ttm: %s\n", "test_eviction(kt)");
	test_eviction(kt);
	pr_info("lkpi_test_ttm: %s\n", "test_swap(kt)");
	test_swap(kt);
	pr_info("lkpi_test_ttm: %s\n", "test_async_moves(kt)");
	test_async_moves(kt);
	pr_info("lkpi_test_ttm: %s\n", "test_sched(kt)");
	test_sched(kt);
	pr_info("lkpi_test_ttm: %s\n", "test_gem_shmem(kt)");
	test_gem_shmem(kt);
	pr_info("lkpi_test_ttm: %s\n", "test_ttm_shrinker(kt)");
	test_ttm_shrinker(kt);
	pr_info("lkpi_test_ttm: %s\n", "test_stress(kt, false)");
	test_stress(kt, false);
	pr_info("lkpi_test_ttm: %s\n", "test_stress(kt, true)");
	test_stress(kt, true);
	KT_EQ(atomic_read(&kt->bos), 0);
	pr_info("lkpi_test_ttm: %d movimientos sync, %d por el planificador, %d nulos\n",
		atomic_read(&kt->moves_sync), atomic_read(&kt->moves_async),
		atomic_read(&kt->moves_null));
out:
	kt_teardown();
	return 0;
}

static void __exit test_ttm_exit(void)
{
}

module_init(test_ttm_init);
module_exit(test_ttm_exit);
MODULE_LICENSE("GPL");
#endif /* KT_DEVICE */
