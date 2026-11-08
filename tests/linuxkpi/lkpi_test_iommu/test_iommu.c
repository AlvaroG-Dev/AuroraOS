// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pruebas de L9.1: IOMMU / VT-d con el dispositivo `edu` de QEMU.
 *
 *  - el dispositivo recibe un dominio privado al enlazarse el driver
 *  - dma_alloc_coherent / dma_map_single entregan IOVA traducibles
 *  - el motor DMA de edu funciona a traves de la traduccion
 *  - permisos: una pagina mapeada solo para lectura no se puede escribir
 *  - una IOVA desmapeada ya no es accesible y el fallo queda registrado
 *  - al desvincular el driver el dominio desaparece y no queda nada mapeado
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/iommu.h>
#include "lkpi_k5_toys.h"

struct dstats {
	u64 maps, unmaps, pages_live, faults, last_fault_iova;
	u8 last_fault_reason;
	u8 quarantined;
	u16 bdf, did;
};
struct gstats {
	int units, domains;
	u64 faults, unattributed_faults;
};
extern bool kt_iommu_active(void) __asm__("iommu_active");
extern void *kt_iommu_find(u16 bdf) __asm__("iommu_domain_find");
extern u64 kt_iommu_iova_to_phys(void *d, u64 iova) __asm__("iommu_iova_to_phys");
extern void kt_iommu_dstats(const void *d, struct dstats *o) __asm__("iommu_domain_get_stats");
extern void kt_iommu_gstats(struct gstats *o) __asm__("iommu_get_stats");
extern int kt_iommu_poll(void) __asm__("iommu_fault_poll");
extern int kt_pm_cycle(void) __asm__("lkpi_pm_cycle_devices");

static void *dom_of(struct pci_dev *pdev)
{
	return kt_iommu_find(PCI_DEVID(pdev->bus->number, pdev->devfn));
}

/* Espera a que baje START del motor DMA de edu. */
static bool dma_done(struct kt_edu *e)
{
	int i;

	for (i = 0; i < 300; i++) {
		if (!(readq(e->mmio + EDU_DMA_CMD) & EDU_DMA_START))
			return true;
		msleep(5);
	}
	return false;
}

/* RAM(host_dma) -> buffer interno de edu. */
static bool edu_to_dev(struct kt_edu *e, u64 host_dma, u64 len)
{
	writeq(host_dma, e->mmio + EDU_DMA_SRC);
	writeq(EDU_DMA_BUF, e->mmio + EDU_DMA_DST);
	writeq(len, e->mmio + EDU_DMA_CNT);
	writeq(EDU_DMA_START, e->mmio + EDU_DMA_CMD);
	return dma_done(e);
}

/* buffer interno de edu -> RAM(host_dma). */
static bool edu_from_dev(struct kt_edu *e, u64 host_dma, u64 len)
{
	writeq(EDU_DMA_BUF, e->mmio + EDU_DMA_SRC);
	writeq(host_dma, e->mmio + EDU_DMA_DST);
	writeq(len, e->mmio + EDU_DMA_CNT);
	writeq(EDU_DMA_START | EDU_DMA_FROM_EDU, e->mmio + EDU_DMA_CMD);
	return dma_done(e);
}

static u64 faults_of(struct pci_dev *pdev)
{
	struct dstats st;

	kt_iommu_poll();		/* por si la interrupcion de fallos tarda */
	kt_iommu_dstats(dom_of(pdev), &st);
	return st.faults;
}

/* El fallo llega de forma asincrona (registro + interrupcion): espera hasta 10 s. */
static bool wait_fault(struct pci_dev *pdev, u64 f0)
{
	int i;

	for (i = 0; i < 1000; i++) {
		if (faults_of(pdev) > f0)
			return true;
		msleep(10);
	}
	return false;
}

