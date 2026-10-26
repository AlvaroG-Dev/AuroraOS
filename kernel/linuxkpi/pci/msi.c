// SPDX-License-Identifier: GPL-2.0-only
/*
 * Vectores de interrupcion PCI (K5): pci_alloc_irq_vectors con MSI-X, MSI
 * (un vector) e INTx.
 *
 * La capa programa MSI y MSI-X ella misma (espacio de configuracion y su
 * propio ioremap de la tabla MSI-X), no con pci_enable_msi/msix de Aurora:
 * asi cada vector tiene su CPU de destino, mascara por entrada y cambio de
 * afinidad. Los vectores salen del asignador de Aurora (msi_vector_alloc,
 * 21 vectores de kernel compartidos por todas las CPU: la IDT es una sola);
 * lo que se reparte entre CPU es el destino (LAPIC) de cada vector.
 *
 * INTx: la linea que dejo el firmware (PCI_INTERRUPT_LINE, 1..15), en el
 * IOAPIC por nivel, con la polaridad del ISO de la MADT si lo hay (QEMU pc:
 * activa a nivel alto) o activa a nivel bajo, como en PCI.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/io.h>
#include "../aurora.h"
#include "../lkpi_internal.h"

#define LKPI_MSI_ADDR_BASE 0xfee00000U

enum { LKPI_IRQ_NONE, LKPI_IRQ_INTX, LKPI_IRQ_MSI, LKPI_IRQ_MSIX };

struct lkpi_pci_vec {
	struct pci_dev *pdev;
	int entry;			/* MSI-X: entrada de la tabla */
	unsigned int irq;
	struct lkpi_irq_desc *desc;
};

struct lkpi_pci_irqs {
	int type;
	int nvec;
	void __iomem *msix_table;
	int msix_table_size;
	struct cpumask *masks;		/* afinidad por vector (pci_irq_get_affinity) */
	struct lkpi_pci_vec vec[];
};

static atomic_t lkpi_next_cpu = ATOMIC_INIT(0);

static int lkpi_cpu_lapic(int cpu)
{
	int id = aurora_smp_cpu_lapic_id(cpu);

	return id < 0 ? aurora_smp_cpu_lapic_id(0) : id;
}

static u32 lkpi_msi_addr(int cpu)
{
	return LKPI_MSI_ADDR_BASE | ((u32)lkpi_cpu_lapic(cpu) << 12);
}

/* CPU para un vector no gestionado: por turnos entre las que estan en linea. */
static int lkpi_pick_cpu(void)
{
	unsigned int n = num_online_cpus();

	return n ? atomic_inc_return(&lkpi_next_cpu) % n : 0;
}

/* ------------------------------------------------------------ MSI-X */

static void __iomem *msix_entry(struct lkpi_pci_vec *v)
{
	return v->pdev->lkpi_irqs->msix_table + v->entry * PCI_MSIX_ENTRY_SIZE;
}

static void lkpi_msix_mask(struct lkpi_irq_desc *d)
{
	struct lkpi_pci_vec *v = d->chip_data;
	void __iomem *e = msix_entry(v);

	writel(readl(e + PCI_MSIX_ENTRY_VECTOR_CTRL) | PCI_MSIX_ENTRY_CTRL_MASKBIT,
	       e + PCI_MSIX_ENTRY_VECTOR_CTRL);
	readl(e + PCI_MSIX_ENTRY_VECTOR_CTRL);	/* vaciar escrituras posteadas */
}

static void lkpi_msix_unmask(struct lkpi_irq_desc *d)
{
	struct lkpi_pci_vec *v = d->chip_data;
	void __iomem *e = msix_entry(v);

	writel(readl(e + PCI_MSIX_ENTRY_VECTOR_CTRL) & ~PCI_MSIX_ENTRY_CTRL_MASKBIT,
	       e + PCI_MSIX_ENTRY_VECTOR_CTRL);
	readl(e + PCI_MSIX_ENTRY_VECTOR_CTRL);
}

