// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pruebas de L5 en el kernel (K7): dma_fence (senal, callbacks, error, espera,
 * stub, orden de seqno), dma_fence_chain, dma_fence_array, unwrap/merge,
 * dma_resv (usos, iteradores, singleton, copia, reemplazo, espera), sync_file
 * y el enlace con los fences de Aurora en los dos sentidos. El fence implicito
 * de los dmabuf (necesita un proceso con fds) lo prueba lkpi_test_dev.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-array.h>
#include <linux/dma-fence-chain.h>
#include <linux/dma-fence-unwrap.h>
#include <linux/dma-resv.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include "lkpi_test.h"

/* Servicios de Aurora que usa la prueba (lkpi_test.h no los trae) */
struct aurora_fence;
struct aurora_fence *kt_af_create(int signaled) __asm__("aur_fence_create");
void kt_af_put(struct aurora_fence *f) __asm__("aur_fence_put");
void kt_af_signal(struct aurora_fence *f) __asm__("aur_fence_signal");
bool kt_af_signaled(struct aurora_fence *f) __asm__("aur_fence_signaled");

struct kt_fence {
	struct dma_fence base;
	spinlock_t lock;
	int released;
};

static const char *kt_drv_name(struct dma_fence *f) { return "kt_k7"; }
static const char *kt_tl_name(struct dma_fence *f) { return "kt_timeline"; }
static bool kt_enable(struct dma_fence *f) { return true; }

static int kt_nreleased, kt_nalloc;

static void kt_release(struct dma_fence *f)
{
	struct kt_fence *kf = container_of(f, struct kt_fence, base);

	kt_nreleased++;
	dma_fence_free(&kf->base);
}

static const struct dma_fence_ops kt_ops = {
	.get_driver_name = kt_drv_name,
	.get_timeline_name = kt_tl_name,
	.enable_signaling = kt_enable,
	.release = kt_release,
};

static struct kt_fence *kt_new(u64 ctx, u64 seqno)
{
	struct kt_fence *kf = kzalloc(sizeof(*kf), GFP_KERNEL);

	if (!kf)
		return NULL;
	spin_lock_init(&kf->lock);
	dma_fence_init(&kf->base, &kt_ops, &kf->lock, ctx, seqno);
	kt_nalloc++;
	return kf;
}

static int kt_cb_count;
static struct dma_fence *kt_cb_last;

static void kt_cb(struct dma_fence *f, struct dma_fence_cb *cb)
{
	kt_cb_count++;
	kt_cb_last = f;
}

/* ------------------------------------------------------ dma_fence basico */

static void test_fence_basic(void)
{
	struct kt_fence *kf = kt_new(dma_fence_context_alloc(1), 1);
	struct dma_fence_cb cb1, cb2;
	struct dma_fence *f;
	int rel0;

	KT_ASSERT(kf != NULL, "kzalloc");
	if (!kf)
		return;
	f = &kf->base;
	KT_ASSERT(!dma_fence_is_signaled(f), "nace sin senalar");
	kt_cb_count = 0;
	KT_EQ(dma_fence_add_callback(f, &cb1, kt_cb), 0);
	KT_EQ(dma_fence_add_callback(f, &cb2, kt_cb), 0);
	KT_ASSERT(dma_fence_remove_callback(f, &cb2), "quitar callback pendiente");
	KT_ASSERT(!dma_fence_remove_callback(f, &cb2), "quitarlo otra vez no hace nada");
	KT_EQ(dma_fence_get_status(f), 0);
	KT_STREQ((const char *)dma_fence_driver_name(f), "kt_k7");
	KT_STREQ((const char *)dma_fence_timeline_name(f), "kt_timeline");
	KT_EQ(dma_fence_signal(f), 0);
	KT_EQ(kt_cb_count, 1);
	KT_ASSERT(kt_cb_last == f, "el callback recibe el fence");
	KT_ASSERT(dma_fence_is_signaled(f), "senalado");
	KT_EQ(dma_fence_signal(f), -EINVAL);	/* ya senalado */
	KT_EQ(kt_cb_count, 1);
	KT_EQ(dma_fence_add_callback(f, &cb2, kt_cb), -ENOENT);
	KT_EQ(dma_fence_get_status(f), 1);
	KT_ASSERT(ktime_to_ns(dma_fence_timestamp(f)) != 0, "marca de tiempo");
	/* senalado, el fence se "desprende" del driver (Linux) */
	KT_STREQ((const char *)dma_fence_driver_name(f), "detached-driver");
	rel0 = kt_nreleased;
	dma_fence_put(f);
	KT_EQ(kt_nreleased - rel0, 1);
}

