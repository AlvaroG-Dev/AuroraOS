/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Drivers PCI de Linux "de juguete" para las pruebas de K5 (hito de L3),
 * escritos contra la API de Linux de la capa como lo haria un driver real:
 *
 *  - kt_edu: el dispositivo `edu` de QEMU (1234:11e8). MMIO del BAR0
 *    (identificacion, prueba de vida, factorial), INTx o MSI de un vector
 *    (registro de "raise"/"ack") y DMA con su motor (buffer interno en
 *    0x40000, mascara de DMA de 28 bits).
 *  - kt_vrng: virtio-rng-pci por la interfaz moderna (virtio 1.x): MSI-X
 *    con dos vectores, una virtqueue en memoria coherente y peticiones de
 *    bytes aleatorios (el dispositivo escribe por DMA y avisa por MSI-X).
 *
 * Los registros de edu son los de hw/misc/edu.c de QEMU; los de virtio, los
 * de la especificacion virtio 1.x (capabilities de fabricante 0x09).
 */
#ifndef _LKPI_K5_TOYS_H
#define _LKPI_K5_TOYS_H

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include "lkpi_test.h"

#define KT_TIMEOUT msecs_to_jiffies(3000)

/* ------------------------------------------------------------ edu */

#define EDU_VENDOR 0x1234
#define EDU_DEVICE 0x11e8
#define EDU_ID 0x00
#define EDU_LIVENESS 0x04
#define EDU_FACT 0x08
#define EDU_STATUS 0x20
#define  EDU_STATUS_COMPUTING 0x01
#define  EDU_STATUS_IRQFACT 0x80
#define EDU_IRQ_STATUS 0x24
#define EDU_IRQ_RAISE 0x60
#define EDU_IRQ_ACK 0x64
#define EDU_DMA_SRC 0x80
#define EDU_DMA_DST 0x88
#define EDU_DMA_CNT 0x90
#define EDU_DMA_CMD 0x98
#define  EDU_DMA_START 0x1
#define  EDU_DMA_FROM_EDU 0x2
#define  EDU_DMA_IRQ 0x4
#define EDU_DMA_BUF 0x40000
#define EDU_DMA_IRQ_BIT 0x100
#define EDU_DMA_SIZE 4096

struct kt_edu {
	struct pci_dev *pdev;
	void __iomem *mmio;
	int irq;
	struct completion done;
	atomic_t handled;		/* handlers primarios que atendieron algo */
	atomic_t threads;		/* thread_fn ejecutados */
	u32 last_status;
	bool saw_hardirq;		/* in_hardirq() e IRQ desactivadas en el handler */
	bool thread_ctx_ok;		/* el hilo no es hardirq y puede dormir */
	int handler_cpu;
	bool in_primary_ok;
};

static struct kt_edu *kt_edu_dev;
static int kt_edu_probes, kt_edu_removes;

static irqreturn_t kt_edu_irq(int irq, void *data)
{
	struct kt_edu *e = data;
	u32 st = readl(e->mmio + EDU_IRQ_STATUS);

	if (!st)
		return IRQ_NONE;
	writel(st, e->mmio + EDU_IRQ_ACK);	/* baja la linea antes del EOI */
	e->last_status = st;
	e->saw_hardirq = in_hardirq() && irqs_disabled() && in_interrupt() && !in_task();
	e->handler_cpu = smp_processor_id();
	atomic_inc(&e->handled);
	complete(&e->done);
	return IRQ_HANDLED;
}

/* Hilo de una IRQ con hilo: la causa la atiende aqui (ONESHOT en INTx). */
static irqreturn_t kt_edu_thread(int irq, void *data)
{
	struct kt_edu *e = data;
	u32 st = readl(e->mmio + EDU_IRQ_STATUS);

	e->thread_ctx_ok = !in_hardirq() && !irqs_disabled() && in_task();
	msleep(1);			/* un hilo de IRQ puede dormir */
	if (st)
		writel(st, e->mmio + EDU_IRQ_ACK);
	e->last_status = st;
	atomic_inc(&e->threads);
	complete(&e->done);
	return IRQ_HANDLED;
}

/* Primario de la variante con hilo en MSI: anota y despierta al hilo. */
static irqreturn_t kt_edu_primary(int irq, void *data)
{
	struct kt_edu *e = data;

	e->in_primary_ok = in_hardirq();
	return IRQ_WAKE_THREAD;
}

static void kt_edu_raise(struct kt_edu *e, u32 bits)
{
	writel(bits, e->mmio + EDU_IRQ_RAISE);
}