static void lkpi_msix_write(struct lkpi_pci_vec *v, int cpu, bool masked)
{
	void __iomem *e = msix_entry(v);

	writel(readl(e + PCI_MSIX_ENTRY_VECTOR_CTRL) | PCI_MSIX_ENTRY_CTRL_MASKBIT,
	       e + PCI_MSIX_ENTRY_VECTOR_CTRL);
	writel(lkpi_msi_addr(cpu), e + PCI_MSIX_ENTRY_LOWER_ADDR);
	writel(0, e + PCI_MSIX_ENTRY_UPPER_ADDR);
	writel(v->irq, e + PCI_MSIX_ENTRY_DATA);
	if (!masked)
		writel(readl(e + PCI_MSIX_ENTRY_VECTOR_CTRL) & ~PCI_MSIX_ENTRY_CTRL_MASKBIT,
		       e + PCI_MSIX_ENTRY_VECTOR_CTRL);
	readl(e + PCI_MSIX_ENTRY_VECTOR_CTRL);
}

static int lkpi_msix_set_affinity(struct lkpi_irq_desc *d, int cpu)
{
	lkpi_msix_write(d->chip_data, cpu, d->masked);
	return 0;
}

static const struct lkpi_irq_chip lkpi_msix_chip = {
	.mask = lkpi_msix_mask,
	.unmask = lkpi_msix_unmask,
	.set_affinity = lkpi_msix_set_affinity,
};

/* -------------------------------------------------------------- MSI */

static bool msi_has_mask(struct pci_dev *p, u16 ctrl) { return ctrl & PCI_MSI_FLAGS_MASKBIT; }

static int msi_mask_reg(u16 ctrl)
{
	return (ctrl & PCI_MSI_FLAGS_64BIT) ? PCI_MSI_MASK_64 : PCI_MSI_MASK_32;
}

static void lkpi_msi_set_mask(struct lkpi_irq_desc *d, bool mask)
{
	struct lkpi_pci_vec *v = d->chip_data;
	struct pci_dev *p = v->pdev;
	u16 ctrl;

	pci_read_config_word(p, p->msi_cap + PCI_MSI_FLAGS, &ctrl);
	/* Sin bit de mascara por vector: la IRQ se recuerda pendiente (irq.c). */
	if (msi_has_mask(p, ctrl))
		pci_write_config_dword(p, p->msi_cap + msi_mask_reg(ctrl), mask ? 1 : 0);
}

static void lkpi_msi_mask(struct lkpi_irq_desc *d) { lkpi_msi_set_mask(d, true); }
static void lkpi_msi_unmask(struct lkpi_irq_desc *d) { lkpi_msi_set_mask(d, false); }

static void lkpi_msi_write(struct pci_dev *p, int cpu, unsigned int vector)
{
	u16 ctrl;

	pci_read_config_word(p, p->msi_cap + PCI_MSI_FLAGS, &ctrl);
	pci_write_config_dword(p, p->msi_cap + PCI_MSI_ADDRESS_LO, lkpi_msi_addr(cpu));
	if (ctrl & PCI_MSI_FLAGS_64BIT) {
		pci_write_config_dword(p, p->msi_cap + PCI_MSI_ADDRESS_HI, 0);
		pci_write_config_word(p, p->msi_cap + PCI_MSI_DATA_64, vector);
	} else {
		pci_write_config_word(p, p->msi_cap + PCI_MSI_DATA_32, vector);
	}
}

static int lkpi_msi_set_affinity(struct lkpi_irq_desc *d, int cpu)
{
	struct lkpi_pci_vec *v = d->chip_data;

	lkpi_msi_write(v->pdev, cpu, v->irq);
	return 0;
}

static const struct lkpi_irq_chip lkpi_msi_chip = {
	.mask = lkpi_msi_mask,
	.unmask = lkpi_msi_unmask,
	.set_affinity = lkpi_msi_set_affinity,
};

/* ------------------------------------------------------------- INTx */

static void lkpi_intx_route(struct lkpi_irq_desc *d, int cpu, bool masked)
{
	/* Vector 32 + linea: el que Aurora atiende para las IRQ 0..15. */
	aurora_ioapic_route_pci_intx(d->irq, 32 + d->irq, lkpi_cpu_lapic(cpu), masked);
}

static void lkpi_intx_startup(struct lkpi_irq_desc *d)
{
	lkpi_intx_route(d, d->cpu, true);
}

static void lkpi_intx_mask(struct lkpi_irq_desc *d) { aurora_ioapic_mask_irq(d->irq, 1); }
static void lkpi_intx_unmask(struct lkpi_irq_desc *d) { aurora_ioapic_mask_irq(d->irq, 0); }

