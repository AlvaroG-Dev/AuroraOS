// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pruebas de L4 en el kernel (K1-DMA, K6 basico, K8):
 *  - scatterlist (tablas desde paginas, iteradores, copias);
 *  - DMA de streaming: mapeo directo y bounce cuando la mascara no alcanza,
 *    en las cuatro direcciones, con sync_*; y contra hardware: el motor DMA de
 *    `edu` (mascara de 28 bits) leyendo y escribiendo un buffer por encima de
 *    256 MiB a traves del bounce;
 *  - memoria coherente WC, dma_get_sgtable, dma_pool, vmap/vmap_pfn;
 *  - dma_buf sin fds: exportar, adjuntar, mapear, vmap y liberar;
 *  - request_firmware con un fichero real de linux-firmware y el de Aurora,
 *    _nowait e _into_buf;
 *  - i2c-algo-bit contra una EEPROM 24C02 simulada por software (lineas de
 *    drenador abierto), con SMBus emulado y un NAK de direccion;
 *  - backlight por /sys; debugfs (entradas y borrado); identidad de current.
 * Lo que necesita un proceso (fds, mmap, poll, senales) esta en
 * lkpi_test_dev + user/musl/apps/lkpi_test.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/backlight.h>
#include <linux/capability.h>
#include <linux/completion.h>
#include <linux/crc32.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include "lkpi_k5_toys.h"

static struct platform_device *kt_pdev;
static u64 kt_mask;

/* Ruta (sin "/sys/") del atributo `attr` del dispositivo al que apunta el
 * enlace `link` de /sys; "" si el enlace no existe. */
static void kt_attr_path(const char *link, const char *attr, char *out, size_t n)
{
	char *data = NULL;
	size_t len = 0;

	out[0] = '\0';
	if (kt_sysfs_get(link, &data, &len) == KT_SYSFS_LINK && data &&
	    !strncmp(data, "/sys/", 5))
		snprintf(out, n, "%s/%s", data + 5, attr);
	kt_sysfs_free(data);
}

/* Contenido de un atributo (o "" si no es un atributo). */
static void kt_attr_read(const char *path, char *out, size_t n)
{
	char *data = NULL;
	size_t len = 0;

	out[0] = '\0';
	if (kt_sysfs_get(path, &data, &len) == KT_SYSFS_ATTR && data)
		strscpy(out, data, n);
	kt_sysfs_free(data);
}

/* --------------------------------------------------------- scatterlist */

static void test_scatterlist(void)
{
	struct page *pages[6], *blk;
	struct sg_table sgt;
	struct sg_page_iter it;
	struct scatterlist *sg, one, two[2];
	unsigned int i, n;
	char *buf = kmalloc(64, GFP_KERNEL);	/* sg: memoria de la ventana directa */

	/* Dos tramos contiguos (3 + 2 paginas) y uno suelto. */
	blk = alloc_pages(GFP_KERNEL, 3);
	KT_ASSERT(blk && buf, "alloc_pages(3)");
	if (!blk || !buf) {
		kfree(buf);
		return;
	}
	pages[0] = blk; pages[1] = blk + 1; pages[2] = blk + 2;
	pages[3] = blk + 4; pages[4] = blk + 5;
	pages[5] = blk + 7;
	KT_EQ(sg_alloc_table_from_pages(&sgt, pages, 6, 100, 6 * PAGE_SIZE - 200, GFP_KERNEL), 0);
	KT_EQ(sgt.orig_nents, 3);
	KT_EQ(sgt.sgl[0].offset, 100);
	KT_EQ(sgt.sgl[0].length, 3 * PAGE_SIZE - 100);
	KT_EQ(sgt.sgl[1].length, 2 * PAGE_SIZE);
	KT_EQ(sgt.sgl[2].length, PAGE_SIZE - 100);
	KT_ASSERT(sg_is_last(&sgt.sgl[2]), "ultima marcada");
	KT_EQ(sg_nents(sgt.sgl), 3);
	n = 0;
	for_each_sgtable_page(&sgt, &it, 0) {
		KT_ASSERT(sg_page_iter_page(&it) == pages[n], "pagina %u del iterador", n);
		n++;
	}
	KT_EQ(n, 6);
	for (i = 0; i < 64; i++)
		buf[i] = i;
	KT_EQ(sg_pcopy_from_buffer(sgt.sgl, sgt.orig_nents, buf, 64,
				   3 * PAGE_SIZE - 100 - 10), 64);
	KT_ASSERT(((u8 *)page_address(blk + 4))[0] == 10, "copia que cruza un tramo");
	sg_free_table(&sgt);

	sg_init_one(&one, buf, 16);
	KT_ASSERT(sg_virt(&one) == buf && sg_is_last(&one) && one.length == 16, "sg_init_one");
	sg_init_table(two, 2);
	sg_set_buf(&two[0], buf, 16);
	sg_set_buf(&two[1], buf + 16, 8);
	n = 0;
	for_each_sg(two, sg, 2, i)
		n += sg->length;
	KT_EQ(n, 24);
	__free_pages(blk, 3);
	kfree(buf);
}

/* ------------------------------------------------- DMA de streaming */

