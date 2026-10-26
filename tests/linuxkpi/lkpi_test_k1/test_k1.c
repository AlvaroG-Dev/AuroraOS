// SPDX-License-Identifier: GPL-2.0-only
/* Pruebas de K1 sin DMA: kmalloc, kmem_cache, paginas, vmalloc, ioremap. */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include "lkpi_test.h"

unsigned long lkpi_pages_owned(void);

static void test_kmalloc(void)
{
	static const size_t sizes[] = { 1, 7, 8, 24, 64, 100, 128, 500, 512, 1000,
					2048, 2049, 4096, 5000, 65536, 1 << 20 };
	void *p[ARRAY_SIZE(sizes)];
	unsigned int i;
	bool ok_align = true, ok_size = true, ok_phys = true;

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		size_t sz = sizes[i];
		p[i] = kmalloc(sz, GFP_KERNEL);
		KT_ASSERT(p[i] != NULL, "kmalloc(%zu)", sz);
		if (!p[i])
			continue;
		memset(p[i], 0xa5, sz);
		/* Potencias de dos: alineadas a su tamano (hasta una pagina). */
		if (is_power_of_2(sz) && !IS_ALIGNED((unsigned long)p[i], min_t(size_t, sz, PAGE_SIZE)))
			ok_align = false;
		if (ksize(p[i]) < sz)
			ok_size = false;
		/* Fisicamente contigua: el ultimo byte esta donde dice __pa. */
		if (__pa((char *)p[i] + sz - 1) != __pa(p[i]) + sz - 1)
			ok_phys = false;
	}
	KT_ASSERT(ok_align, "kmalloc: alineacion natural");
	KT_ASSERT(ok_size, "ksize >= tamano pedido");
	KT_ASSERT(ok_phys, "kmalloc fisicamente contiguo");
	for (i = 0; i < ARRAY_SIZE(sizes); i++)
		kfree(p[i]);

	{
		u8 *z = kzalloc(3000, GFP_KERNEL);
		KT_ASSERT(z && !memchr_inv(z, 0, 3000), "kzalloc a cero");
		z = krealloc(z, 6000, GFP_KERNEL | __GFP_ZERO);
		KT_ASSERT(z && !memchr_inv(z, 0, 6000), "krealloc conserva y pone a cero");
		memset(z, 7, 6000);
		z = krealloc(z, 10, GFP_KERNEL);
		KT_ASSERT(z && z[9] == 7, "krealloc a menos");
		kfree(z);
	}
	KT_ASSERT(kmalloc(0, GFP_KERNEL) == ZERO_SIZE_PTR, "kmalloc(0)");
	kfree(ZERO_SIZE_PTR);
	kfree(NULL);
	KT_ASSERT(kmalloc_array(SIZE_MAX / 2, 4, GFP_KERNEL) == NULL, "kmalloc_array desborda");
	KT_ASSERT(kcalloc(SIZE_MAX / 8, 16, GFP_KERNEL) == NULL, "kcalloc desborda");
	{
		int *a = kcalloc(1000, sizeof(int), GFP_KERNEL);
		KT_ASSERT(a && a[999] == 0, "kcalloc");
		kfree(a);
	}

	/* Muchas asignaciones pequenas: se reutilizan los slabs. */
	{
		void **v = kmalloc_array(4000, sizeof(void *), GFP_KERNEL);
		bool all = v != NULL;
		for (i = 0; v && i < 4000; i++) {
			v[i] = kmalloc(16 + (i % 7) * 40, GFP_KERNEL);
			if (!v[i])
				all = false;
			else
				*(u32 *)v[i] = i;
		}
		for (i = 0; v && i < 4000; i++)
			if (v[i] && *(u32 *)v[i] != i)
				all = false;
		for (i = 0; v && i < 4000; i++)
			kfree(v[i]);
		kfree(v);
		KT_ASSERT(all, "4000 kmalloc pequenos");
	}
}

static int ctor_calls;
struct kt_obj {
	u64 magic;
	char data[200];
};

static void kt_ctor(void *p)
{
	((struct kt_obj *)p)->magic = 0xC0FFEE;
	ctor_calls++;
}