static int lkpi_intx_set_affinity(struct lkpi_irq_desc *d, int cpu)
{
	lkpi_intx_route(d, cpu, d->masked);
	return 0;
}

static const struct lkpi_irq_chip lkpi_intx_chip = {
	.mask = lkpi_intx_mask,
	.unmask = lkpi_intx_unmask,
	.startup = lkpi_intx_startup,
	.shutdown = lkpi_intx_mask,
	.set_affinity = lkpi_intx_set_affinity,
};

/* ---------------------------------------------------------- reparto */

/*
 * CPU de cada vector. Con PCI_IRQ_AFFINITY los vectores "gestionados"
 * (sin pre/post) se reparten en orden entre las CPU en linea, uno por CPU
 * y vuelta a empezar; el resto, y sin AFFINITY todos, por turnos globales.
 */
static void lkpi_spread(struct lkpi_pci_irqs *irqs, int nvec, unsigned int flags,
			struct irq_affinity *affd, int *cpus)
{
	unsigned int online = num_online_cpus() ? num_online_cpus() : 1;
	unsigned int pre = 0, post = 0;

	if ((flags & PCI_IRQ_AFFINITY) && affd) {
		pre = min_t(unsigned int, affd->pre_vectors, nvec);
		post = min_t(unsigned int, affd->post_vectors, nvec - pre);
	}
	for (int i = 0; i < nvec; i++) {
		bool managed = (flags & PCI_IRQ_AFFINITY) && i >= pre && i < nvec - post;

		if (managed) {
			cpus[i] = (i - pre) % online;
			cpumask_copy(&irqs->masks[i], cpumask_of(cpus[i]));
		} else {
			cpus[i] = lkpi_pick_cpu();
			cpumask_copy(&irqs->masks[i], cpu_online_mask);
		}
	}
}

static void lkpi_free_vectors(struct lkpi_pci_irqs *irqs, int n)
{
	for (int i = 0; i < n; i++) {
		lkpi_irq_desc_free(irqs->vec[i].desc);
		irqs->vec[i].desc = NULL;
		if (irqs->type != LKPI_IRQ_INTX)
			aurora_msi_vector_free(irqs->vec[i].irq);
	}
}

static struct lkpi_pci_irqs *lkpi_irqs_alloc(int nvec)
{
	struct lkpi_pci_irqs *irqs = kzalloc(struct_size(irqs, vec, nvec), GFP_KERNEL);

	if (!irqs)
		return NULL;
	irqs->masks = kcalloc(nvec, sizeof(*irqs->masks), GFP_KERNEL);
	if (!irqs->masks) {
		kfree(irqs);
		return NULL;
	}
	return irqs;
}

static void lkpi_irqs_kfree(struct lkpi_pci_irqs *irqs)
{
	kfree(irqs->masks);
	kfree(irqs);
}

/* Pide hasta `max` vectores a Aurora; devuelve cuantos consiguio. */
static int lkpi_get_vectors(struct lkpi_pci_irqs *irqs, int max)
{
	int n;

	for (n = 0; n < max; n++) {
		int v = aurora_msi_vector_alloc();

		if (v < 0)
			break;
		irqs->vec[n].irq = v;
	}
	return n;
}

static int lkpi_setup_descs(struct pci_dev *dev, struct lkpi_pci_irqs *irqs, int nvec,
			    const int *cpus, const struct lkpi_irq_chip *chip, bool level)
{
	for (int i = 0; i < nvec; i++) {
		struct lkpi_irq_desc *d;

		irqs->vec[i].pdev = dev;
		d = lkpi_irq_desc_alloc(irqs->vec[i].irq, chip, &irqs->vec[i], cpus[i], level);
		if (IS_ERR(d)) {
			lkpi_free_vectors(irqs, i);
			for (int j = i; j < nvec; j++)
				if (irqs->type != LKPI_IRQ_INTX)
					aurora_msi_vector_free(irqs->vec[j].irq);
			return PTR_ERR(d);
		}
		cpumask_copy(&d->affinity, &irqs->masks[i]);
		irqs->vec[i].desc = d;
	}
	return 0;
}