static void test_fence_error(void)
{
	struct kt_fence *kf = kt_new(dma_fence_context_alloc(1), 1);

	if (!kf) {
		KT_ASSERT(0, "kzalloc");
		return;
	}
	dma_fence_set_error(&kf->base, -EIO);
	KT_EQ(dma_fence_get_status(&kf->base), 0);	/* aun sin senalar */
	dma_fence_signal(&kf->base);
	KT_EQ(dma_fence_get_status(&kf->base), -EIO);
	dma_fence_put(&kf->base);
}

static void test_fence_order(void)
{
	u64 c1 = dma_fence_context_alloc(2), c2 = dma_fence_context_alloc(1);
	struct kt_fence *a = kt_new(c1, 5), *b = kt_new(c1, 6), *w1 = kt_new(c1, 0xFFFFFFFFull);
	struct kt_fence *w2 = kt_new(c1, 0x100000000ull + 1);

	KT_ASSERT(c2 >= c1 + 2, "los contextos no se solapan");
	if (!a || !b || !w1 || !w2) {
		KT_ASSERT(0, "kzalloc");
		return;
	}
	KT_ASSERT(dma_fence_is_later(&b->base, &a->base), "6 es posterior a 5");
	KT_ASSERT(!dma_fence_is_later(&a->base, &b->base), "5 no es posterior a 6");
	KT_ASSERT(dma_fence_is_later_or_same(&a->base, &a->base), "igual");
	/* seqno de 32 bits con vuelta: 0x100000001 (bajo 1) es posterior a 0xFFFFFFFF */
	KT_ASSERT(dma_fence_is_later(&w2->base, &w1->base), "vuelta de 32 bits");
	KT_ASSERT(dma_fence_later(&a->base, &b->base) == &b->base, "later = b");
	dma_fence_signal(&b->base);
	KT_ASSERT(dma_fence_later(&a->base, &b->base) == NULL, "later: el posterior ya esta senalado");
	dma_fence_put(&a->base);
	dma_fence_put(&b->base);
	dma_fence_put(&w1->base);
	dma_fence_put(&w2->base);
}

static void test_fence_stub(void)
{
	struct dma_fence *s1 = dma_fence_get_stub(), *s2 = dma_fence_get_stub();
	struct dma_fence *p = dma_fence_allocate_private_stub(ktime_get());

	KT_ASSERT(s1 && s1 == s2, "un unico stub");
	KT_ASSERT(dma_fence_is_signaled(s1), "el stub esta senalado");
	KT_ASSERT(p && p != s1 && dma_fence_is_signaled(p), "stub privado senalado");
	dma_fence_put(s1);
	dma_fence_put(s2);
	dma_fence_put(p);
}

/* ---------------------------------------------------------------- espera */

static int kt_signal_later(void *data)
{
	struct dma_fence *f = data;

	msleep(30);
	dma_fence_signal(f);
	dma_fence_put(f);
	return 0;
}

static void test_fence_wait(void)
{
	struct kt_fence *kf = kt_new(dma_fence_context_alloc(1), 1);
	struct task_struct *t;
	signed long r;

	if (!kf) {
		KT_ASSERT(0, "kzalloc");
		return;
	}
	r = dma_fence_wait_timeout(&kf->base, false, msecs_to_jiffies(10));
	KT_EQ(r, 0);			/* vence sin senalar */
	KT_ASSERT(!dma_fence_is_signaled(&kf->base), "sigue sin senalar");
	dma_fence_get(&kf->base);
	t = kthread_run(kt_signal_later, &kf->base, "kt_k7_sig");
	KT_ASSERT(!IS_ERR(t), "kthread");
	r = dma_fence_wait_timeout(&kf->base, true, msecs_to_jiffies(2000));
	KT_ASSERT(r > 0, "espera cortada por la senal (r=%ld)", r);
	KT_EQ(dma_fence_wait(&kf->base, false), 0);	/* ya senalado */
	if (!IS_ERR(t))
		kthread_stop(t);	/* libera el hilo de la capa */
	dma_fence_put(&kf->base);
}