static bool kt_fill(u8 *p, size_t n, u8 seed)
{
	size_t i;

	for (i = 0; i < n; i++)
		p[i] = (u8)(seed + i * 13);
	return true;
}

static bool kt_check(const u8 *p, size_t n, u8 seed)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (p[i] != (u8)(seed + i * 13))
			return false;
	return true;
}

static void test_dma_bounce(void)
{
	struct device *dev = &kt_pdev->dev;
	u64 bounces = atomic64_read(&lkpi_dma_stats.bounces);
	u8 *buf = kmalloc(2 * PAGE_SIZE, GFP_KERNEL);
	phys_addr_t phys;
	dma_addr_t d;
	int bits;

	KT_ASSERT(buf != NULL, "kmalloc");
	if (!buf)
		return;
	phys = virt_to_phys(buf);
	/* Mascara de 64 bits: direccion de bus = fisica. */
	kt_mask = DMA_BIT_MASK(64);
	dev->dma_mask = &kt_mask;
	d = dma_map_single(dev, buf + 8, 100, DMA_TO_DEVICE);
	KT_EQ(d, phys + 8);
	dma_unmap_single(dev, d, 100, DMA_TO_DEVICE);
	KT_EQ(atomic64_read(&lkpi_dma_stats.bounces), bounces);

	/* Mascara por debajo del buffer: bounce. */
	bits = fls64(phys) - 1;
	if (bits < 24) {
		KT_SKIP("buffer de kmalloc por debajo de 16 MiB: sin prueba de bounce");
		kfree(buf);
		return;
	}
	KT_EQ(dma_set_mask(dev, DMA_BIT_MASK(bits)), 0);
	kt_fill(buf, 300, 1);
	d = dma_map_single(dev, buf + 4, 300, DMA_TO_DEVICE);
	KT_ASSERT(d != DMA_MAPPING_ERROR && d + 300 - 1 <= DMA_BIT_MASK(bits) && d != phys + 4,
		  "bounce bajo la mascara (%llx, mascara %d bits)", (unsigned long long)d, bits);
	KT_EQ(atomic64_read(&lkpi_dma_stats.bounces), bounces + 1);
	KT_ASSERT(kt_check(phys_to_virt(d), 296, 1 + 4 * 13), "TO_DEVICE copia al bounce");
	/* El "dispositivo" no escribe: el unmap TO_DEVICE no copia de vuelta. */
	memset(phys_to_virt(d), 0xEE, 300);
	dma_unmap_single(dev, d, 300, DMA_TO_DEVICE);
	KT_ASSERT(kt_check(buf + 4, 296, 1 + 4 * 13), "TO_DEVICE no pisa el original");

	memset(buf, 0, 300);
	d = dma_map_single(dev, buf, 300, DMA_FROM_DEVICE);
	kt_fill(phys_to_virt(d), 300, 9);	/* "escribe" el dispositivo */
	dma_sync_single_for_cpu(dev, d, 300, DMA_FROM_DEVICE);
	KT_ASSERT(kt_check(buf, 300, 9), "sync_for_cpu copia del bounce");
	kt_fill(buf, 300, 50);			/* la CPU escribe */
	dma_sync_single_for_device(dev, d, 300, DMA_FROM_DEVICE);
	KT_ASSERT(kt_check(phys_to_virt(d), 300, 9), "FROM_DEVICE: sync_for_device no copia");
	kt_fill(phys_to_virt(d), 300, 77);
	dma_unmap_single(dev, d, 300, DMA_FROM_DEVICE);
	KT_ASSERT(kt_check(buf, 300, 77), "unmap FROM_DEVICE copia de vuelta");

	d = dma_map_page(dev, virt_to_page(buf), offset_in_page(buf), 64, DMA_BIDIRECTIONAL);
	kt_fill(phys_to_virt(d), 64, 3);
	dma_unmap_page(dev, d, 64, DMA_BIDIRECTIONAL);
	KT_ASSERT(kt_check(buf, 64, 3), "BIDIRECTIONAL");
	KT_EQ(atomic64_read(&lkpi_dma_stats.bounce_live), 0);
	kfree(buf);
	dev->dma_mask = NULL;
}

static void test_dma_sg(void)
{
	struct device *dev = &kt_pdev->dev;
	struct sg_table sgt;
	struct scatterlist *sg;
	u8 *a = kmalloc(512, GFP_KERNEL), *b = kmalloc(256, GFP_KERNEL);
	unsigned int i;
	int bits;

	if (!a || !b)
		goto out;
	bits = min(fls64(virt_to_phys(a)), fls64(virt_to_phys(b))) - 1;
	kt_mask = bits >= 24 ? DMA_BIT_MASK(bits) : DMA_BIT_MASK(64);
	dev->dma_mask = &kt_mask;
	KT_EQ(sg_alloc_table(&sgt, 2, GFP_KERNEL), 0);
	sg_set_buf(&sgt.sgl[0], a, 512);
	sg_set_buf(&sgt.sgl[1], b, 256);
	kt_fill(a, 512, 2);
	kt_fill(b, 256, 4);
	KT_EQ(dma_map_sgtable(dev, &sgt, DMA_BIDIRECTIONAL, 0), 0);
	KT_EQ(sgt.nents, 2);
	for_each_sgtable_dma_sg(&sgt, sg, i)
		KT_ASSERT(sg_dma_address(sg) + sg_dma_len(sg) - 1 <= kt_mask,
			  "entrada %u bajo la mascara", i);
	kt_fill(phys_to_virt(sg_dma_address(&sgt.sgl[1])), 256, 99);
	dma_unmap_sgtable(dev, &sgt, DMA_BIDIRECTIONAL, 0);
	KT_ASSERT(kt_check(a, 512, 2) && kt_check(b, 256, 99), "sg ida y vuelta");
	sg_free_table(&sgt);
	dev->dma_mask = NULL;
out:
	kfree(a);
	kfree(b);
}