static int kt_edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct kt_edu *e;
	int rc;

	kt_edu_probes++;
	e = devm_kzalloc(&pdev->dev, sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;
	e->pdev = pdev;
	init_completion(&e->done);
	rc = pcim_enable_device(pdev);
	if (rc)
		return rc;
	e->mmio = pcim_iomap_region(pdev, 0, "kt_edu");
	if (IS_ERR(e->mmio))
		return PTR_ERR(e->mmio);
	pci_set_master(pdev);
	/* edu solo ve 28 bits de direccion (propiedad dma_mask de QEMU). */
	rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(28));
	if (rc)
		return rc;
	pci_set_drvdata(pdev, e);
	kt_edu_dev = e;
	return 0;
}

static void kt_edu_remove(struct pci_dev *pdev)
{
	kt_edu_removes++;
	kt_edu_dev = NULL;
}

static const struct pci_device_id kt_edu_ids[] = {
	{ PCI_DEVICE(EDU_VENDOR, EDU_DEVICE) },
	{ }
};
MODULE_DEVICE_TABLE(pci, kt_edu_ids);

static struct pci_driver kt_edu_driver = {
	.name = "kt_edu",
	.id_table = kt_edu_ids,
	.probe = kt_edu_probe,
	.remove = kt_edu_remove,
};

/* Espera una interrupcion; false si no llega. */
static bool kt_edu_wait(struct kt_edu *e)
{
	return wait_for_completion_timeout(&e->done, KT_TIMEOUT) != 0;
}

/* ------------------------------------------------------- virtio-rng */

#define VRNG_VENDOR 0x1af4
#define VRNG_DEVICE_TRANS 0x1005
#define VRNG_DEVICE_MODERN 0x1044

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
/* struct virtio_pci_cap */
#define VCAP_CFG_TYPE 3
#define VCAP_BAR 4
#define VCAP_OFFSET 8
#define VCAP_NOTIFY_MULT 16
/* struct virtio_pci_common_cfg */
#define VCOM_DFSELECT 0x00
#define VCOM_DF 0x04
#define VCOM_GFSELECT 0x08
#define VCOM_GF 0x0c
#define VCOM_MSIX 0x10
#define VCOM_NUMQ 0x12
#define VCOM_STATUS 0x14
#define VCOM_Q_SELECT 0x16
#define VCOM_Q_SIZE 0x18
#define VCOM_Q_MSIX 0x1a
#define VCOM_Q_ENABLE 0x1c
#define VCOM_Q_NOFF 0x1e
#define VCOM_Q_DESC 0x20
#define VCOM_Q_AVAIL 0x28
#define VCOM_Q_USED 0x30
#define VSTAT_ACK 1
#define VSTAT_DRIVER 2
#define VSTAT_DRIVER_OK 4
#define VSTAT_FEATURES_OK 8
#define VIRTIO_MSI_NO_VECTOR 0xffff
#define VRING_DESC_F_WRITE 2

#define VRNG_QSIZE_MAX 8
#define VRNG_BUF 64

struct kt_vring_desc {
	__le64 addr;
	__le32 len;
	__le16 flags;
	__le16 next;
};

struct kt_vrng {
	struct pci_dev *pdev;
	void __iomem *common;
	void __iomem *notify;
	int nvec;
	int qirq, cirq;
	unsigned int qsize;
	void *ring;			/* pagina coherente: desc, avail, used, datos */
	dma_addr_t ring_dma;
	struct kt_vring_desc *desc;
	__le16 *avail;			/* flags, idx, ring[] */
	__le16 *used;			/* flags, idx, luego {id, len}[] */
	u8 *buf;
	u16 avail_idx, used_seen;
	struct completion done;
	atomic_t irqs;
	int handler_cpu;
	bool handler_hardirq;
};

static struct kt_vrng *kt_vrng_dev;
static int kt_vrng_removes;

static irqreturn_t kt_vrng_irq(int irq, void *data)
{
	struct kt_vrng *v = data;
	u16 idx = le16_to_cpu(READ_ONCE(v->used[1]));

	if (idx == v->used_seen)
		return IRQ_NONE;
	v->used_seen = idx;
	v->handler_cpu = smp_processor_id();
	v->handler_hardirq = in_hardirq();
	atomic_inc(&v->irqs);
	complete(&v->done);
	return IRQ_HANDLED;
}

static irqreturn_t kt_vrng_config_irq(int irq, void *data)
{
	return IRQ_HANDLED;
}