static void test_kmem_cache(void)
{
	struct kmem_cache *c = kmem_cache_create("kt_obj", sizeof(struct kt_obj), 64,
						 SLAB_HWCACHE_ALIGN, kt_ctor);
	struct kt_obj *o[300];
	bool ok = true;
	unsigned int i;

	KT_ASSERT(c != NULL, "kmem_cache_create");
	if (!c)
		return;
	KT_EQ(kmem_cache_size(c), sizeof(struct kt_obj));
	for (i = 0; i < ARRAY_SIZE(o); i++) {
		o[i] = kmem_cache_alloc(c, GFP_KERNEL);
		if (!o[i] || o[i]->magic != 0xC0FFEE || !IS_ALIGNED((unsigned long)o[i], 64))
			ok = false;
	}
	KT_ASSERT(ok, "objetos construidos y alineados a 64");
	KT_ASSERT(ctor_calls >= (int)ARRAY_SIZE(o), "constructor llamado %d veces", ctor_calls);
	for (i = 0; i < ARRAY_SIZE(o); i += 2)
		kmem_cache_free(c, o[i]);
	for (i = 0; i < ARRAY_SIZE(o); i += 2) {
		o[i] = kmem_cache_zalloc(c, GFP_KERNEL);
		if (!o[i] || o[i]->magic != 0)
			ok = false;
	}
	KT_ASSERT(ok, "kmem_cache_zalloc");
	for (i = 0; i < ARRAY_SIZE(o); i++)
		kfree(o[i]);	/* kfree tambien vale para objetos de un cache */
	kmem_cache_destroy(c);

	c = KMEM_CACHE(kt_obj, 0);
	KT_ASSERT(c != NULL, "KMEM_CACHE");
	if (c) {
		void *x = kmem_cache_alloc(c, GFP_KERNEL);
		KT_ASSERT(x != NULL, "alloc");
		kmem_cache_free(c, x);
		kmem_cache_destroy(c);
	}
}

static void test_pages(void)
{
	unsigned long owned0 = lkpi_pages_owned();
	struct page *pg;
	unsigned long addr;
	void *v;

	for (unsigned int order = 0; order <= 4; order++) {
		pg = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
		KT_ASSERT(pg != NULL, "alloc_pages(order %u)", order);
		if (!pg)
			continue;
		v = page_address(pg);
		KT_ASSERT(!memchr_inv(v, 0, PAGE_SIZE << order), "__GFP_ZERO");
		KT_EQ(page_to_phys(pg), __pa(v));
		KT_ASSERT(virt_to_page(v) == pg, "virt_to_page");
		KT_ASSERT(pfn_to_page(page_to_pfn(pg)) == pg, "pfn_to_page");
		if (order) {
			struct page *last = nth_page(pg, (1 << order) - 1);
			KT_ASSERT(page_address(last) == (char *)v + ((PAGE_SIZE << order) - PAGE_SIZE),
				  "pagina de cola");
			KT_ASSERT(virt_to_page((char *)v + PAGE_SIZE) == pg + 1, "virt_to_page(cola)");
			KT_ASSERT(compound_head(last) == pg, "compound_head");
		}
		KT_ASSERT(kmap_local_page(pg) == v, "kmap_local_page");
		__free_pages(pg, order);
	}

	pg = alloc_page(GFP_KERNEL);
	if (pg) {
		get_page(pg);
		KT_EQ(page_count(pg), 2);
		put_page(pg);
		KT_EQ(page_count(pg), 1);
		put_page(pg);	/* libera */
	}
	addr = __get_free_pages(GFP_KERNEL, 2);
	KT_ASSERT(addr && IS_ALIGNED(addr, PAGE_SIZE), "__get_free_pages");
	free_pages(addr, 2);
	addr = get_zeroed_page(GFP_KERNEL);
	KT_ASSERT(addr && !memchr_inv((void *)addr, 0, PAGE_SIZE), "get_zeroed_page");
	free_page(addr);
	v = alloc_pages_exact(3 * PAGE_SIZE, GFP_KERNEL);
	KT_ASSERT(v != NULL, "alloc_pages_exact");
	free_pages_exact(v, 3 * PAGE_SIZE);
	KT_EQ(lkpi_pages_owned(), owned0);

	KT_ASSERT(__va(__pa(&ctor_calls)) != NULL, "__va/__pa");
	KT_EQ(get_order(1), 0);
	KT_EQ(get_order(PAGE_SIZE + 1), 1);
	KT_EQ(get_order(5 * PAGE_SIZE), 3);
	KT_ASSERT(totalram_pages() > 1000, "totalram_pages");
}