static void test_fence_wait_any(void)
{
	struct kt_fence *a = kt_new(dma_fence_context_alloc(1), 1);
	struct kt_fence *b = kt_new(dma_fence_context_alloc(1), 1);
	struct dma_fence *fs[2];
	uint32_t idx = 99;
	signed long r;

	if (!a || !b) {
		KT_ASSERT(0, "kzalloc");
		return;
	}
	fs[0] = &a->base;
	fs[1] = &b->base;
	r = dma_fence_wait_any_timeout(fs, 2, false, msecs_to_jiffies(10), &idx);
	KT_EQ(r, 0);
	dma_fence_signal(&b->base);
	r = dma_fence_wait_any_timeout(fs, 2, false, msecs_to_jiffies(500), &idx);
	KT_ASSERT(r > 0 && idx == 1, "wait_any: r=%ld idx=%u", r, idx);
	dma_fence_put(&a->base);
	dma_fence_put(&b->base);
}

/* ----------------------------------------------------------------- chain */

static void test_chain(void)
{
	u64 ctx = dma_fence_context_alloc(1);
	struct kt_fence *f1 = kt_new(ctx, 1), *f2 = kt_new(ctx, 2), *f3 = kt_new(ctx, 3);
	struct dma_fence_chain *c1, *c2, *c3;
	struct dma_fence *head = NULL, *it, *found;
	int n = 0;

	if (!f1 || !f2 || !f3) {
		KT_ASSERT(0, "kzalloc");
		return;
	}
	c1 = dma_fence_chain_alloc();
	c2 = dma_fence_chain_alloc();
	c3 = dma_fence_chain_alloc();
	KT_ASSERT(c1 && c2 && c3, "chain alloc");
	if (!c1 || !c2 || !c3)
		return;
	dma_fence_chain_init(c1, NULL, &f1->base, 1);
	dma_fence_chain_init(c2, &c1->base, &f2->base, 2);
	dma_fence_chain_init(c3, &c2->base, &f3->base, 3);
	head = &c3->base;
	KT_ASSERT(dma_fence_is_chain(head), "es chain");
	KT_ASSERT(to_dma_fence_chain(head) == c3, "to_dma_fence_chain");
	KT_ASSERT(dma_fence_chain_contained(head) == &f3->base, "contained");
	KT_ASSERT(dma_fence_chain_contained(&f1->base) == &f1->base, "contained de un no-chain");

	dma_fence_chain_for_each(it, head)
		n++;
	KT_EQ(n, 3);

	found = dma_fence_get(head);
	KT_EQ(dma_fence_chain_find_seqno(&found, 2), 0);
	KT_ASSERT(found == &c2->base, "find_seqno 2");
	dma_fence_put(found);
	found = dma_fence_get(head);
	KT_EQ(dma_fence_chain_find_seqno(&found, 4), -EINVAL);	/* aun no existe */
	dma_fence_put(found);

	/* el chain se senala en orden: 3 no antes que 1 y 2 */
	KT_ASSERT(!dma_fence_is_signaled(head), "chain sin senalar");
	dma_fence_signal(&f3->base);
	KT_ASSERT(!dma_fence_is_signaled(head), "falta la cola");
	dma_fence_signal(&f1->base);
	dma_fence_signal(&f2->base);
	KT_ASSERT(dma_fence_wait_timeout(head, false, msecs_to_jiffies(500)) > 0 ||
		  dma_fence_is_signaled(head), "chain senalado al completar todo");
	/* dma_fence_chain_init se queda con las referencias de f1..f3 y de prev */
	dma_fence_put(head);
}

/* ----------------------------------------------------------------- array */

static struct dma_fence_array *kt_make_array(struct kt_fence **fs, int n, bool any)
{
	struct dma_fence **arr = kmalloc_array(n, sizeof(*arr), GFP_KERNEL);
	struct dma_fence_array *a;
	int i;

	if (!arr)
		return NULL;
	for (i = 0; i < n; i++)
		arr[i] = dma_fence_get(&fs[i]->base);
	a = dma_fence_array_create(n, arr, dma_fence_context_alloc(1), 1, any);
	if (!a) {
		for (i = 0; i < n; i++)
			dma_fence_put(arr[i]);
		kfree(arr);
	}
	return a;
}