/* Motor DMA de edu (28 bits) sobre un buffer por encima de 256 MiB. */
static void test_dma_edu(void)
{
	struct pci_dev *pdev = pci_get_device(EDU_VENDOR, EDU_DEVICE, NULL);
	u64 bounces = atomic64_read(&lkpi_dma_stats.bounces);
	struct page *pg[256];
	struct kt_edu *e;
	dma_addr_t d;
	u8 *buf = NULL;
	int i, n = 0;

	if (!pdev) {
		KT_SKIP("QEMU sin -device edu");
		return;
	}
	KT_EQ(pci_register_driver(&kt_edu_driver), 0);
	e = kt_edu_dev;
	KT_ASSERT(e != NULL, "probe de kt_edu");
	if (!e)
		goto out_drv;
	/* El PMM da primero memoria baja: bloques de 2 MiB hasta pasar 256 MiB. */
	for (n = 0; n < 256; n++) {
		pg[n] = alloc_pages(GFP_KERNEL, 9);
		if (!pg[n])
			break;
		if (page_to_phys(pg[n]) >= (1ULL << 28)) {
			buf = page_address(pg[n]);
			n++;
			break;
		}
	}
	if (!buf) {
		KT_SKIP("sin paginas por encima de 256 MiB para el bounce de edu");
		goto out_pages;
	}
	kt_fill(buf, EDU_DMA_SIZE, 21);
	d = dma_map_single(&pdev->dev, buf, EDU_DMA_SIZE, DMA_TO_DEVICE);
	KT_ASSERT(d != DMA_MAPPING_ERROR && d + EDU_DMA_SIZE <= (1ULL << 28),
		  "edu: bounce bajo 28 bits (%llx)", (unsigned long long)d);
	writeq(d, e->mmio + EDU_DMA_SRC);
	writeq(EDU_DMA_BUF, e->mmio + EDU_DMA_DST);
	writeq(EDU_DMA_SIZE, e->mmio + EDU_DMA_CNT);
	writeq(EDU_DMA_START, e->mmio + EDU_DMA_CMD);
	for (i = 0; i < 300 && (readq(e->mmio + EDU_DMA_CMD) & EDU_DMA_START); i++)
		msleep(10);
	dma_unmap_single(&pdev->dev, d, EDU_DMA_SIZE, DMA_TO_DEVICE);

	memset(buf, 0, EDU_DMA_SIZE);
	d = dma_map_single(&pdev->dev, buf, EDU_DMA_SIZE, DMA_FROM_DEVICE);
	writeq(EDU_DMA_BUF, e->mmio + EDU_DMA_SRC);
	writeq(d, e->mmio + EDU_DMA_DST);
	writeq(EDU_DMA_SIZE, e->mmio + EDU_DMA_CNT);
	writeq(EDU_DMA_START | EDU_DMA_FROM_EDU, e->mmio + EDU_DMA_CMD);
	for (i = 0; i < 300 && (readq(e->mmio + EDU_DMA_CMD) & EDU_DMA_START); i++)
		msleep(10);
	dma_unmap_single(&pdev->dev, d, EDU_DMA_SIZE, DMA_FROM_DEVICE);
	KT_ASSERT(kt_check(buf, EDU_DMA_SIZE, 21), "edu: datos de vuelta por el bounce");
	KT_EQ(atomic64_read(&lkpi_dma_stats.bounces), bounces + 2);
out_pages:
	while (n--)
		__free_pages(pg[n], 9);
out_drv:
	pci_unregister_driver(&kt_edu_driver);
	pci_dev_put(pdev);
}

/* ---------------------------------------- coherente, pool y vmap */