/* Busca la capability virtio de un tipo y devuelve su region mapeada. */
static void __iomem *kt_vrng_map_cap(struct pci_dev *pdev, u8 type, u32 *notify_mult)
{
	u8 pos;

	for (pos = pci_find_capability(pdev, PCI_CAP_ID_VNDR); pos;
	     pos = pci_find_next_capability(pdev, pos, PCI_CAP_ID_VNDR)) {
		u8 t, bar;
		u32 off;

		pci_read_config_byte(pdev, pos + VCAP_CFG_TYPE, &t);
		if (t != type)
			continue;
		pci_read_config_byte(pdev, pos + VCAP_BAR, &bar);
		pci_read_config_dword(pdev, pos + VCAP_OFFSET, &off);
		if (notify_mult)
			pci_read_config_dword(pdev, pos + VCAP_NOTIFY_MULT, notify_mult);
		/* El BAR entero con devres (el driver no tiene que desmapearlo). */
		{
			void __iomem *base = pcim_iomap(pdev, bar, 0);

			return base ? base + off : NULL;
		}
	}
	return NULL;
}

static int kt_vrng_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct kt_vrng *v;
	u32 mult = 0, feat;
	u16 noff;
	int rc;

	v = devm_kzalloc(&pdev->dev, sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;
	v->pdev = pdev;
	init_completion(&v->done);
	rc = pcim_enable_device(pdev);
	if (rc)
		return rc;
	pci_set_master(pdev);
	v->common = kt_vrng_map_cap(pdev, VIRTIO_PCI_CAP_COMMON_CFG, NULL);
	v->notify = kt_vrng_map_cap(pdev, VIRTIO_PCI_CAP_NOTIFY_CFG, &mult);
	if (!v->common || !v->notify) {
		dev_err(&pdev->dev, "sin interfaz virtio moderna\n");
		return -ENODEV;
	}

	/* Reset, ACK, DRIVER y negociar solo VIRTIO_F_VERSION_1 (bit 32). */
	iowrite8(0, v->common + VCOM_STATUS);
	while (ioread8(v->common + VCOM_STATUS))
		cpu_relax();
	iowrite8(VSTAT_ACK | VSTAT_DRIVER, v->common + VCOM_STATUS);
	iowrite32(1, v->common + VCOM_DFSELECT);
	feat = ioread32(v->common + VCOM_DF);
	if (!(feat & 1)) {
		dev_err(&pdev->dev, "el dispositivo no ofrece VIRTIO_F_VERSION_1\n");
		return -ENODEV;
	}
	iowrite32(1, v->common + VCOM_GFSELECT);
	iowrite32(1, v->common + VCOM_GF);
	iowrite32(0, v->common + VCOM_GFSELECT);
	iowrite32(0, v->common + VCOM_GF);
	iowrite8(VSTAT_ACK | VSTAT_DRIVER | VSTAT_FEATURES_OK, v->common + VCOM_STATUS);
	if (!(ioread8(v->common + VCOM_STATUS) & VSTAT_FEATURES_OK))
		return -ENODEV;

	/* MSI-X: vector 0 para la configuracion, 1 para la cola; repartidos. */
	{
		struct irq_affinity affd = { .pre_vectors = 0 };

		v->nvec = pci_alloc_irq_vectors_affinity(pdev, 2, 2, PCI_IRQ_MSIX | PCI_IRQ_AFFINITY,
							 &affd);
	}
	if (v->nvec != 2) {
		dev_err(&pdev->dev, "MSI-X: %d\n", v->nvec);
		return v->nvec < 0 ? v->nvec : -ENOSPC;
	}
	v->cirq = pci_irq_vector(pdev, 0);
	v->qirq = pci_irq_vector(pdev, 1);
	rc = request_irq(v->cirq, kt_vrng_config_irq, 0, "kt_vrng-config", v);
	if (rc)
		goto err_vec;
	rc = request_irq(v->qirq, kt_vrng_irq, 0, "kt_vrng-req", v);
	if (rc)
		goto err_cirq;

	iowrite16(0, v->common + VCOM_MSIX);
	if (ioread16(v->common + VCOM_MSIX) == VIRTIO_MSI_NO_VECTOR) {
		rc = -EIO;
		goto err_qirq;
	}

	/* Cola 0 en una pagina coherente. */
	iowrite16(0, v->common + VCOM_Q_SELECT);
	v->qsize = ioread16(v->common + VCOM_Q_SIZE);
	if (!v->qsize) {
		rc = -ENODEV;
		goto err_qirq;
	}
	if (v->qsize > VRNG_QSIZE_MAX)
		v->qsize = VRNG_QSIZE_MAX;
	iowrite16(v->qsize, v->common + VCOM_Q_SIZE);
	v->ring = dma_alloc_coherent(&pdev->dev, PAGE_SIZE, &v->ring_dma, GFP_KERNEL);
	if (!v->ring) {
		rc = -ENOMEM;
		goto err_qirq;
	}
	v->desc = v->ring;
	v->avail = v->ring + 0x100;
	v->used = v->ring + 0x200;
	v->buf = v->ring + 0x800;
	iowrite16(1, v->common + VCOM_Q_MSIX);
	if (ioread16(v->common + VCOM_Q_MSIX) != 1) {
		rc = -EIO;
		goto err_ring;
	}
	writeq(v->ring_dma, v->common + VCOM_Q_DESC);
	writeq(v->ring_dma + 0x100, v->common + VCOM_Q_AVAIL);
	writeq(v->ring_dma + 0x200, v->common + VCOM_Q_USED);
	noff = ioread16(v->common + VCOM_Q_NOFF);
	v->notify += (u32)noff * mult;
	iowrite16(1, v->common + VCOM_Q_ENABLE);
	iowrite8(VSTAT_ACK | VSTAT_DRIVER | VSTAT_FEATURES_OK | VSTAT_DRIVER_OK,
		 v->common + VCOM_STATUS);
	pci_set_drvdata(pdev, v);
	kt_vrng_dev = v;
	return 0;

err_ring:
	dma_free_coherent(&pdev->dev, PAGE_SIZE, v->ring, v->ring_dma);
err_qirq:
	free_irq(v->qirq, v);
err_cirq:
	free_irq(v->cirq, v);
err_vec:
	pci_free_irq_vectors(pdev);
	iowrite8(0, v->common + VCOM_STATUS);
	return rc;
}

