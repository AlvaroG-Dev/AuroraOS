// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pruebas de K5 (hito de L3): drivers PCI de Linux de juguete sobre
 * dispositivos de QEMU (lkpi_k5_toys.h). edu: probe, MMIO, BARs y regiones,
 * INTx (con y sin hilo ONESHOT), MSI (disable/enable con IRQ pendiente,
 * hilo) y DMA en los dos sentidos con su motor. virtio-rng: MSI-X con dos
 * vectores y una virtqueue (DMA del dispositivo + interrupcion). Si QEMU no
 * tiene edu o virtio-rng-pci (scripts/qemu-drive.py los anade), se omite.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include "lkpi_k5_toys.h"

static int kt_sys(const char *path, char *out, size_t outlen)
{
	char *data = NULL;
	size_t len = 0;
	int kind = kt_sysfs_get(path, &data, &len);

	if (out && outlen) {
		out[0] = '\0';
		if (data)
			strscpy(out, data, outlen);
	}
	if (data)
		kt_sysfs_free(data);
	return kind;
}

/* ------------------------------------------------- PCI sin driver */

static void test_pci_lookup(struct pci_dev *pdev)
{
	u16 vendor;
	u32 v;
	struct pci_dev *again;

	KT_EQ(pdev->vendor, EDU_VENDOR);
	KT_EQ(pdev->device, EDU_DEVICE);
	KT_EQ(pci_read_config_word(pdev, PCI_VENDOR_ID, &vendor), PCIBIOS_SUCCESSFUL);
	KT_EQ(vendor, EDU_VENDOR);
	KT_EQ(pci_read_config_dword(pdev, 0x100, &v), PCIBIOS_BAD_REGISTER_NUMBER);
	KT_EQ(v, 0xffffffff);
	KT_ASSERT(pdev->msi_cap && !pdev->msix_cap, "edu: MSI si, MSI-X no");
	KT_EQ(pci_msi_vec_count(pdev), 1);
	KT_EQ(pci_msix_vec_count(pdev), -EINVAL);
	KT_EQ(pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_ERR), 0);
	KT_ASSERT(!pci_is_pcie(pdev), "edu no es PCIe en la maquina pc");
	KT_ASSERT(!pcie_has_flr(pdev), "sin FLR");
	KT_EQ(pcie_flr(pdev), -ENOTTY);
	KT_EQ(pci_reset_function(pdev), -ENOTTY);
	KT_ASSERT(pci_device_is_present(pdev), "presente");
	KT_STREQ(pci_name(pdev), dev_name(&pdev->dev));
	again = pci_get_domain_bus_and_slot(0, pdev->bus->number, pdev->devfn);
	KT_ASSERT(again == pdev, "pci_get_domain_bus_and_slot da el mismo pci_dev");
	pci_dev_put(again);
}

/* ---------------------------------------------------------------- edu */

static void test_edu_mmio(struct kt_edu *e)
{
	struct pci_dev *pdev = e->pdev;
	u32 id = readl(e->mmio + EDU_ID);
	int i;

	KT_EQ(id & 0xff, 0xed);
	writel(0x12345678, e->mmio + EDU_LIVENESS);
	KT_EQ(readl(e->mmio + EDU_LIVENESS), ~0x12345678U);
	writel(5, e->mmio + EDU_FACT);
	for (i = 0; i < 1000 && (readl(e->mmio + EDU_STATUS) & EDU_STATUS_COMPUTING); i++)
		msleep(1);
	KT_EQ(readl(e->mmio + EDU_FACT), 120);

	KT_EQ(pci_resource_len(pdev, 0), 1 << 20);
	KT_ASSERT(pci_resource_flags(pdev, 0) & IORESOURCE_MEM, "BAR0 MMIO");
	KT_ASSERT(pdev->is_busmaster, "bus master");
	KT_ASSERT(pci_is_enabled(pdev), "habilitado");
	/* La region del BAR0 ya la tiene pcim_iomap_region. */
	KT_EQ(pci_request_region(pdev, 0, "otro"), -EBUSY);
}