static void test_dma_coherent(void)
{
	struct device *dev = &kt_pdev->dev;
	struct sg_table sgt;
	struct dma_pool *pool;
	dma_addr_t h, hs[40];
	void *p, *ps[40];
	u32 *wc;
	int i;

	dev->coherent_dma_mask = DMA_BIT_MASK(32);
	wc = dma_alloc_wc(dev, 3 * PAGE_SIZE, &h, GFP_KERNEL);
	KT_ASSERT(wc != NULL && is_vmalloc_addr(wc), "dma_alloc_wc en la region vmap");
	if (wc) {
		KT_ASSERT(h + 4 * PAGE_SIZE - 1 <= DMA_BIT_MASK(32) && !(h & (4 * PAGE_SIZE - 1)),
			  "WC: bajo 4 GiB y alineado");
		wc[0] = 0x11223344;
		wc[2 * PAGE_SIZE / 4] = 0x55667788;
		KT_EQ(*(u32 *)phys_to_virt(h), 0x11223344);
		KT_EQ(*(u32 *)phys_to_virt(h + 2 * PAGE_SIZE), 0x55667788);
		KT_EQ(vmalloc_to_pfn(wc + PAGE_SIZE / 4), (h >> PAGE_SHIFT) + 1);
		dma_free_wc(dev, 3 * PAGE_SIZE, wc, h);
	}
	p = dma_alloc_coherent(dev, 2 * PAGE_SIZE, &h, GFP_KERNEL);
	KT_ASSERT(p != NULL, "dma_alloc_coherent");
	if (p) {
		KT_EQ(dma_get_sgtable(dev, &sgt, p, h, 2 * PAGE_SIZE), 0);
		KT_EQ(sg_phys(sgt.sgl), h);
		KT_EQ(sgt.sgl->length, 2 * PAGE_SIZE);
		sg_free_table(&sgt);
		dma_free_coherent(dev, 2 * PAGE_SIZE, p, h);
	}

	pool = dma_pool_create("kt_pool", dev, 200, 64, 1024);
	KT_ASSERT(pool != NULL, "dma_pool_create");
	if (!pool)
		return;
	for (i = 0; i < 40; i++) {
		ps[i] = dma_pool_zalloc(pool, GFP_KERNEL, &hs[i]);
		KT_ASSERT(ps[i] && !(hs[i] & 63) && (hs[i] / 1024) == ((hs[i] + 199) / 1024) &&
			  virt_to_phys(ps[i]) == hs[i] && !((u8 *)ps[i])[199],
			  "bloque %d del pool (%llx)", i, (unsigned long long)hs[i]);
		if (ps[i])
			memset(ps[i], i, 200);
	}
	for (i = 0; i < 40; i += 2)
		dma_pool_free(pool, ps[i], hs[i]);
	p = dma_pool_alloc(pool, GFP_KERNEL, &h);
	KT_ASSERT(p == ps[38], "el ultimo liberado se reutiliza");
	dma_pool_free(pool, p, h);
	for (i = 1; i < 40; i += 2)
		KT_ASSERT(((u8 *)ps[i])[150] == i, "bloque %d intacto", i);
	for (i = 1; i < 40; i += 2)
		dma_pool_free(pool, ps[i], hs[i]);
	dma_pool_destroy(pool);
}

static void test_vmap(void)
{
	struct page *pages[3];
	unsigned long pfns[2];
	u8 *v;
	int i;

	for (i = 0; i < 3; i++)
		pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
	v = vmap(pages, 3, VM_MAP, PAGE_KERNEL);
	KT_ASSERT(v && is_vmalloc_addr(v), "vmap");
	if (v) {
		v[0] = 1;
		v[PAGE_SIZE] = 2;
		v[3 * PAGE_SIZE - 1] = 3;
		KT_ASSERT(((u8 *)page_address(pages[0]))[0] == 1 &&
			  ((u8 *)page_address(pages[1]))[0] == 2 &&
			  ((u8 *)page_address(pages[2]))[PAGE_SIZE - 1] == 3,
			  "vmap escribe en las paginas");
		KT_ASSERT(vmalloc_to_page(v + PAGE_SIZE + 5) &&
			  page_to_pfn(vmalloc_to_page(v + PAGE_SIZE + 5)) == page_to_pfn(pages[1]),
			  "vmalloc_to_page de vmap");
		vunmap(v);
	}
	pfns[0] = page_to_pfn(pages[2]);
	pfns[1] = page_to_pfn(pages[0]);
	v = vmap_pfn(pfns, 2, pgprot_writecombine(PAGE_KERNEL));
	KT_ASSERT(v != NULL, "vmap_pfn WC");
	if (v) {
		KT_EQ(v[PAGE_SIZE], 1);
		KT_EQ(v[PAGE_SIZE - 1], 3);
		vunmap(v);
	}
	for (i = 0; i < 3; i++)
		__free_page(pages[i]);
}

/* -------------------------------------------------------- dma_buf */

struct kt_exp {
	struct page *pages;
	int attached, detached, released, mapped;
};

static int kt_exp_attach(struct dma_buf *d, struct dma_buf_attachment *a)
{
	((struct kt_exp *)d->priv)->attached++;
	return 0;
}

static void kt_exp_detach(struct dma_buf *d, struct dma_buf_attachment *a)
{
	((struct kt_exp *)d->priv)->detached++;
}

static struct sg_table *kt_exp_map(struct dma_buf_attachment *a, enum dma_data_direction dir)
{
	struct kt_exp *x = a->dmabuf->priv;
	struct sg_table *sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);

	if (!sgt || sg_alloc_table(sgt, 1, GFP_KERNEL)) {
		kfree(sgt);
		return ERR_PTR(-ENOMEM);
	}
	sg_set_page(sgt->sgl, x->pages, 2 * PAGE_SIZE, 0);
	if (dma_map_sgtable(a->dev, sgt, dir, 0)) {
		sg_free_table(sgt);
		kfree(sgt);
		return ERR_PTR(-ENOMEM);
	}
	x->mapped++;
	return sgt;
}

