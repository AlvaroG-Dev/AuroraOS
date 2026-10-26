// SPDX-License-Identifier: GPL-2.0-only
/*
 * K5 con varias CPU (hito de L3): vectores MSI-X repartidos entre CPU y
 * cambio de afinidad (el handler corre donde se pidio), y estres de una MSI
 * disparada desde varios hilos mientras otro la deshabilita, la mueve de
 * CPU y la sincroniza (ningun handler dentro de una ventana deshabilitada).
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kthread.h>
#include "lkpi_k5_toys.h"

static void test_vrng_affinity(void)
{
	struct pci_dev *pdev = pci_get_device(VRNG_VENDOR, VRNG_DEVICE_TRANS, NULL);
	unsigned int online = num_online_cpus(), ok = 0, rounds = 3 * online;
	struct kt_vrng *v;

	if (!pdev)
		pdev = pci_get_device(VRNG_VENDOR, VRNG_DEVICE_MODERN, NULL);
	if (!pdev) {
		KT_SKIP("QEMU sin -device virtio-rng-pci");
		return;
	}
	KT_EQ(pci_register_driver(&kt_vrng_driver), 0);
	v = kt_vrng_dev;
	KT_ASSERT(v != NULL, "probe de kt_vrng");
	if (!v)
		goto out;
	/* PCI_IRQ_AFFINITY: un vector por CPU, en orden. */
	KT_EQ(lkpi_irq_get_effective_cpu(v->cirq), 0);
	KT_EQ(lkpi_irq_get_effective_cpu(v->qirq), 1);
	KT_ASSERT(cpumask_equal(pci_irq_get_affinity(v->pdev, 1), cpumask_of(1)),
		  "mascara del vector 1");

	for (unsigned int r = 0; r < rounds; r++) {
		unsigned int target = r % online;

		KT_EQ(irq_set_affinity(v->qirq, cpumask_of(target)), 0);
		if (kt_vrng_request(v) == VRNG_BUF && v->handler_cpu == target)
			ok++;
		else
			pr_err("kt_vrng: ronda %u: handler en CPU %d, se pidio %u\n", r,
			       v->handler_cpu, target);
	}
	KT_EQ(ok, rounds);
	pci_unregister_driver(&kt_vrng_driver);
out:
	pci_dev_put(pdev);
}

/* ---------------------------------------------------- estres de MSI */

struct kt_raiser {
	struct kt_edu *e;
	u32 bit;
	unsigned long raises;
};

static atomic_t kt_in_window;		/* el hilo principal tiene la IRQ deshabilitada */
static atomic_t kt_violations;
static atomic_t kt_cpu_seen[NR_CPUS];

static irqreturn_t kt_stress_irq(int irq, void *data)
{
	struct kt_edu *e = data;
	u32 st = readl(e->mmio + EDU_IRQ_STATUS);

	if (atomic_read(&kt_in_window))
		atomic_inc(&kt_violations);
	if (!st)
		return IRQ_NONE;
	writel(st, e->mmio + EDU_IRQ_ACK);
	atomic_inc(&kt_cpu_seen[smp_processor_id()]);
	atomic_inc(&e->handled);
	complete(&e->done);
	return IRQ_HANDLED;
}

static int kt_raiser_fn(void *data)
{
	struct kt_raiser *r = data;

	while (!kthread_should_stop()) {
		kt_edu_raise(r->e, r->bit);
		r->raises++;
		if (r->raises % 16 == 0)
			usleep_range(100, 300);
		else
			cpu_relax();
	}
	return 0;
}

static void test_edu_msi_stress(void)
{
	struct pci_dev *pdev = pci_get_device(EDU_VENDOR, EDU_DEVICE, NULL);
	unsigned int online = num_online_cpus(), ncpu_seen = 0;
	struct kt_raiser *rs;
	struct task_struct **th;
	unsigned long total = 0;
	struct kt_edu *e;
	int irq, nthreads = 2 * online, i;

	if (!pdev) {
		KT_SKIP("QEMU sin -device edu");
		return;
	}
	KT_EQ(pci_register_driver(&kt_edu_driver), 0);
	e = kt_edu_dev;
	KT_ASSERT(e != NULL, "probe de kt_edu");
	if (!e)
		goto out;
	KT_EQ(pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI), 1);
	irq = pci_irq_vector(pdev, 0);
	atomic_set(&kt_violations, 0);
	atomic_set(&kt_in_window, 0);
	for (i = 0; i < NR_CPUS; i++)
		atomic_set(&kt_cpu_seen[i], 0);
	atomic_set(&e->handled, 0);
	KT_EQ(request_irq(irq, kt_stress_irq, 0, "kt_edu-stress", e), 0);

	rs = kcalloc(nthreads, sizeof(*rs), GFP_KERNEL);
	th = kcalloc(nthreads, sizeof(*th), GFP_KERNEL);
	if (!rs || !th) {
		KT_ASSERT(0, "sin memoria");
		goto out_irq;
	}
	for (i = 0; i < nthreads; i++) {
		rs[i].e = e;
		rs[i].bit = 1u << (i % 16);
		th[i] = kthread_run(kt_raiser_fn, &rs[i], "kt_raise/%d", i);
		if (IS_ERR(th[i]))
			th[i] = NULL;
	}

	/* 300 vueltas: deshabilitar (y sincronizar), ventana sin handlers,
	 * habilitar, mover la MSI a otra CPU. */
	for (i = 0; i < 300; i++) {
		disable_irq(irq);
		atomic_set(&kt_in_window, 1);
		udelay(200);
		atomic_set(&kt_in_window, 0);
		enable_irq(irq);
		KT_ASSERT(irq_set_affinity(irq, cpumask_of(i % online)) == 0, "irq_set_affinity");
		synchronize_irq(irq);
		if (i % 32 == 0)
			msleep(1);
	}
	for (i = 0; i < nthreads; i++) {
		if (th[i])
			kthread_stop(th[i]);
		total += rs[i].raises;
	}
	/* Lo ultimo pendiente se atiende: una mas y esperarla. */
	reinit_completion(&e->done);
	kt_edu_raise(e, 0x8000);
	KT_ASSERT(kt_edu_wait(e), "la MSI sigue viva tras el estres");
	for (i = 0; i < NR_CPUS; i++)
		ncpu_seen += atomic_read(&kt_cpu_seen[i]) > 0;
	pr_info("kt_edu estres: %lu disparos, %d atendidas, en %u CPU, %d violaciones\n", total,
		atomic_read(&e->handled), ncpu_seen, atomic_read(&kt_violations));
	KT_EQ(atomic_read(&kt_violations), 0);
	KT_ASSERT(atomic_read(&e->handled) > 100, "atendidas: %d", atomic_read(&e->handled));
	KT_ASSERT(ncpu_seen == online, "la MSI llego a %u de %u CPU", ncpu_seen, online);
out_irq:
	kfree(rs);
	kfree(th);
	free_irq(irq, e);
	pci_free_irq_vectors(pdev);
	pci_unregister_driver(&kt_edu_driver);
out:
	pci_dev_put(pdev);
}

static int __init test_k5_smp_init(void)
{
	if (num_online_cpus() < 2) {
		KT_SKIP("hace falta mas de una CPU");
		return 0;
	}
	test_vrng_affinity();
	test_edu_msi_stress();
	return 0;
}

static void __exit test_k5_smp_exit(void)
{
}

module_init(test_k5_smp_init);
module_exit(test_k5_smp_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Pruebas LinuxKPI K5 con varias CPU");