static void test_array(void)
{
	struct kt_fence *f[3];
	struct dma_fence_array *all, *any;
	struct dma_fence *it;
	unsigned int idx, n = 0;
	int i;

	for (i = 0; i < 3; i++) {
		f[i] = kt_new(dma_fence_context_alloc(1), 1);
		if (!f[i]) {
			KT_ASSERT(0, "kzalloc");
			return;
		}
	}
	all = kt_make_array(f, 3, false);
	any = kt_make_array(f, 3, true);
	KT_ASSERT(all && any, "array create");
	if (!all || !any)
		return;
	KT_ASSERT(dma_fence_is_array(&all->base), "es array");
	KT_ASSERT(to_dma_fence_array(&all->base) == all, "to_dma_fence_array");
	KT_ASSERT(dma_fence_match_context(&f[0]->base, f[0]->base.context), "match_context");
	dma_fence_array_for_each(it, idx, &all->base)
		n++;
	KT_EQ(n, 3);
	dma_fence_enable_sw_signaling(&all->base);
	dma_fence_enable_sw_signaling(&any->base);
	KT_ASSERT(!dma_fence_is_signaled(&all->base) && !dma_fence_is_signaled(&any->base),
		  "ambos sin senalar");
	dma_fence_signal(&f[1]->base);
	msleep(20);		/* el array se senala desde irq_work */
	KT_ASSERT(dma_fence_is_signaled(&any->base), "any: basta uno");
	KT_ASSERT(!dma_fence_is_signaled(&all->base), "all: faltan dos");
	dma_fence_signal(&f[0]->base);
	dma_fence_signal(&f[2]->base);
	msleep(20);
	KT_ASSERT(dma_fence_is_signaled(&all->base), "all: completo");
	dma_fence_put(&all->base);
	dma_fence_put(&any->base);
	for (i = 0; i < 3; i++)
		dma_fence_put(&f[i]->base);
}

static void test_unwrap(void)
{
	u64 ctx = dma_fence_context_alloc(1);
	struct kt_fence *f1 = kt_new(ctx, 1), *f2 = kt_new(ctx + 0, 2);
	struct kt_fence *f3 = kt_new(dma_fence_context_alloc(1), 1);
	struct kt_fence *fs[2];
	struct dma_fence_array *arr;
	struct dma_fence_unwrap uw;
	struct dma_fence *it, *merged;
	int n = 0;

	if (!f1 || !f2 || !f3) {
		KT_ASSERT(0, "kzalloc");
		return;
	}
	fs[0] = f1;
	fs[1] = f3;
	arr = kt_make_array(fs, 2, false);
	KT_ASSERT(arr != NULL, "array");
	if (!arr)
		return;
	dma_fence_unwrap_for_each(it, &uw, &arr->base)
		n++;
	KT_EQ(n, 2);
	n = 0;
	dma_fence_unwrap_for_each(it, &uw, &f2->base)
		n++;
	KT_EQ(n, 1);				/* un fence suelto se recorre a si mismo */
	/* merge de {array(f1,f3), f2}: f1 y f2 son del mismo contexto, queda el posterior */
	merged = dma_fence_unwrap_merge(&arr->base, &f2->base);
	KT_ASSERT(merged != NULL, "merge");
	if (merged) {
		n = 0;
		dma_fence_unwrap_for_each(it, &uw, merged)
			n++;
		KT_EQ(n, 2);			/* contexto de f1/f2 (f2) y el de f3 */
		dma_fence_put(merged);
	}
	dma_fence_put(&arr->base);
	dma_fence_put(&f1->base);
	dma_fence_put(&f2->base);
	dma_fence_put(&f3->base);
}

/* --------------------------------------------------------------- dma_resv */