static void kt_exp_unmap(struct dma_buf_attachment *a, struct sg_table *sgt,
			 enum dma_data_direction dir)
{
	dma_unmap_sgtable(a->dev, sgt, dir, 0);
	sg_free_table(sgt);
	kfree(sgt);
	((struct kt_exp *)a->dmabuf->priv)->mapped--;
}

static void kt_exp_release(struct dma_buf *d)
{
	((struct kt_exp *)d->priv)->released++;
}

static int kt_exp_vmap(struct dma_buf *d, struct iosys_map *map)
{
	iosys_map_set_vaddr(map, page_address(((struct kt_exp *)d->priv)->pages));
	return 0;
}

static const struct dma_buf_ops kt_exp_ops = {
	.attach = kt_exp_attach,
	.detach = kt_exp_detach,
	.map_dma_buf = kt_exp_map,
	.unmap_dma_buf = kt_exp_unmap,
	.release = kt_exp_release,
	.vmap = kt_exp_vmap,
};

static void test_dma_buf(void)
{
	DEFINE_DMA_BUF_EXPORT_INFO(info);
	struct kt_exp x = {};
	struct dma_buf *db;
	struct dma_buf_attachment *a;
	struct sg_table *sgt;
	struct iosys_map m1, m2;

	x.pages = alloc_pages(GFP_KERNEL | __GFP_ZERO, 1);
	if (!x.pages)
		return;
	strcpy(page_address(x.pages), "dma_buf de prueba");
	info.ops = &kt_exp_ops;
	info.size = 2 * PAGE_SIZE;
	info.flags = O_RDWR;
	info.priv = &x;
	db = dma_buf_export(&info);
	KT_ASSERT(!IS_ERR(db) && db->file && db->size == 2 * PAGE_SIZE, "dma_buf_export");
	if (IS_ERR(db))
		goto out;
	kt_mask = DMA_BIT_MASK(64);
	kt_pdev->dev.dma_mask = &kt_mask;
	a = dma_buf_attach(db, &kt_pdev->dev);
	KT_ASSERT(!IS_ERR(a) && x.attached == 1, "dma_buf_attach");
	sgt = dma_buf_map_attachment_unlocked(a, DMA_BIDIRECTIONAL);
	KT_ASSERT(!IS_ERR(sgt) && sg_dma_address(sgt->sgl) == page_to_phys(x.pages),
		  "map_attachment");
	dma_buf_unmap_attachment_unlocked(a, sgt, DMA_BIDIRECTIONAL);
	KT_EQ(x.mapped, 0);
	KT_EQ(dma_buf_vmap_unlocked(db, &m1), 0);
	KT_EQ(dma_buf_vmap_unlocked(db, &m2), 0);
	KT_ASSERT(iosys_map_is_equal(&m1, &m2) && !strcmp(m1.vaddr, "dma_buf de prueba"),
		  "vmap compartido");
	KT_EQ(db->vmapping_counter, 2);
	dma_buf_vunmap_unlocked(db, &m2);
	dma_buf_vunmap_unlocked(db, &m1);
	get_dma_buf(db);
	dma_buf_put(db);
	KT_EQ(x.released, 0);
	dma_buf_detach(db, a);
	KT_EQ(x.detached, 1);
	dma_buf_put(db);
	KT_EQ(x.released, 1);
	kt_pdev->dev.dma_mask = NULL;
out:
	__free_pages(x.pages, 1);
}

/* -------------------------------------------------------- firmware */

static struct completion kt_fw_done;
static size_t kt_fw_size;

static void kt_fw_cont(const struct firmware *fw, void *ctx)
{
	kt_fw_size = fw ? fw->size : 0;
	KT_ASSERT(ctx == &kt_fw_done, "contexto de _nowait");
	release_firmware(fw);
	complete(&kt_fw_done);
}

static void test_firmware(void)
{
	const struct firmware *fw = NULL;
	char small[16];
	int rc;

	KT_EQ(request_firmware(&fw, "aurora/selftest.bin", &kt_pdev->dev), 0);
	KT_ASSERT(fw && fw->size == 10 && !memcmp(fw->data, "AURORA-FW\n", 10),
		  "firmware de Aurora");
	release_firmware(fw);
	KT_EQ(firmware_request_nowarn(&fw, "lkpi/no-existe.bin", &kt_pdev->dev), -ENOENT);
	KT_ASSERT(fw == NULL, "fw a NULL si falla");
	KT_EQ(request_firmware_into_buf(&fw, "aurora/selftest.bin", &kt_pdev->dev, small,
					sizeof(small)), 0);
	KT_ASSERT(fw && fw->data == (u8 *)small && fw->size == 10, "into_buf");
	release_firmware(fw);
	KT_EQ(request_firmware_into_buf(&fw, "aurora/selftest.bin", &kt_pdev->dev, small, 4),
	      -EFBIG);
	init_completion(&kt_fw_done);
	KT_EQ(request_firmware_nowait(THIS_MODULE, true, "aurora/selftest.bin", &kt_pdev->dev,
				      GFP_KERNEL, &kt_fw_done, kt_fw_cont), 0);
	KT_ASSERT(wait_for_completion_timeout(&kt_fw_done, msecs_to_jiffies(5000)), "_nowait");
	KT_EQ(kt_fw_size, 10);

	/* linux-firmware 20261019 (kernel/linuxkpi/firmware.list), si se instalo. */
	rc = firmware_request_nowarn(&fw, "i915/kbl_dmc_ver1.bin", &kt_pdev->dev);
	if (rc == -ENOENT) {
		KT_SKIP("linux-firmware no instalado (scripts/fetch-linux-drivers.sh --firmware)");
		return;
	}
	KT_EQ(rc, 0);
	if (rc)
		return;
	KT_EQ(fw->size, 8616);
	KT_EQ(~crc32_le(~0U, fw->data, fw->size), 0xc11e0fc6);
	release_firmware(fw);
}