static int lkpi_msix_table_size(struct pci_dev *dev)
{
	u16 ctrl;

	if (!dev->msix_cap)
		return 0;
	pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &ctrl);
	return (ctrl & PCI_MSIX_FLAGS_QSIZE) + 1;
}

/* Direccion fisica de la tabla MSI-X (BAR + desplazamiento). */
static resource_size_t lkpi_msix_table_phys(struct pci_dev *dev)
{
	u32 tbl;
	int bir;

	pci_read_config_dword(dev, dev->msix_cap + PCI_MSIX_TABLE, &tbl);
	bir = tbl & PCI_MSIX_TABLE_BIR;
	if (bir >= PCI_STD_NUM_BARS || !(pci_resource_flags(dev, bir) & IORESOURCE_MEM) ||
	    !pci_resource_start(dev, bir))
		return 0;
	return pci_resource_start(dev, bir) + (tbl & PCI_MSIX_TABLE_OFFSET);
}

static int lkpi_enable_msix(struct pci_dev *dev, int min, int max, unsigned int flags,
			    struct irq_affinity *affd)
{
	int tsize = lkpi_msix_table_size(dev), nvec, rc, *cpus;
	struct lkpi_pci_irqs *irqs;
	resource_size_t phys;
	u16 ctrl;

	if (!tsize || dev->no_msi)
		return -EINVAL;
	if (max > tsize)
		max = tsize;
	if (max < min)
		return -ENOSPC;
	phys = lkpi_msix_table_phys(dev);
	if (!phys)
		return -EINVAL;
	irqs = lkpi_irqs_alloc(max);
	cpus = kcalloc(max, sizeof(*cpus), GFP_KERNEL);
	if (!irqs || !cpus) {
		rc = -ENOMEM;
		goto err_free;
	}
	irqs->type = LKPI_IRQ_MSIX;
	irqs->msix_table_size = tsize;
	irqs->msix_table = ioremap(phys, tsize * PCI_MSIX_ENTRY_SIZE);
	if (!irqs->msix_table) {
		rc = -ENOMEM;
		goto err_free;
	}
	nvec = lkpi_get_vectors(irqs, max);
	if (nvec < min) {
		pci_info(dev, "MSI-X: %d vectores libres, hacen falta %d\n", nvec, min);
		for (int i = 0; i < nvec; i++)
			aurora_msi_vector_free(irqs->vec[i].irq);
		rc = -ENOSPC;
		goto err_unmap;
	}
	irqs->nvec = nvec;
	lkpi_spread(irqs, nvec, flags, affd, cpus);
	for (int i = 0; i < nvec; i++)
		irqs->vec[i].entry = i;
	rc = lkpi_setup_descs(dev, irqs, nvec, cpus, &lkpi_msix_chip, false);
	if (rc)
		goto err_unmap;
	dev->lkpi_irqs = irqs;

	/* Habilitar con la funcion enmascarada, programar, quitar la mascara.
	 * MSI e INTx quedan apagados; cada entrada sigue enmascarada hasta su
	 * request_irq(). */
	if (dev->msi_cap) {
		u16 mc;

		pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &mc);
		pci_write_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, mc & ~PCI_MSI_FLAGS_ENABLE);
	}
	pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &ctrl);
	ctrl |= PCI_MSIX_FLAGS_ENABLE | PCI_MSIX_FLAGS_MASKALL;
	pci_write_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, ctrl);
	pci_intx(dev, 0);
	for (int i = 0; i < tsize; i++) {
		void __iomem *e = irqs->msix_table + i * PCI_MSIX_ENTRY_SIZE;

		writel(readl(e + PCI_MSIX_ENTRY_VECTOR_CTRL) | PCI_MSIX_ENTRY_CTRL_MASKBIT,
		       e + PCI_MSIX_ENTRY_VECTOR_CTRL);
	}
	for (int i = 0; i < nvec; i++)
		lkpi_msix_write(&irqs->vec[i], cpus[i], true);
	ctrl &= ~PCI_MSIX_FLAGS_MASKALL;
	pci_write_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, ctrl);
	dev->msix_enabled = 1;
	kfree(cpus);
	pci_dbg(dev, "MSI-X: %d vectores (primero 0x%x)\n", nvec, irqs->vec[0].irq);
	return nvec;

err_unmap:
	iounmap(irqs->msix_table);