static void test_edu_intx(struct kt_edu *e)
{
	struct pci_dev *pdev = e->pdev;
	int n, irq;

	if (!pdev->irq) {
		KT_SKIP("edu sin linea INTx asignada por el firmware");
		return;
	}
	n = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_INTX);
	KT_EQ(n, 1);
	irq = pci_irq_vector(pdev, 0);
	KT_EQ(irq, pdev->irq);
	KT_ASSERT(irq > 0 && irq < 16, "INTx en el IOAPIC: %d", irq);

	reinit_completion(&e->done);
	atomic_set(&e->handled, 0);
	KT_EQ(request_irq(irq, kt_edu_irq, IRQF_SHARED, "kt_edu", e), 0);
	kt_edu_raise(e, 0x1);
	KT_ASSERT(kt_edu_wait(e), "INTx: no llego la interrupcion");
	KT_ASSERT(e->saw_hardirq, "INTx: el handler no corre en hardirq");
	KT_EQ(e->last_status & 0x1, 0x1);
	KT_EQ(readl(e->mmio + EDU_IRQ_STATUS), 0);
	KT_ASSERT(free_irq(irq, e) != NULL, "free_irq");

	/* Sin primario en una linea de nivel hace falta ONESHOT. */
	KT_EQ(request_threaded_irq(irq, NULL, kt_edu_thread, IRQF_SHARED, "kt_edu", e), -EINVAL);
	reinit_completion(&e->done);
	atomic_set(&e->threads, 0);
	KT_EQ(request_threaded_irq(irq, NULL, kt_edu_thread, IRQF_SHARED | IRQF_ONESHOT,
				   "kt_edu", e), 0);
	kt_edu_raise(e, 0x2);
	KT_ASSERT(kt_edu_wait(e), "INTx ONESHOT: el hilo no corrio");
	KT_EQ(atomic_read(&e->threads), 1);
	KT_ASSERT(e->thread_ctx_ok, "contexto del hilo");
	KT_EQ(e->last_status & 0x2, 0x2);
	/* La linea se desenmascara al terminar el hilo: otra vez. */
	reinit_completion(&e->done);
	kt_edu_raise(e, 0x2);
	KT_ASSERT(kt_edu_wait(e), "INTx ONESHOT: segunda interrupcion");
	synchronize_irq(irq);
	KT_EQ(atomic_read(&e->threads), 2);
	free_irq(irq, e);
	pci_free_irq_vectors(pdev);
}

static void test_edu_dma(struct kt_edu *e)
{
	struct pci_dev *pdev = e->pdev;
	dma_addr_t h;
	u8 *buf;
	int i;

	buf = dma_alloc_coherent(&pdev->dev, 2 * EDU_DMA_SIZE, &h, GFP_KERNEL);
	KT_ASSERT(buf != NULL, "dma_alloc_coherent");
	if (!buf)
		return;
	KT_ASSERT(h + 2 * EDU_DMA_SIZE <= (1ULL << 28), "DMA bajo la mascara de 28 bits: %llx",
		  (unsigned long long)h);
	KT_ASSERT((h & (2 * EDU_DMA_SIZE - 1)) == 0, "alineado al tamano");
	for (i = 0; i < EDU_DMA_SIZE; i++)
		buf[i] = (u8)(i * 7 + 3);

	/* RAM -> edu, sin interrupcion: esperar a que baje START. */
	writeq(h, e->mmio + EDU_DMA_SRC);
	writeq(EDU_DMA_BUF, e->mmio + EDU_DMA_DST);
	writeq(EDU_DMA_SIZE, e->mmio + EDU_DMA_CNT);
	writeq(EDU_DMA_START, e->mmio + EDU_DMA_CMD);
	for (i = 0; i < 300 && (readq(e->mmio + EDU_DMA_CMD) & EDU_DMA_START); i++)
		msleep(10);
	KT_ASSERT(!(readq(e->mmio + EDU_DMA_CMD) & EDU_DMA_START), "DMA RAM->edu termina");

	/* edu -> RAM con interrupcion (bit 0x100 del estado). */
	reinit_completion(&e->done);
	e->last_status = 0;
	writeq(EDU_DMA_BUF, e->mmio + EDU_DMA_SRC);
	writeq(h + EDU_DMA_SIZE, e->mmio + EDU_DMA_DST);
	writeq(EDU_DMA_SIZE, e->mmio + EDU_DMA_CNT);
	writeq(EDU_DMA_START | EDU_DMA_FROM_EDU | EDU_DMA_IRQ, e->mmio + EDU_DMA_CMD);
	KT_ASSERT(kt_edu_wait(e), "DMA edu->RAM: no llego la interrupcion");
	KT_EQ(e->last_status & EDU_DMA_IRQ_BIT, EDU_DMA_IRQ_BIT);
	KT_ASSERT(!memcmp(buf, buf + EDU_DMA_SIZE, EDU_DMA_SIZE), "los datos vuelven iguales");
	dma_free_coherent(&pdev->dev, 2 * EDU_DMA_SIZE, buf, h);
}