/* ------------------------------------- i2c-algo-bit con una EEPROM */

#define KT_EEPROM_ADDR 0x50

/*
 * Bus de drenador abierto: cada linea vale el AND de lo que sueltan maestro
 * y esclavo. El esclavo (una 24C02 de 256 bytes) reacciona a los flancos de
 * SCL y a START/STOP como uno real, sin estirar el reloj.
 */
struct kt_eeprom {
	int m_sda, m_scl, s_sda;	/* 1 = soltar */
	int l_sda, l_scl;		/* lineas tras el ultimo cambio */
	enum { EE_IDLE, EE_RX, EE_TX, EE_WAIT } st;
	int bit, is_addr, rw, have_ptr, mack;
	u8 byte, ptr;
	u8 mem[256];
	int starts, stops, naks;
};
static struct kt_eeprom kt_ee;

static void kt_ee_rising(struct kt_eeprom *e, int sda)
{
	if (e->st == EE_RX) {
		if (e->bit < 8)
			e->byte = (e->byte << 1) | sda;
		e->bit++;
	} else if (e->st == EE_TX) {
		if (e->bit == 8)
			e->mack = !sda;
		e->bit++;
	}
}

static void kt_ee_falling(struct kt_eeprom *e)
{
	if (e->st == EE_RX) {
		if (e->bit == 8) {
			if (e->is_addr) {
				if ((e->byte >> 1) != KT_EEPROM_ADDR) {
					e->naks++;
					e->st = EE_WAIT;
					e->s_sda = 1;
					return;
				}
				e->rw = e->byte & 1;
				if (!e->rw)
					e->have_ptr = 0;
			} else if (!e->have_ptr) {
				e->ptr = e->byte;
				e->have_ptr = 1;
			} else {
				e->mem[e->ptr++] = e->byte;
			}
			e->s_sda = 0;	/* ACK en el 9.o reloj */
		} else if (e->bit == 9) {
			e->s_sda = 1;
			e->bit = 0;
			e->byte = 0;
			if (e->is_addr && e->rw) {
				e->st = EE_TX;
				e->byte = e->mem[e->ptr++];
				e->s_sda = (e->byte >> 7) & 1;
			}
			e->is_addr = 0;
		}
	} else if (e->st == EE_TX) {
		if (e->bit >= 1 && e->bit < 8) {
			e->s_sda = (e->byte >> (7 - e->bit)) & 1;
		} else if (e->bit == 8) {
			e->s_sda = 1;	/* el maestro pone su ACK */
		} else if (e->bit == 9) {
			if (e->mack) {
				e->byte = e->mem[e->ptr++];
				e->bit = 0;
				e->s_sda = (e->byte >> 7) & 1;
			} else {
				e->st = EE_WAIT;
				e->s_sda = 1;
			}
		}
	}
}

static void kt_ee_update(struct kt_eeprom *e)
{
	int scl = e->m_scl, sda = e->m_sda & e->s_sda;

	if (scl && e->l_scl && sda != e->l_sda) {
		if (!sda) {		/* START */
			e->starts++;
			e->st = EE_RX;
			e->is_addr = 1;
			e->bit = 0;
			e->byte = 0;
		} else {		/* STOP */
			e->stops++;
			e->st = EE_IDLE;
		}
		e->s_sda = 1;
	} else if (scl && !e->l_scl) {
		kt_ee_rising(e, sda);
	} else if (!scl && e->l_scl) {
		kt_ee_falling(e);
	}
	e->l_scl = scl;
	e->l_sda = e->m_sda & e->s_sda;
}

static void kt_setsda(void *d, int v) { kt_ee.m_sda = !!v; kt_ee_update(&kt_ee); }
static void kt_setscl(void *d, int v) { kt_ee.m_scl = !!v; kt_ee_update(&kt_ee); }
static int kt_getsda(void *d) { return kt_ee.m_sda & kt_ee.s_sda; }
static int kt_getscl(void *d) { return kt_ee.m_scl; }

static struct i2c_algo_bit_data kt_bit = {
	.setsda = kt_setsda,
	.setscl = kt_setscl,
	.getsda = kt_getsda,
	.getscl = kt_getscl,
	.udelay = 1,
	.timeout = HZ / 10,
};