static void test_resv(void)
{
	struct dma_resv resv, copy;
	struct dma_resv_iter cursor;
	struct dma_fence *fence, *single, **fences;
	struct kt_fence *w = kt_new(dma_fence_context_alloc(1), 1);
	struct kt_fence *r = kt_new(dma_fence_context_alloc(1), 1);
	struct kt_fence *k = kt_new(dma_fence_context_alloc(1), 1);
	unsigned int nfences = 0, i;
	int n = 0;

	if (!w || !r || !k) {
		KT_ASSERT(0, "kzalloc");
		return;
	}
	dma_resv_init(&resv);
	KT_ASSERT(dma_resv_test_signaled(&resv, DMA_RESV_USAGE_BOOKKEEP), "vacio = senalado");
	KT_ASSERT(!dma_resv_is_locked(&resv), "sin cerrojo");
	KT_EQ(dma_resv_lock(&resv, NULL), 0);
	KT_ASSERT(dma_resv_is_locked(&resv), "con cerrojo");
	KT_EQ(dma_resv_reserve_fences(&resv, 3), 0);
	dma_resv_add_fence(&resv, &w->base, DMA_RESV_USAGE_WRITE);
	dma_resv_add_fence(&resv, &r->base, DMA_RESV_USAGE_READ);
	dma_resv_add_fence(&resv, &k->base, DMA_RESV_USAGE_KERNEL);

	/* un uso incluye los anteriores: WRITE ve KERNEL y WRITE */
	dma_resv_for_each_fence(&cursor, &resv, DMA_RESV_USAGE_WRITE, fence)
		n++;
	KT_EQ(n, 2);
	n = 0;
	dma_resv_for_each_fence(&cursor, &resv, DMA_RESV_USAGE_READ, fence)
		n++;
	KT_EQ(n, 3);
	dma_resv_iter_end(&cursor);
	dma_resv_unlock(&resv);

	n = 0;
	dma_resv_iter_begin(&cursor, &resv, DMA_RESV_USAGE_BOOKKEEP);
	dma_resv_for_each_fence_unlocked(&cursor, fence)
		n++;
	dma_resv_iter_end(&cursor);
	KT_EQ(n, 3);

	KT_ASSERT(!dma_resv_test_signaled(&resv, DMA_RESV_USAGE_WRITE), "escritura pendiente");
	KT_EQ(dma_resv_wait_timeout(&resv, DMA_RESV_USAGE_WRITE, false, msecs_to_jiffies(10)), 0);
	KT_EQ(dma_resv_get_fences(&resv, DMA_RESV_USAGE_READ, &nfences, &fences), 0);
	KT_EQ(nfences, 3);
	for (i = 0; i < nfences; i++)
		dma_fence_put(fences[i]);
	kfree(fences);
	KT_EQ(dma_resv_get_singleton(&resv, DMA_RESV_USAGE_READ, &single), 0);
	KT_ASSERT(single != NULL && dma_fence_is_array(single), "singleton de varios = array");
	dma_fence_put(single);

	dma_resv_init(&copy);
	KT_EQ(dma_resv_copy_fences(&copy, &resv), 0);
	KT_ASSERT(!dma_resv_test_signaled(&copy, DMA_RESV_USAGE_READ), "copia con los fences");

	dma_fence_signal(&k->base);
	dma_fence_signal(&w->base);
	KT_ASSERT(dma_resv_test_signaled(&resv, DMA_RESV_USAGE_WRITE), "KERNEL y WRITE senalados");
	KT_ASSERT(!dma_resv_test_signaled(&resv, DMA_RESV_USAGE_READ), "falta la lectura");
	dma_fence_signal(&r->base);
	KT_ASSERT(dma_resv_wait_timeout(&resv, DMA_RESV_USAGE_BOOKKEEP, false,
					msecs_to_jiffies(100)) > 0, "espera de todos");
	KT_EQ(dma_resv_get_singleton(&resv, DMA_RESV_USAGE_READ, &single), 0);
	KT_ASSERT(single == NULL || dma_fence_is_signaled(single), "singleton senalado");
	dma_fence_put(single);

	/* reemplazar: mismo contexto que w, otro fence */
	{
		struct kt_fence *w2 = kt_new(w->base.context, 2);

		if (w2) {
			dma_resv_lock(&resv, NULL);
			dma_resv_replace_fences(&resv, w->base.context, &w2->base,
						DMA_RESV_USAGE_WRITE);
			dma_resv_unlock(&resv);
			KT_ASSERT(!dma_resv_test_signaled(&resv, DMA_RESV_USAGE_WRITE),
				  "replace deja el nuevo");
			dma_fence_signal(&w2->base);
			dma_fence_put(&w2->base);
		}
	}
	dma_resv_fini(&copy);
	dma_resv_fini(&resv);
	dma_fence_put(&w->base);
	dma_fence_put(&r->base);
	dma_fence_put(&k->base);
}