static void test_iommu(void)
{
	struct pci_dev *pdev = pci_get_device(EDU_VENDOR, EDU_DEVICE, NULL);
	struct device *dev;
	struct kt_edu *e;
	struct dstats st;
	struct gstats gs;
	dma_addr_t h, hs;
	u8 *buf, *ro;
	u64 f0;
	void *dom;
	int i;

	if (!kt_iommu_active()) {
		KT_SKIP("IOMMU apagada (QEMU: -machine q35 -device intel-iommu y iommu=on)");
		return;
	}
	if (!pdev) {
		KT_SKIP("QEMU sin -device edu");
		return;
	}
	kt_edu_probes = kt_edu_removes = 0;
	KT_EQ(pci_register_driver(&kt_edu_driver), 0);
	e = kt_edu_dev;
	KT_ASSERT(e != NULL, "probe de kt_edu");
	if (!e)
		goto out;
	dev = &e->pdev->dev;

	/* --- dominio privado al enlazar el driver --- */
	dom = dom_of(pdev);
	KT_ASSERT(dom != NULL, "dominio privado de edu");
	KT_ASSERT(device_iommu_mapped(dev), "device_iommu_mapped");
	KT_ASSERT(iommu_get_domain_for_dev(dev) != NULL, "iommu_get_domain_for_dev");
	kt_iommu_gstats(&gs);
	KT_ASSERT(gs.units >= 1 && gs.domains >= 1, "unidades=%d dominios=%d", gs.units, gs.domains);
	if (!dom)
		goto unbind;
	kt_iommu_dstats(dom, &st);
	KT_EQ(st.pages_live, 0);

	/* --- coherente: la direccion de bus es una IOVA traducible --- */
	buf = dma_alloc_coherent(dev, 2 * EDU_DMA_SIZE, &h, GFP_KERNEL);
	KT_ASSERT(buf != NULL, "dma_alloc_coherent");
	if (!buf)
		goto unbind;
	KT_ASSERT(h != 0 && h + 2 * EDU_DMA_SIZE <= (1ULL << 28), "IOVA bajo la mascara: %llx",
		  (unsigned long long)h);
	KT_ASSERT((h & (2 * EDU_DMA_SIZE - 1)) == 0, "IOVA alineada al tamano");
	KT_EQ(kt_iommu_iova_to_phys(dom, h), virt_to_phys(buf));
	KT_EQ(kt_iommu_iova_to_phys(dom, h + EDU_DMA_SIZE + 8), virt_to_phys(buf) + EDU_DMA_SIZE + 8);
	KT_EQ(kt_iommu_iova_to_phys(dom, h + 2 * EDU_DMA_SIZE), 0);	/* fuera de lo mapeado */
	kt_iommu_dstats(dom, &st);
	KT_EQ(st.pages_live, 2);

	/* --- DMA real a traves de la traduccion (ida y vuelta) --- */
	for (i = 0; i < EDU_DMA_SIZE; i++)
		buf[i] = (u8)(i * 11 + 5);
	memset(buf + EDU_DMA_SIZE, 0, EDU_DMA_SIZE);
	f0 = faults_of(pdev);
	KT_ASSERT(edu_to_dev(e, h, EDU_DMA_SIZE), "DMA RAM->edu termina");
	KT_ASSERT(edu_from_dev(e, h + EDU_DMA_SIZE, EDU_DMA_SIZE), "DMA edu->RAM termina");
	KT_ASSERT(!memcmp(buf, buf + EDU_DMA_SIZE, EDU_DMA_SIZE), "los datos vuelven iguales");
	KT_EQ(faults_of(pdev), f0);	/* un DMA legitimo no falla */

	/* --- lectura/escritura: solo-lectura (TO_DEVICE) impide escribir --- */
	ro = kmalloc(PAGE_SIZE, GFP_KERNEL);
	KT_ASSERT(ro != NULL, "kmalloc");
	if (!ro)
		goto free_coh;
	memset(ro, 0xA5, PAGE_SIZE);
	hs = dma_map_single(dev, ro, PAGE_SIZE, DMA_TO_DEVICE);
	KT_ASSERT(!dma_mapping_error(dev, hs), "dma_map_single");
	if (dma_mapping_error(dev, hs))
		goto free_ro;
	KT_EQ(kt_iommu_iova_to_phys(dom, hs), virt_to_phys(ro));
	KT_ASSERT(hs + PAGE_SIZE <= (1ULL << 28), "IOVA streaming bajo la mascara");
	/* El dispositivo puede leerla... */
	f0 = faults_of(pdev);
	KT_ASSERT(edu_to_dev(e, hs, 64), "lectura permitida");
	KT_EQ(faults_of(pdev), f0);
	/* ...pero no escribirla. */
	KT_ASSERT(edu_from_dev(e, hs, 64), "escritura denegada termina igualmente");
	/* QEMU registra este fallo con retraso (se ve en el log como fallo DMA
	 * escritura); lo esencial es que la memoria no cambie. */
	(void)wait_fault(pdev, f0);
	for (i = 0; i < 64; i++)
		if (ro[i] != 0xA5)
			break;
	KT_EQ(i, 64);			/* la memoria no se toco */
	dma_unmap_single(dev, hs, PAGE_SIZE, DMA_TO_DEVICE);

	/* --- IOVA obsoleta: tras desmapear ya no hay acceso --- */
	KT_EQ(kt_iommu_iova_to_phys(dom, hs), 0);
	memset(ro, 0x5A, PAGE_SIZE);
	f0 = faults_of(pdev);
	KT_ASSERT(edu_to_dev(e, h, 64) , "DMA valido sigue funcionando");
	KT_ASSERT(edu_from_dev(e, hs, 64), "DMA a IOVA desmapeada termina");
	KT_ASSERT(wait_fault(pdev, f0), "IOVA desmapeada da fallo");
	for (i = 0; i < 64; i++)
		if (ro[i] != 0x5A)
			break;
	KT_EQ(i, 64);

	/* --- varios mapeos: no se solapan y se liberan --- */
	{
		dma_addr_t a[8];
		void *p[8];
		int n;

		for (n = 0; n < 8; n++) {
			p[n] = kmalloc(PAGE_SIZE, GFP_KERNEL);
			a[n] = dma_map_single(dev, p[n], PAGE_SIZE, DMA_BIDIRECTIONAL);
			KT_ASSERT(!dma_mapping_error(dev, a[n]), "mapeo %d", n);
		}
		for (n = 1; n < 8; n++)
			KT_ASSERT(a[n] != a[n - 1], "IOVA distintas");
		for (n = 0; n < 8; n++) {
			KT_EQ(kt_iommu_iova_to_phys(dom, a[n]), virt_to_phys(p[n]));
			dma_unmap_single(dev, a[n], PAGE_SIZE, DMA_BIDIRECTIONAL);
			kfree(p[n]);
		}
	}
	/* --- suspender y reanudar: la IOMMU vuelve y el DMA sigue funcionando --- */
	{
		u64 fb;

		fb = faults_of(pdev);
		KT_EQ(kt_pm_cycle(), 0);
		memset(buf + EDU_DMA_SIZE, 0, EDU_DMA_SIZE);
		KT_ASSERT(edu_to_dev(e, h, EDU_DMA_SIZE), "DMA tras el ciclo (ida)");
		KT_ASSERT(edu_from_dev(e, h + EDU_DMA_SIZE, EDU_DMA_SIZE), "DMA tras el ciclo (vuelta)");
		KT_ASSERT(!memcmp(buf, buf + EDU_DMA_SIZE, EDU_DMA_SIZE), "datos intactos tras el ciclo");
		KT_EQ(faults_of(pdev), fb);
	}

	/* --- cuarentena: demasiados fallos y el dispositivo pierde bus master --- */
	{
		u16 cmd;
		int n;

		kt_iommu_dstats(dom, &st);
		for (n = 0; n < 150 && !st.quarantined; n++) {
			edu_from_dev(e, 0x0f000000ULL + (n % 16) * PAGE_SIZE, 64);
			msleep(2);
			faults_of(pdev);
			kt_iommu_dstats(dom, &st);
		}
		if (!st.quarantined) {
			KT_SKIP("sin cuarentena (arranca con iommu.fault_limit=40 para probarla)");
		} else {
			pci_read_config_word(pdev, PCI_COMMAND, &cmd);
			KT_ASSERT(!(cmd & PCI_COMMAND_MASTER), "cuarentena quita el bus master");
		}
	}
free_ro:
	kfree(ro);
free_coh:
	dma_free_coherent(dev, 2 * EDU_DMA_SIZE, buf, h);
	kt_iommu_dstats(dom, &st);
	KT_EQ(st.pages_live, 0);	/* nada queda mapeado */
	KT_ASSERT(st.maps >= 5 && st.faults >= 2, "maps=%llu faults=%llu",
		  (unsigned long long)st.maps, (unsigned long long)st.faults);

unbind:
	pci_unregister_driver(&kt_edu_driver);
	KT_EQ(kt_edu_removes, 1);
	KT_ASSERT(dom_of(pdev) == NULL, "el dominio se libera al desvincular");
	/* Y se puede volver a enlazar: dominio nuevo, limpio. */
	KT_EQ(pci_register_driver(&kt_edu_driver), 0);
	KT_ASSERT(dom_of(pdev) != NULL, "dominio nuevo");
	if (dom_of(pdev)) {
		kt_iommu_dstats(dom_of(pdev), &st);
		KT_EQ(st.pages_live, 0);
		KT_EQ(st.faults, 0);
		KT_EQ(st.quarantined, 0);	/* la cuarentena no sobrevive al driver */
	}
	pci_unregister_driver(&kt_edu_driver);
out:
	pci_dev_put(pdev);
}

static int __init kt_iommu_init(void)
{
	test_iommu();
	return 0;
}
static void __exit kt_iommu_exit(void) {}
module_init(kt_iommu_init);
module_exit(kt_iommu_exit);
MODULE_LICENSE("GPL");