static int kt_i2c_probed;
static int kt_i2c_probe(struct i2c_client *c)
{
	kt_i2c_probed++;
	return 0;
}
static const struct i2c_device_id kt_i2c_ids[] = { { "kt_eeprom", 0 }, { } };
static struct i2c_driver kt_i2c_driver = {
	.driver = { .name = "kt_eeprom" },
	.probe = kt_i2c_probe,
	.id_table = kt_i2c_ids,
};

static void test_i2c(void)
{
	static const u8 hdr[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	struct i2c_adapter *adap;
	struct i2c_board_info info = { I2C_BOARD_INFO("kt_eeprom", KT_EEPROM_ADDR) };
	struct i2c_client *client;
	u8 off = 0, edid[128], w[3];
	struct i2c_msg msgs[2] = {
		{ .addr = KT_EEPROM_ADDR, .flags = 0, .len = 1, .buf = &off },
		{ .addr = KT_EEPROM_ADDR, .flags = I2C_M_RD, .len = 128, .buf = edid },
	};
	char path[64], b[64], ap[160];
	u8 sum = 0;
	int i;

	memset(&kt_ee, 0, sizeof(kt_ee));
	kt_ee.m_sda = kt_ee.m_scl = kt_ee.s_sda = kt_ee.l_sda = kt_ee.l_scl = 1;
	memcpy(kt_ee.mem, hdr, 8);
	for (i = 8; i < 127; i++)
		kt_ee.mem[i] = (u8)(i * 3);
	for (i = 0; i < 127; i++)
		sum += kt_ee.mem[i];
	kt_ee.mem[127] = (u8)(0x100 - sum);
	kt_ee.mem[200] = 0xA5;

	adap = kzalloc(sizeof(*adap), GFP_KERNEL);
	if (!adap)
		return;
	strscpy(adap->name, "kt-bitbang", sizeof(adap->name));
	adap->algo_data = &kt_bit;
	adap->dev.parent = &kt_pdev->dev;
	KT_EQ(i2c_bit_add_bus(adap), 0);
	KT_ASSERT(i2c_check_functionality(adap, I2C_FUNC_I2C | I2C_FUNC_SMBUS_BYTE_DATA),
		  "funcionalidad de algo-bit");
	snprintf(path, sizeof(path), "bus/i2c/devices/i2c-%d", adap->nr);
	kt_attr_path(path, "name", ap, sizeof(ap));
	kt_attr_read(ap, b, sizeof(b));
	KT_STREQ(b, "kt-bitbang\n");

	KT_EQ(i2c_add_driver(&kt_i2c_driver), 0);
	client = i2c_new_client_device(adap, &info);
	KT_ASSERT(!IS_ERR(client) && kt_i2c_probed == 1, "cliente kt_eeprom con probe");
	if (IS_ERR(client))
		goto out;
	snprintf(path, sizeof(path), "bus/i2c/devices/%d-0050", adap->nr);
	kt_attr_path(path, "name", ap, sizeof(ap));
	kt_attr_read(ap, b, sizeof(b));
	KT_STREQ(b, "kt_eeprom\n");

	/* Lectura del bloque EDID: escritura del offset + START repetido + 128. */
	KT_EQ(i2c_transfer(adap, msgs, 2), 2);
	KT_ASSERT(!memcmp(edid, hdr, 8), "cabecera EDID por I2C");
	sum = 0;
	for (i = 0; i < 128; i++)
		sum += edid[i];
	KT_EQ(sum, 0);
	KT_EQ(i2c_smbus_read_byte_data(client, 200), 0xA5);
	KT_EQ(i2c_smbus_write_byte_data(client, 10, 0x3C), 0);
	KT_EQ(kt_ee.mem[10], 0x3C);
	KT_EQ(i2c_smbus_read_word_data(client, 0), 0xff00);
	w[0] = 0x80; w[1] = 0x12; w[2] = 0x34;
	KT_EQ(i2c_master_send(client, (char *)w, 3), 3);
	KT_EQ(i2c_smbus_read_i2c_block_data(client, 0x80, 2, edid), 2);
	KT_ASSERT(edid[0] == 0x12 && edid[1] == 0x34, "bloque I2C");

	/* Nadie en 0x51: NAK de direccion (con reintentos). */
	{
		u8 x;
		struct i2c_msg m = { .addr = 0x51, .flags = I2C_M_RD, .len = 1, .buf = &x };
		int naks = kt_ee.naks;

		KT_EQ(i2c_transfer(adap, &m, 1), -ENXIO);
		KT_EQ(kt_ee.naks - naks, adap->retries + 1);
	}
	i2c_unregister_device(client);
out:
	i2c_del_driver(&kt_i2c_driver);
	i2c_del_adapter(adap);
	kfree(adap);
}

/* -------------------------------------------------------- backlight */

static int kt_bl_updates, kt_bl_last;

static int kt_bl_update(struct backlight_device *bd)
{
	kt_bl_updates++;
	kt_bl_last = backlight_get_brightness(bd);
	return 0;
}

static int kt_bl_get(struct backlight_device *bd)
{
	return bd->props.brightness / 2;	/* el "hardware" redondea */
}

static const struct backlight_ops kt_bl_ops = {
	.update_status = kt_bl_update,
	.get_brightness = kt_bl_get,
};

static void test_backlight(void)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW, .max_brightness = 100, .brightness = 40,
	};
	struct backlight_device *bd;
	char p[160], b[32];

	bd = backlight_device_register("kt_bl", &kt_pdev->dev, &kt_bl_updates, &kt_bl_ops, &props);
	KT_ASSERT(!IS_ERR(bd), "backlight_device_register");
	if (IS_ERR(bd))
		return;
	KT_ASSERT(bl_get_data(bd) == &kt_bl_updates, "bl_get_data");
	kt_attr_path("class/backlight/kt_bl", "max_brightness", p, sizeof(p));
	kt_attr_read(p, b, sizeof(b));
	KT_STREQ(b, "100\n");
	kt_attr_path("class/backlight/kt_bl", "brightness", p, sizeof(p));
	KT_EQ(kt_sysfs_store(p, "70\n", 3), 3);
	KT_EQ(bd->props.brightness, 70);
	KT_EQ(kt_bl_last, 70);
	KT_ASSERT(kt_sysfs_store(p, "101\n", 4) < 0, "brillo > max rechazado");
	kt_attr_path("class/backlight/kt_bl", "actual_brightness", p, sizeof(p));
	kt_attr_read(p, b, sizeof(b));
	KT_STREQ(b, "35\n");
	kt_attr_path("class/backlight/kt_bl", "type", p, sizeof(p));
	kt_attr_read(p, b, sizeof(b));
	KT_STREQ(b, "raw\n");
	kt_attr_path("class/backlight/kt_bl", "bl_power", p, sizeof(p));
	KT_EQ(kt_sysfs_store(p, "4\n", 2), 2);
	KT_EQ(kt_bl_last, 0);	/* apagada: brillo efectivo 0 */
	KT_EQ(backlight_enable(bd), 0);
	KT_EQ(kt_bl_last, 70);
	KT_ASSERT(backlight_device_get_by_name("kt_bl") == bd, "get_by_name");
	backlight_put(bd);
	backlight_device_unregister(bd);
	KT_EQ(kt_sysfs_get("class/backlight/kt_bl", NULL, NULL), KT_SYSFS_NONE);
}