static void test_edu_msi(struct kt_edu *e)
{
	struct pci_dev *pdev = e->pdev;
	int n, irq, before;

	n = pci_alloc_irq_vectors(pdev, 1, 4, PCI_IRQ_MSI);
	KT_EQ(n, 1);
	irq = pci_irq_vector(pdev, 0);
	KT_ASSERT(irq >= 0x60 && irq <= 0x7f, "vector MSI: %#x", irq);
	KT_EQ(pdev->irq, irq);
	KT_ASSERT(pdev->msi_enabled, "msi_enabled");
	KT_EQ(pci_irq_vector(pdev, 1), -EINVAL);
	KT_EQ(pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI), -EINVAL);	/* ya tiene */

	reinit_completion(&e->done);
	atomic_set(&e->handled, 0);
	KT_EQ(request_irq(irq, kt_edu_irq, 0, "kt_edu", e), 0);
	KT_EQ(request_irq(irq, kt_edu_irq, 0, "kt_edu2", e), -EBUSY);	/* no compartida */
	kt_edu_raise(e, 0x1);
	KT_ASSERT(kt_edu_wait(e), "MSI: no llego la interrupcion");
	KT_ASSERT(e->saw_hardirq, "MSI: el handler no corre en hardirq");
	KT_EQ(e->handler_cpu, lkpi_irq_get_effective_cpu(irq));

	/* Deshabilitada: la MSI (flanco) queda pendiente y llega al habilitar. */
	disable_irq(irq);
	reinit_completion(&e->done);
	before = atomic_read(&e->handled);
	kt_edu_raise(e, 0x4);
	msleep(50);
	KT_EQ(atomic_read(&e->handled), before);
	disable_irq(irq);	/* anidado */
	enable_irq(irq);
	msleep(20);
	KT_EQ(atomic_read(&e->handled), before);
	enable_irq(irq);
	KT_ASSERT(kt_edu_wait(e), "MSI pendiente no reproducida al habilitar");
	KT_EQ(e->last_status & 0x4, 0x4);

	test_edu_dma(e);

	/* Con hilo: primario en hardirq, hilo despues. */
	KT_STREQ(free_irq(irq, e), "kt_edu");
	reinit_completion(&e->done);
	atomic_set(&e->threads, 0);
	KT_EQ(request_threaded_irq(irq, kt_edu_primary, kt_edu_thread, 0, "kt_edu-t", e), 0);
	kt_edu_raise(e, 0x8);
	KT_ASSERT(kt_edu_wait(e), "MSI con hilo");
	synchronize_irq(irq);
	KT_EQ(atomic_read(&e->threads), 1);
	KT_ASSERT(e->in_primary_ok && e->thread_ctx_ok, "contextos primario/hilo");
	free_irq(irq, e);

	/* devm_request_irq: lo libera devres al desvincular el driver. */
	KT_EQ(devm_request_irq(&pdev->dev, irq, kt_edu_irq, 0, "kt_edu-devm", e), 0);
	reinit_completion(&e->done);
	kt_edu_raise(e, 0x10);
	KT_ASSERT(kt_edu_wait(e), "devm_request_irq");
	devm_free_irq(&pdev->dev, irq, e);
	pci_free_irq_vectors(pdev);
	KT_ASSERT(!pdev->msi_enabled, "MSI apagado al liberar");
}

