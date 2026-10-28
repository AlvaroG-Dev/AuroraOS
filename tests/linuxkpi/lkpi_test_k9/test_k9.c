// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pruebas de L6 en la capa (K9): shrinker minimo (registro, count/scan,
 * presion de memoria desde alloc_pages), split_page(), si_meminfo() y el
 * "swap" en RAM, shmem como respaldo (paginas que se escriben, se sueltan y se
 * recuperan), kmap_local_page_prot() con WC, io_mapping, mmu_notifier como
 * stub y hmm desactivado.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/hmm.h>
#include <linux/io-mapping.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/pagemap.h>
#include <linux/shmem_fs.h>
#include <linux/shrinker.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/vmalloc.h>
#include "lkpi_test.h"

/* ---------------------------------------------------------- shrinker */

struct kt_cache {
	atomic_t objs;
	atomic_t scans;
	unsigned long last_nr;
};

static unsigned long kt_count(struct shrinker *s, struct shrink_control *sc)
{
	struct kt_cache *c = s->private_data;
	int n = atomic_read(&c->objs);

	return n ? n : SHRINK_EMPTY;
}

static unsigned long kt_scan(struct shrinker *s, struct shrink_control *sc)
{
	struct kt_cache *c = s->private_data;
	unsigned long n = min_t(unsigned long, sc->nr_to_scan, atomic_read(&c->objs));

	atomic_inc(&c->scans);
	c->last_nr = sc->nr_to_scan;
	if (!n)
		return SHRINK_STOP;
	atomic_sub(n, &c->objs);
	return n;
}

static void test_shrinker(void)
{
	struct kt_cache c = { };
	struct shrinker *s = shrinker_alloc(0, "kt-%s", "cache");
	unsigned long got;

	KT_ASSERT(s, "shrinker_alloc");
	if (!s)
		return;
	s->count_objects = kt_count;
	s->scan_objects = kt_scan;
	s->private_data = &c;
	atomic_set(&c.objs, 100);
	/* sin registrar no participa */
	KT_EQ(lkpi_shrink_slab(GFP_KERNEL, 10), 0);
	KT_EQ(atomic_read(&c.scans), 0);
	shrinker_register(s);
	got = lkpi_shrink_slab(GFP_KERNEL, 30);
	KT_EQ(got, 30);
	KT_EQ(atomic_read(&c.objs), 70);
	KT_EQ(c.last_nr, 30);
	got = lkpi_shrink_slab(GFP_KERNEL, 1000);	/* solo hay 70 */
	KT_EQ(got, 70);
	KT_EQ(atomic_read(&c.objs), 0);
	/* vacio: count devuelve SHRINK_EMPTY y no se llama a scan */
	KT_EQ(lkpi_shrink_slab(GFP_KERNEL, 10), 0);
	KT_EQ(atomic_read(&c.scans), 2);
	shrinker_free(s);
	atomic_set(&c.objs, 5);
	KT_EQ(lkpi_shrink_slab(GFP_KERNEL, 10), 0);	/* ya no esta */
}

/* Un shrinker que devuelve paginas al PMM cuando falta memoria. */
static struct page *kt_hoard[512];
static atomic_t kt_hoard_n;

static unsigned long kt_hoard_count(struct shrinker *s, struct shrink_control *sc)
{
	int n = atomic_read(&kt_hoard_n);

	return n ? n : SHRINK_EMPTY;
}

static unsigned long kt_hoard_scan(struct shrinker *s, struct shrink_control *sc)
{
	unsigned long freed = 0;

	while (freed < sc->nr_to_scan && atomic_read(&kt_hoard_n) > 0) {
		int n = atomic_dec_return(&kt_hoard_n);

		__free_pages(kt_hoard[n], 6);
		kt_hoard[n] = NULL;
		freed += 64;
	}
	return freed ?: SHRINK_STOP;
}