err_free:
	kfree(cpus);
	if (irqs)
		lkpi_irqs_kfree(irqs);
	return rc;
}

static int lkpi_enable_msi(struct pci_dev *dev, int min, unsigned int flags,
			   struct irq_affinity *affd)
{
	struct lkpi_pci_irqs *irqs;
	int cpu, rc;
	u16 ctrl;

	if (!dev->msi_cap || dev->no_msi || min > 1)
		return -EINVAL;		/* MSI de varios mensajes: no (vectores alineados) */
	irqs = lkpi_irqs_alloc(1);
	if (!irqs)
		return -ENOMEM;
	irqs->type = LKPI_IRQ_MSI;
	if (lkpi_get_vectors(irqs, 1) != 1) {
		lkpi_irqs_kfree(irqs);
		return -ENOSPC;
	}
	irqs->nvec = 1;
	lkpi_spread(irqs, 1, flags, affd, &cpu);
	rc = lkpi_setup_descs(dev, irqs, 1, &cpu, &lkpi_msi_chip, false);
	if (rc) {
		lkpi_irqs_kfree(irqs);
		return rc;
	}
	dev->lkpi_irqs = irqs;

	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &ctrl);
	if (msi_has_mask(dev, ctrl))
		pci_write_config_dword(dev, dev->msi_cap + msi_mask_reg(ctrl), 1);
	lkpi_msi_write(dev, cpu, irqs->vec[0].irq);
	ctrl &= ~PCI_MSI_FLAGS_QSIZE;		/* un solo mensaje */
	ctrl |= PCI_MSI_FLAGS_ENABLE;
	pci_write_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, ctrl);
	pci_intx(dev, 0);
	dev->msi_enabled = 1;
	dev->irq = irqs->vec[0].irq;
	return 1;
}

static int lkpi_enable_intx(struct pci_dev *dev, unsigned int flags)
{
	struct lkpi_pci_irqs *irqs;
	int cpu, rc;

	if (!dev->irq)
		return -EINVAL;
	irqs = lkpi_irqs_alloc(1);
	if (!irqs)
		return -ENOMEM;
	irqs->type = LKPI_IRQ_INTX;
	irqs->nvec = 1;
	irqs->vec[0].irq = dev->irq;
	lkpi_spread(irqs, 1, 0, NULL, &cpu);
	cpu = 0;			/* las INTx van a la CPU de arranque */
	cpumask_copy(&irqs->masks[0], cpumask_of(0));
	rc = lkpi_setup_descs(dev, irqs, 1, &cpu, &lkpi_intx_chip, true);
	if (rc == -EBUSY) {
		/* Otro dispositivo de la capa ya usa la linea: compartirla. */
		irqs->vec[0].pdev = dev;
		irqs->vec[0].desc = lkpi_irq_desc_get_shared(dev->irq);
		rc = irqs->vec[0].desc ? 0 : -EBUSY;
	}
	if (rc) {
		lkpi_irqs_kfree(irqs);
		return rc;
	}
	dev->lkpi_irqs = irqs;
	pci_intx(dev, 1);
	return 1;
}

int lkpi_pci_alloc_irq_vectors_affinity(struct pci_dev *dev, unsigned int min_vecs,
					unsigned int max_vecs, unsigned int flags,
					struct irq_affinity *affd)
{
	int rc = -ENOSPC;

	if (!min_vecs || max_vecs < min_vecs)
		return -EINVAL;
	if (dev->lkpi_irqs) {
		pci_err(dev, "pci_alloc_irq_vectors: ya tiene vectores\n");
		return -EINVAL;
	}
	if (flags & PCI_IRQ_MSIX) {
		rc = lkpi_enable_msix(dev, min_vecs, max_vecs, flags, affd);
		if (rc > 0)
			return rc;
	}
	if (flags & PCI_IRQ_MSI) {
		rc = lkpi_enable_msi(dev, min_vecs, flags, affd);
		if (rc > 0)
			return rc;
	}
	if ((flags & PCI_IRQ_INTX) && min_vecs == 1) {
		rc = lkpi_enable_intx(dev, flags);
		if (rc > 0)
			return rc;
	}
	return rc;
}
EXPORT_SYMBOL(lkpi_pci_alloc_irq_vectors_affinity);