/* ---------------------------------------------- debugfs y current */

static void test_debugfs(void)
{
	static u32 val = 7;
	struct dentry *dir, *f;

	dir = debugfs_create_dir("kt_k6", NULL);
	KT_ASSERT(!IS_ERR(dir), "debugfs_create_dir (montado en /sys/kernel/debug)");
	if (IS_ERR(dir))
		return;
	debugfs_create_u32("valor", 0644, dir, &val);
	KT_ASSERT(debugfs_lookup("valor", dir) != NULL, "debugfs_lookup");
	f = debugfs_create_dir("kt_k6", NULL);
	KT_EQ(PTR_ERR(f), -EEXIST);
	debugfs_remove(dir);
	KT_ASSERT(debugfs_lookup("kt_k6", NULL) == NULL, "borrado");
	dir = debugfs_create_dir("kt_k6", NULL);
	KT_ASSERT(!IS_ERR(dir), "se puede volver a crear");
	debugfs_remove(dir);
}

static int kt_thread_fn(void *data)
{
	struct task_struct *me = current;

	*(int *)data = !me->mm && (me->flags & PF_KTHREAD) && !signal_pending(me) &&
		       !fatal_signal_pending(me) && !strcmp(me->comm, "kt_k6_hilo");
	return 0;
}

static void test_current(void)
{
	struct task_struct *me = current, *t;
	int ok = 0;

	KT_ASSERT(me->pid > 0 && me->tgid > 0, "pid/tgid de Aurora (%d/%d)", me->pid, me->tgid);
	KT_ASSERT(!me->mm && !get_task_mm(me), "hilo del kernel sin mm");
	KT_ASSERT(!signal_pending(me), "sin senales");
	KT_ASSERT(capable(CAP_SYS_ADMIN), "el kernel tiene todas las capacidades");
	t = kthread_run(kt_thread_fn, &ok, "kt_k6_hilo");
	KT_ASSERT(!IS_ERR(t), "kthread_run");
	if (!IS_ERR(t)) {
		get_task_struct(t);
		msleep(50);
		kthread_stop(t);
		put_task_struct(t);
	}
	KT_ASSERT(ok, "kthread: comm, sin mm ni senales");
}

static int __init test_k6_init(void)
{
	(void)&kt_vrng_driver;	/* de lkpi_k5_toys.h, aqui sin uso */
	kt_pdev = platform_device_register_simple("kt_k6", -1, NULL, 0);
	KT_ASSERT(!IS_ERR(kt_pdev), "dispositivo de plataforma");
	if (IS_ERR(kt_pdev))
		return 0;
	test_scatterlist();
	test_dma_bounce();
	test_dma_sg();
	test_dma_edu();
	test_dma_coherent();
	test_vmap();
	test_dma_buf();
	test_firmware();
	test_i2c();
	test_backlight();
	test_debugfs();
	test_current();
	platform_device_unregister(kt_pdev);
	return 0;
}

static void __exit test_k6_exit(void)
{
}

module_init(test_k6_init);
module_exit(test_k6_exit);
MODULE_LICENSE("GPL");