static void test_memory_pressure(void)
{
	struct shrinker *s = shrinker_alloc(0, "kt-hoard");
	struct page *p, *junk[64];
	int n = 0, i;
	struct sysinfo si;

	KT_ASSERT(s, "shrinker_alloc");
	if (!s)
		return;
	s->count_objects = kt_hoard_count;
	s->scan_objects = kt_hoard_scan;
	shrinker_register(s);
	si_meminfo(&si);
	KT_ASSERT(si.totalram > 0 && si.freeram > 0 && si.freeram <= si.totalram, "si_meminfo");
	KT_EQ(si.mem_unit, PAGE_SIZE);
	/* acaparar bloques de 64 paginas hasta que el PMM no de mas (o 512 bloques) */
	while (n < ARRAY_SIZE(kt_hoard)) {
		p = alloc_pages(GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN, 6);
		if (!p)
			break;
		kt_hoard[n++] = p;
	}
	atomic_set(&kt_hoard_n, n);
	KT_ASSERT(n > 0, "acaparado %d bloques", n);
	if (n == ARRAY_SIZE(kt_hoard)) {
		/* hay RAM de sobra: no se puede forzar el fallo; se prueba la ruta a mano */
		KT_EQ(lkpi_shrink_slab(GFP_KERNEL, 64 * 4), 64 * 4);
		KT_EQ(atomic_read(&kt_hoard_n), n - 4);
		n -= 4;
	} else {
		/* el PMM esta agotado: alloc_pages sin NORETRY debe pedir al shrinker y tener exito */
		p = alloc_pages(GFP_KERNEL, 6);
		KT_ASSERT(p, "alloc_pages bajo presion con shrinker");
		if (p)
			__free_pages(p, 6);
		KT_ASSERT(atomic_read(&kt_hoard_n) < n, "el shrinker libero bloques");
		n = atomic_read(&kt_hoard_n);
	}
	(void)junk;
	(void)i;
	/* devolver el resto */
	while (atomic_read(&kt_hoard_n) > 0) {
		int k = atomic_dec_return(&kt_hoard_n);

		__free_pages(kt_hoard[k], 6);
		kt_hoard[k] = NULL;
	}
	shrinker_free(s);
}

/* ---------------------------------------------------------- split_page */

static void test_split_page(void)
{
	struct page *p = alloc_pages(GFP_KERNEL | __GFP_ZERO, 4);
	struct sysinfo a, b;
	unsigned long free0;
	int i;

	KT_ASSERT(p, "alloc_pages orden 4");
	if (!p)
		return;
	si_meminfo(&a);
	free0 = a.freeram;
	for (i = 0; i < 16; i++)
		memset(page_address(p + i), i + 1, PAGE_SIZE);
	split_page(p, 4);
	for (i = 0; i < 16; i++) {
		KT_ASSERT(!PageCompound(p + i), "pagina %d suelta", i);
		KT_EQ(page_ref_count(p + i), 1);
		KT_EQ(((u8 *)page_address(p + i))[100], i + 1);
	}
	/* liberar en desorden: cada pagina vuelve sola al PMM */
	for (i = 0; i < 16; i += 2)
		__free_pages(p + i, 0);
	si_meminfo(&b);
	KT_ASSERT(b.freeram >= free0 + 8 - 2, "liberadas 8 paginas (%lu -> %lu)", free0, b.freeram);
	for (i = 1; i < 16; i += 2) {
		KT_EQ(((u8 *)page_address(p + i))[0], i + 1);	/* las otras siguen intactas */
		put_page(p + i);
	}
	si_meminfo(&b);
	KT_ASSERT(b.freeram >= free0 + 16 - 2, "liberadas las 16 (%lu -> %lu)", free0, b.freeram);
	/* y el bloque se puede volver a pedir entero */
	p = alloc_pages(GFP_KERNEL, 4);
	KT_ASSERT(p, "alloc_pages orden 4 tras el split");
	if (p)
		__free_pages(p, 4);
}

/* ---------------------------------------------------------- swap en RAM */

static void test_swap_ram(void)
{
	struct file *f = shmem_file_setup("kt swap", 64 * PAGE_SIZE, 0);
	struct page *src;
	long nr0, nr1;
	int i, bad = 0;

	KT_ASSERT(!IS_ERR(f), "shmem_file_setup");
	if (IS_ERR(f))
		return;
	nr0 = get_nr_swap_pages();
	KT_ASSERT(nr0 > 1024, "espacio de swap en RAM: %ld paginas", nr0);
	src = alloc_page(GFP_KERNEL);
	if (!src) {
		fput(f);
		return;
	}
	/* escribir 64 paginas en el fichero (lo que hace ttm_backup_backup_page) */
	for (i = 0; i < 64; i++) {
		struct folio *fo = shmem_read_folio(f->f_mapping, i);

		if (IS_ERR(fo)) {
			bad++;
			continue;
		}
		memset(page_address(src), 0x40 + i, PAGE_SIZE);
		folio_mark_dirty(fo);
		folio_lock(fo);
		copy_highpage(folio_file_page(fo, i), src);
		folio_unlock(fo);
		folio_put(fo);
	}
	KT_EQ(bad, 0);
	nr1 = get_nr_swap_pages();
	KT_ASSERT(nr1 < nr0, "el respaldo consume RAM (%ld -> %ld)", nr0, nr1);
	/* la pagina se suelta (truncate) y la siguiente lectura la devuelve a cero */
	shmem_truncate_range(file_inode(f), 10 * PAGE_SIZE, 11 * PAGE_SIZE - 1);
	for (i = 0; i < 64; i++) {
		struct folio *fo = shmem_read_folio(f->f_mapping, i);
		u8 *p;

		if (IS_ERR(fo)) {
			bad++;
			continue;
		}
		p = page_address(folio_file_page(fo, i));
		if (p[7] != (i == 10 ? 0 : (u8)(0x40 + i)))
			bad++;
		folio_put(fo);
	}
	KT_EQ(bad, 0);
	KT_EQ(shmem_writeout(NULL, NULL, NULL), -EBUSY);	/* sin disco no se escribe */
	__free_page(src);
	fput(f);
	/* al soltar el fichero toda la RAM vuelve */
	KT_ASSERT(get_nr_swap_pages() > nr1, "la RAM del respaldo se libera");
}