static void test_vmalloc(void)
{
	size_t sz = 3 << 20;
	u8 *v = vzalloc(sz);
	bool ok = true;

	KT_ASSERT(v != NULL, "vzalloc 3 MiB");
	if (!v)
		return;
	KT_ASSERT(IS_ALIGNED((unsigned long)v, PAGE_SIZE), "vmalloc alineado a pagina");
	KT_ASSERT(!memchr_inv(v, 0, sz), "vzalloc a cero");
	for (size_t i = 0; i < sz; i += 4096)
		v[i] = (u8)(i >> 12);
	for (size_t i = 0; i < sz; i += 4096)
		if (v[i] != (u8)(i >> 12))
			ok = false;
	KT_ASSERT(ok, "vmalloc lectura/escritura");
	KT_ASSERT(is_vmalloc_addr(v + 100), "is_vmalloc_addr");
	{
		struct page *pg = vmalloc_to_page(v + 2 * PAGE_SIZE);
		KT_ASSERT(pg && page_address(pg) &&
			  ((u8 *)page_address(pg))[0] == v[2 * PAGE_SIZE], "vmalloc_to_page");
	}
	vfree(v);

	v = kvmalloc(1000, GFP_KERNEL);
	KT_ASSERT(v && !is_vmalloc_addr(v), "kvmalloc pequeno usa kmalloc");
	kvfree(v);
	v = kvzalloc(8 << 20, GFP_KERNEL);
	KT_ASSERT(v && is_vmalloc_addr(v) && v[(8 << 20) - 1] == 0, "kvmalloc grande usa vmalloc");
	kvfree(v);
	v = kvmalloc_array(10, 100, GFP_KERNEL);
	KT_ASSERT(v != NULL, "kvmalloc_array");
	v = kvrealloc(v, 64 << 10, GFP_KERNEL);
	KT_ASSERT(v != NULL, "kvrealloc");
	kvfree(v);
}

static void test_io(void)
{
	/* La LAPIC: MMIO que siempre existe en x86 (registro de version). */
	void __iomem *a = ioremap(0xFEE00000, PAGE_SIZE);
	void __iomem *b;
	u32 ver;

	KT_ASSERT(a != NULL, "ioremap LAPIC");
	if (!a)
		return;
	ver = readl(a + 0x30);
	KT_ASSERT((ver & 0xff) >= 0x10, "version de la LAPIC = 0x%x", ver);
	b = ioremap(0xFEE00030, 4);
	KT_ASSERT(b == a + 0x30, "dos ioremap del mismo rango comparten direccion");
	iounmap(b);
	KT_EQ(readl(a + 0x30), ver);	/* sigue mapeado */
	iounmap(a);

	{
		void *ram = kmalloc(64, GFP_KERNEL);
		void *m = memremap(__pa(ram), 64, MEMREMAP_WB);
		KT_ASSERT(m == ram, "memremap(WB) de RAM es la ventana directa");
		memunmap(m);
		kfree(ram);
	}
	{
		/* Puntero de kernel como si fuera de usuario: debe fallar. */
		char k[8];
		KT_EQ(copy_to_user((void __user *)k, "x", 1), 1);
		KT_EQ(access_ok((void __user *)0x1000, 16), 1);
		KT_EQ(access_ok((void __user *)k, 16), 0);
	}
}

static int __init test_k1_init(void)
{
	test_kmalloc();
	test_kmem_cache();
	test_pages();
	test_vmalloc();
	test_io();
	return 0;
}

static void __exit test_k1_exit(void)
{
}

module_init(test_k1_init);
module_exit(test_k1_exit);
MODULE_LICENSE("GPL");