/* Pide VRNG_BUF bytes aleatorios; devuelve los recibidos o -errno. */
static int kt_vrng_request(struct kt_vrng *v)
{
	u16 slot = v->avail_idx % v->qsize;
	u32 len;

	memset(v->buf, 0, VRNG_BUF);
	reinit_completion(&v->done);
	v->desc[0].addr = cpu_to_le64(v->ring_dma + 0x800);
	v->desc[0].len = cpu_to_le32(VRNG_BUF);
	v->desc[0].flags = cpu_to_le16(VRING_DESC_F_WRITE);
	v->desc[0].next = 0;
	v->avail[2 + slot] = cpu_to_le16(0);
	wmb();
	v->avail_idx++;
	WRITE_ONCE(v->avail[1], cpu_to_le16(v->avail_idx));
	mb();
	iowrite16(0, v->notify);		/* la cola 0 */
	if (!wait_for_completion_timeout(&v->done, KT_TIMEOUT))
		return -ETIMEDOUT;
	rmb();
	/* used ring: {id, len} desde el byte 4 */
	len = le32_to_cpu(*(__le32 *)((u8 *)v->used + 4 + ((v->used_seen - 1) % v->qsize) * 8 + 4));
	return len;
}

static void kt_vrng_remove(struct pci_dev *pdev)
{
	struct kt_vrng *v = pci_get_drvdata(pdev);

	kt_vrng_removes++;
	/* Reset del dispositivo antes de quitarle la memoria y los vectores. */
	iowrite8(0, v->common + VCOM_STATUS);
	while (ioread8(v->common + VCOM_STATUS))
		cpu_relax();
	free_irq(v->qirq, v);
	free_irq(v->cirq, v);
	pci_free_irq_vectors(pdev);
	dma_free_coherent(&pdev->dev, PAGE_SIZE, v->ring, v->ring_dma);
	kt_vrng_dev = NULL;
}

static const struct pci_device_id kt_vrng_ids[] = {
	{ PCI_DEVICE(VRNG_VENDOR, VRNG_DEVICE_TRANS) },
	{ PCI_DEVICE(VRNG_VENDOR, VRNG_DEVICE_MODERN) },
	{ }
};
MODULE_DEVICE_TABLE(pci, kt_vrng_ids);

static struct pci_driver kt_vrng_driver = {
	.name = "kt_vrng",
	.id_table = kt_vrng_ids,
	.probe = kt_vrng_probe,
	.remove = kt_vrng_remove,
};

#endif