void lkpi_pci_free_irq_vectors(struct pci_dev *dev)
{
	struct lkpi_pci_irqs *irqs = dev->lkpi_irqs;
	u16 ctrl;

	if (!irqs)
		return;
	if (irqs->type == LKPI_IRQ_MSIX) {
		pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &ctrl);
		pci_write_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS,
				      ctrl | PCI_MSIX_FLAGS_MASKALL);
	}
	lkpi_free_vectors(irqs, irqs->nvec);
	if (irqs->type == LKPI_IRQ_MSIX) {
		pci_write_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS,
				      ctrl & ~(PCI_MSIX_FLAGS_ENABLE | PCI_MSIX_FLAGS_MASKALL));
		iounmap(irqs->msix_table);
		dev->msix_enabled = 0;
		pci_intx(dev, 1);
	} else if (irqs->type == LKPI_IRQ_MSI) {
		pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &ctrl);
		pci_write_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS,
				      ctrl & ~PCI_MSI_FLAGS_ENABLE);
		dev->msi_enabled = 0;
		pci_intx(dev, 1);
		/* pdev->irq vuelve a ser la INTx. */
		{
			u8 line;

			pci_read_config_byte(dev, PCI_INTERRUPT_LINE, &line);
			dev->irq = (dev->pin && line > 0 && line < 16) ? line : 0;
		}
	}
	dev->lkpi_irqs = NULL;
	lkpi_irqs_kfree(irqs);
}
EXPORT_SYMBOL(lkpi_pci_free_irq_vectors);

int lkpi_pci_irq_vector(struct pci_dev *dev, unsigned int nr)
{
	struct lkpi_pci_irqs *irqs = dev->lkpi_irqs;

	if (!irqs)
		return nr == 0 && dev->irq ? (int)dev->irq : -EINVAL;
	if (nr >= irqs->nvec)
		return -EINVAL;
	return irqs->vec[nr].irq;
}
EXPORT_SYMBOL(lkpi_pci_irq_vector);

const struct cpumask *lkpi_pci_irq_get_affinity(struct pci_dev *dev, int nr)
{
	struct lkpi_pci_irqs *irqs = dev->lkpi_irqs;

	if (!irqs || nr < 0 || nr >= irqs->nvec)
		return NULL;
	return &irqs->masks[nr];
}
EXPORT_SYMBOL(lkpi_pci_irq_get_affinity);

int lkpi_pci_msi_vec_count(struct pci_dev *dev)
{
	u16 ctrl;

	if (!dev->msi_cap)
		return -EINVAL;
	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &ctrl);
	return 1 << ((ctrl & PCI_MSI_FLAGS_QMASK) >> 1);
}
EXPORT_SYMBOL(lkpi_pci_msi_vec_count);

int lkpi_pci_msix_vec_count(struct pci_dev *dev)
{
	int n = lkpi_msix_table_size(dev);

	return n ? n : -EINVAL;
}
EXPORT_SYMBOL(lkpi_pci_msix_vec_count);

/* Tras un reset (pci_restore_state): volver a programar MSI/MSI-X. */
void lkpi_pci_msi_restore(struct pci_dev *dev)
{
	struct lkpi_pci_irqs *irqs = dev->lkpi_irqs;
	u16 ctrl;

	if (!irqs)
		return;
	if (irqs->type == LKPI_IRQ_MSIX) {
		pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &ctrl);
		pci_write_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS,
				      ctrl | PCI_MSIX_FLAGS_ENABLE | PCI_MSIX_FLAGS_MASKALL);
		for (int i = 0; i < irqs->nvec; i++)
			lkpi_msix_write(&irqs->vec[i], irqs->vec[i].desc->cpu,
					irqs->vec[i].desc->masked);
		pci_write_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS,
				      (ctrl | PCI_MSIX_FLAGS_ENABLE) & ~PCI_MSIX_FLAGS_MASKALL);
	} else if (irqs->type == LKPI_IRQ_MSI) {
		lkpi_msi_write(dev, irqs->vec[0].desc->cpu, irqs->vec[0].irq);
		pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &ctrl);
		pci_write_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, ctrl | PCI_MSI_FLAGS_ENABLE);
		if (msi_has_mask(dev, ctrl))
			pci_write_config_dword(dev, dev->msi_cap + msi_mask_reg(ctrl),
					       irqs->vec[0].desc->masked);
	}
}