static void test_edu(void)
{
	struct pci_dev *pdev = pci_get_device(EDU_VENDOR, EDU_DEVICE, NULL);
	char path[96], b[96];
	struct kt_edu *e;
	resource_size_t start, len;
	struct resource *r;

	if (!pdev) {
		KT_SKIP("QEMU sin -device edu");
		return;
	}
	test_pci_lookup(pdev);
	kt_edu_probes = kt_edu_removes = 0;
	KT_EQ(pci_register_driver(&kt_edu_driver), 0);
	KT_EQ(kt_edu_probes, 1);
	e = kt_edu_dev;
	KT_ASSERT(e != NULL, "probe de kt_edu");
	if (!e)
		goto out;
	KT_ASSERT(e->pdev->driver == &kt_edu_driver, "pdev->driver");

	snprintf(path, sizeof(path), "bus/pci/drivers/kt_edu/%s", pci_name(e->pdev));
	KT_EQ(kt_sys(path, b, sizeof(b)), KT_SYSFS_LINK);
	snprintf(path, sizeof(path), "/sys/devices/pci0000:00/%s", pci_name(e->pdev));
	KT_STREQ(b, path);
	snprintf(path, sizeof(path), "bus/pci/devices/%s/driver", pci_name(e->pdev));
	KT_EQ(kt_sys(path, b, sizeof(b)), KT_SYSFS_LINK);
	KT_STREQ(b, "/sys/bus/pci/drivers/kt_edu");
	snprintf(path, sizeof(path), "devices/pci0000:00/%s/vendor", pci_name(e->pdev));
	KT_EQ(kt_sys(path, b, sizeof(b)), KT_SYSFS_ATTR);
	KT_STREQ(b, "0x1234\n");

	test_edu_mmio(e);
	test_edu_intx(e);
	test_edu_msi(e);

	start = pci_resource_start(e->pdev, 0);
	len = pci_resource_len(e->pdev, 0);
	pci_unregister_driver(&kt_edu_driver);
	KT_EQ(kt_edu_removes, 1);
	KT_ASSERT(kt_edu_dev == NULL, "remove");
	snprintf(path, sizeof(path), "bus/pci/devices/%s/driver", pci_name(pdev));
	KT_EQ(kt_sys(path, NULL, 0), KT_SYSFS_NONE);
	/* pcim_*: la region y el mapeo los libero devres. */
	r = request_mem_region(start, len, "kt");
	KT_ASSERT(r != NULL, "BAR0 libre tras desvincular");
	if (r)
		release_mem_region(start, len);
	/* Se puede volver a registrar (y Aurora lo vuelve a ofrecer). */
	KT_EQ(pci_register_driver(&kt_edu_driver), 0);
	KT_EQ(kt_edu_probes, 2);
	pci_unregister_driver(&kt_edu_driver);
	KT_EQ(kt_edu_removes, 2);
out:
	pci_dev_put(pdev);
}

/* -------------------------------------------------------- virtio-rng */

static void test_vrng(void)
{
	struct pci_dev *pdev = pci_get_device(VRNG_VENDOR, VRNG_DEVICE_TRANS, NULL);
	struct kt_vrng *v;
	int i, got, nonzero = 0;

	if (!pdev)
		pdev = pci_get_device(VRNG_VENDOR, VRNG_DEVICE_MODERN, NULL);
	if (!pdev) {
		KT_SKIP("QEMU sin -device virtio-rng-pci");
		return;
	}
	KT_ASSERT(pdev->msix_cap != 0, "virtio-rng con MSI-X");
	KT_ASSERT(pci_msix_vec_count(pdev) >= 2, "tabla MSI-X: %d", pci_msix_vec_count(pdev));
	kt_vrng_removes = 0;
	KT_EQ(pci_register_driver(&kt_vrng_driver), 0);
	v = kt_vrng_dev;
	KT_ASSERT(v != NULL, "probe de kt_vrng");
	if (!v)
		goto out;
	KT_ASSERT(v->pdev->msix_enabled && !v->pdev->msi_enabled, "MSI-X activo");
	KT_ASSERT(v->qirq >= 0x60 && v->qirq <= 0x7f && v->cirq != v->qirq, "vectores %#x %#x",
		  v->cirq, v->qirq);
	KT_ASSERT(pci_irq_get_affinity(v->pdev, 1) != NULL, "afinidad del vector 1");
	for (i = 0; i < 4; i++) {
		got = kt_vrng_request(v);
		KT_EQ(got, VRNG_BUF);
		for (int j = 0; j < VRNG_BUF; j++)
			nonzero += v->buf[j] != 0;
	}
	KT_ASSERT(nonzero > 4 * VRNG_BUF / 2, "bytes aleatorios recibidos: %d", nonzero);
	KT_EQ(atomic_read(&v->irqs), 4);
	KT_ASSERT(v->handler_hardirq, "handler MSI-X en hardirq");
	KT_EQ(v->handler_cpu, lkpi_irq_get_effective_cpu(v->qirq));
	pci_unregister_driver(&kt_vrng_driver);
	KT_EQ(kt_vrng_removes, 1);
out:
	pci_dev_put(pdev);
}

static int __init test_k5_init(void)
{
	test_edu();
	test_vrng();
	return 0;
}

static void __exit test_k5_exit(void)
{
}

module_init(test_k5_init);
module_exit(test_k5_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Pruebas LinuxKPI K5 (PCI, IRQ, DMA provisional)");