/* ------------------------------------------------------------- sync_file */

static void test_sync_file(void)
{
	struct kt_fence *kf = kt_new(dma_fence_context_alloc(1), 1);
	struct sync_file *sf;

	if (!kf) {
		KT_ASSERT(0, "kzalloc");
		return;
	}
	sf = sync_file_create(&kf->base);
	KT_ASSERT(sf != NULL, "sync_file_create");
	if (sf) {
		KT_ASSERT(sf->fence == &kf->base, "sync_file guarda el fence");
		KT_ASSERT(sf->file != NULL, "tiene struct file");
		if (sf->file)
			fput(sf->file);
	}
	dma_fence_put(&kf->base);
}

/* ----------------------------------------------- enlace con Aurora */

static void test_aurora_bridge(void)
{
	struct kt_fence *kf = kt_new(dma_fence_context_alloc(1), 1);
	struct aurora_fence *af, *af2;
	struct dma_fence *wrapped;
	struct dma_fence_cb cb;

	if (!kf) {
		KT_ASSERT(0, "kzalloc");
		return;
	}
	/* dma_fence -> aurora_fence */
	af = lkpi_dma_fence_to_aurora(&kf->base);
	KT_ASSERT(af != NULL && !kt_af_signaled(af), "aurora_fence sin senalar");
	dma_fence_signal(&kf->base);
	KT_ASSERT(kt_af_signaled(af), "senalar el dma_fence senala el de Aurora");
	kt_af_put(af);
	af = lkpi_dma_fence_to_aurora(&kf->base);
	KT_ASSERT(af != NULL && kt_af_signaled(af), "dma_fence ya senalado -> senalado");
	kt_af_put(af);
	dma_fence_put(&kf->base);

	/* aurora_fence -> dma_fence */
	af2 = kt_af_create(0);
	KT_ASSERT(af2 != NULL, "aur_fence_create");
	if (!af2)
		return;
	wrapped = lkpi_dma_fence_from_aurora(af2);
	KT_ASSERT(wrapped != NULL, "envoltorio");
	if (wrapped) {
		kt_cb_count = 0;
		KT_ASSERT(!dma_fence_is_signaled(wrapped), "envoltorio sin senalar");
		KT_EQ(dma_fence_add_callback(wrapped, &cb, kt_cb), 0);
		KT_EQ(kt_cb_count, 0);
		kt_af_signal(af2);
		KT_ASSERT(dma_fence_is_signaled(wrapped), "senalar el de Aurora senala el envoltorio");
		KT_EQ(kt_cb_count, 1);
		dma_fence_put(wrapped);
	}
	kt_af_put(af2);

	/* envoltorio de uno ya senalado y espera de un hilo */
	af2 = kt_af_create(1);
	wrapped = af2 ? lkpi_dma_fence_from_aurora(af2) : NULL;
	KT_ASSERT(wrapped && dma_fence_is_signaled(wrapped), "envoltorio de uno senalado");
	dma_fence_put(wrapped);
	kt_af_put(af2);
}

#define KT_RUN(fn) \
	do { \
		int __before = kt_nalloc - kt_nreleased; \
		fn(); \
		msleep(50);	/* irq_work y kfree_rcu pendientes */ \
		KT_ASSERT(kt_nalloc - kt_nreleased == __before, \
			  #fn ": %d fences sin liberar", kt_nalloc - kt_nreleased - __before); \
	} while (0)

static int __init test_k7_init(void)
{
	KT_RUN(test_fence_basic);
	KT_RUN(test_fence_error);
	KT_RUN(test_fence_order);
	KT_RUN(test_fence_stub);
	KT_RUN(test_fence_wait);
	KT_RUN(test_fence_wait_any);
	KT_RUN(test_chain);
	KT_RUN(test_array);
	KT_RUN(test_unwrap);
	KT_RUN(test_resv);
	KT_RUN(test_sync_file);
	KT_RUN(test_aurora_bridge);
	return 0;
}

static void __exit test_k7_exit(void)
{
}

module_init(test_k7_init);
module_exit(test_k7_exit);
MODULE_LICENSE("GPL");