/* ---------------------------------------------------------- kmap prot */

static void test_kmap_prot(void)
{
	struct page *p = alloc_page(GFP_KERNEL | __GFP_ZERO);
	u32 *wb, *wc;

	KT_ASSERT(p, "alloc_page");
	if (!p)
		return;
	wb = kmap_local_page_prot(p, PAGE_KERNEL);
	KT_EQ((unsigned long)wb, (unsigned long)page_address(p));
	wb[5] = 0x1badcafe;
	wc = kmap_local_page_prot(p, pgprot_writecombine(PAGE_KERNEL));
	KT_ASSERT(wc && wc != wb, "mapeo WC propio");
	if (wc) {
		KT_ASSERT(is_vmalloc_addr(wc), "el mapeo WC sale de la region vmap");
		KT_EQ(wc[5], 0x1badcafe);
		wc[6] = 0x2badcafe;
		kunmap_local(wc);
	}
	KT_EQ(wb[6], 0x2badcafe);
	kunmap_local(wb);
	__free_page(p);
}

/* ---------------------------------------------------------- io_mapping */

static void test_io_mapping(void)
{
	struct page *p = alloc_pages(GFP_KERNEL | __GFP_ZERO, 2);
	struct io_mapping *m;
	void __iomem *a;

	KT_ASSERT(p, "alloc_pages");
	if (!p)
		return;
	m = io_mapping_create_wc(page_to_phys(p), 4 * PAGE_SIZE);
	KT_ASSERT(m, "io_mapping_create_wc");
	if (m) {
		a = io_mapping_map_local_wc(m, 2 * PAGE_SIZE);
		writel(0x600dbeef, a + 8);
		io_mapping_unmap_local(a);
		KT_EQ(((u32 *)page_address(p + 2))[2], 0x600dbeef);
		io_mapping_free(m);
	}
	__free_pages(p, 2);
}

/* ---------------------------------------------------------- mmu_notifier / hmm */

static bool kt_invalidate(struct mmu_interval_notifier *sub, const struct mmu_notifier_range *r,
			  unsigned long seq)
{
	return true;
}

static const struct mmu_interval_notifier_ops kt_mmu_ops = { .invalidate = kt_invalidate };

static void test_mmu_notifier(void)
{
	struct mmu_interval_notifier sub = { };
	struct mmu_notifier n = { };
	struct mmu_notifier_range range;
	struct hmm_range h = { };
	unsigned long seq;

	KT_EQ(mmu_notifier_register(&n, NULL), 0);
	mmu_notifier_unregister(&n, NULL);
	KT_EQ(mmu_interval_notifier_insert(&sub, NULL, 0x1000, 0x4000, &kt_mmu_ops), 0);
	KT_EQ(sub.last, 0x4fff);
	seq = mmu_interval_read_begin(&sub);
	KT_ASSERT(!mmu_interval_read_retry(&sub, seq), "sin invalidaciones no se reintenta");
	mmu_interval_set_seq(&sub, seq + 2);
	KT_ASSERT(mmu_interval_read_retry(&sub, seq), "tras set_seq hay que reintentar");
	mmu_interval_notifier_remove(&sub);
	mmu_notifier_range_init(&range, MMU_NOTIFY_UNMAP, MMU_NOTIFIER_RANGE_BLOCKABLE, NULL, 0, 4096);
	KT_ASSERT(mmu_notifier_range_blockable(&range), "range blockable");
	mmu_notifier_invalidate_range_start(&range);
	mmu_notifier_invalidate_range_end(&range);
	KT_EQ(hmm_range_fault(&h), -EOPNOTSUPP);
}

static int __init test_k9_init(void)
{
	test_shrinker();
	test_memory_pressure();
	test_split_page();
	test_swap_ram();
	test_kmap_prot();
	test_io_mapping();
	test_mmu_notifier();
	return 0;
}

static void __exit test_k9_exit(void)
{
}

module_init(test_k9_init);
module_exit(test_k9_exit);
MODULE_LICENSE("GPL");
