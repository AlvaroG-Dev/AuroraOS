// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pruebas de L9.2 / L9.3: runtime PM y suspend/resume de dispositivos.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/delay.h>
#include <linux/lkpi_tdr.h>
#include "lkpi_test.h"

extern int kt_pm_cycle(void) __asm__("lkpi_pm_cycle_devices");
extern int kt_pm_suspend(void) __asm__("lkpi_pm_suspend_devices");
extern void kt_pm_resume(void) __asm__("lkpi_pm_resume_devices");
extern bool kt_pm_policy(void) __asm__("lkpi_pm_runtime_policy");

/* ---------------------------------------------------- dispositivo virtual */

static char kt_seq[256];
static int kt_rs, kt_rr, kt_ri;		/* runtime suspend / resume / idle */
static int kt_busy;			/* runtime_suspend devuelve -EBUSY mientras > 0 */
static int kt_fail_suspend;		/* suspend devuelve este error */

static void seq(const char *s)
{
	strlcat(kt_seq, s, sizeof(kt_seq));
}

static int kt_rt_suspend(struct device *d) { kt_rs++; return kt_busy ? -EBUSY : 0; }
static int kt_rt_resume(struct device *d) { kt_rr++; return 0; }
static int kt_rt_idle(struct device *d) { kt_ri++; return 0; }
static int kt_prepare(struct device *d) { seq("P "); return 0; }
static void kt_complete(struct device *d) { seq("C "); }
static int kt_suspend(struct device *d) { seq("S "); return kt_fail_suspend; }
static int kt_suspend_late(struct device *d) { seq("L "); return 0; }
static int kt_suspend_noirq(struct device *d) { seq("N "); return 0; }
static int kt_resume_noirq(struct device *d) { seq("n "); return 0; }
static int kt_resume_early(struct device *d) { seq("l "); return 0; }
static int kt_resume(struct device *d) { seq("s "); return 0; }

static const struct dev_pm_ops kt_ops = {
	.prepare = kt_prepare, .complete = kt_complete,
	.suspend = kt_suspend, .resume = kt_resume,
	.suspend_late = kt_suspend_late, .resume_early = kt_resume_early,
	.suspend_noirq = kt_suspend_noirq, .resume_noirq = kt_resume_noirq,
	.runtime_suspend = kt_rt_suspend, .runtime_resume = kt_rt_resume,
	.runtime_idle = kt_rt_idle,
};
static const struct device_type kt_type = { .name = "kt_pm", .pm = &kt_ops };
static struct device kt_dev;

static bool status_is(struct device *d, int st) { return d->power.runtime_status == st; }

static void test_runtime_off(void)
{
	/* Modo "siempre activo": cuenta usos y nunca suspende. */
	pm_runtime_enable(&kt_dev);
	KT_EQ(pm_runtime_get_sync(&kt_dev), 1);
	KT_EQ(atomic_read(&kt_dev.power.usage_count), 1);
	KT_EQ(pm_runtime_put_sync(&kt_dev), 0);
	KT_EQ(atomic_read(&kt_dev.power.usage_count), 0);
	KT_EQ(kt_rs, 0);
	KT_ASSERT(pm_runtime_active(&kt_dev) && !pm_runtime_suspended(&kt_dev), "siempre activo");
	pm_runtime_allow(&kt_dev);
	pm_runtime_put_autosuspend(&kt_dev);
	atomic_set(&kt_dev.power.usage_count, 0);
	msleep(50);
	KT_EQ(kt_rs, 0);
	KT_EQ(pm_runtime_get_if_in_use(&kt_dev), 0);	/* sin usos */
	pm_runtime_get_noresume(&kt_dev);
	KT_EQ(pm_runtime_get_if_in_use(&kt_dev), 1);
	pm_runtime_put_noidle(&kt_dev);
	pm_runtime_put_noidle(&kt_dev);
	pm_runtime_disable(&kt_dev);
}

static void test_runtime_on(void)
{
	KT_ASSERT(!pm_runtime_enabled(&kt_dev), "nace deshabilitado");
	KT_ASSERT(kt_dev.power.runtime_auto, "nace en auto (como Linux, forbid lo pone el bus)");
	pm_runtime_enable(&kt_dev);
	KT_ASSERT(pm_runtime_enabled(&kt_dev), "enable");
	KT_ASSERT(status_is(&kt_dev, RPM_ACTIVE), "activo al empezar");

	/* get/put sincronos: idle -> suspender -> despertar. */
	KT_EQ(pm_runtime_get_sync(&kt_dev), 1);		/* ya activo */
	KT_EQ(kt_rr, 0);
	KT_EQ(pm_runtime_suspend(&kt_dev), -EAGAIN);	/* en uso */
	KT_EQ(pm_runtime_put_sync(&kt_dev), 0);
	KT_EQ(kt_ri, 1);
	KT_EQ(kt_rs, 1);
	KT_ASSERT(status_is(&kt_dev, RPM_SUSPENDED), "suspendido tras put_sync");
	KT_ASSERT(pm_runtime_suspended(&kt_dev), "pm_runtime_suspended");
	KT_EQ(pm_runtime_get_if_in_use(&kt_dev), 0);	/* suspendido: no despierta */
	KT_EQ(pm_runtime_get_sync(&kt_dev), 0);
	KT_EQ(kt_rr, 1);
	KT_ASSERT(status_is(&kt_dev, RPM_ACTIVE), "activo tras get_sync");
	KT_EQ(pm_runtime_get_if_in_use(&kt_dev), 1);	/* en uso y activo */
	pm_runtime_put_noidle(&kt_dev);

	/* El driver puede negarse (-EBUSY): sigue activo y no es un error. */
	kt_busy = 1;
	KT_EQ(pm_runtime_put_sync(&kt_dev), -EBUSY);
	KT_ASSERT(status_is(&kt_dev, RPM_ACTIVE), "-EBUSY mantiene activo");
	KT_EQ(kt_dev.power.runtime_error, 0);
	kt_busy = 0;
	KT_EQ(pm_runtime_suspend(&kt_dev), 0);
	KT_ASSERT(status_is(&kt_dev, RPM_SUSPENDED), "pm_runtime_suspend directo");
	KT_EQ(pm_runtime_resume_and_get(&kt_dev), 0);
	KT_ASSERT(status_is(&kt_dev, RPM_ACTIVE), "resume_and_get");
	KT_EQ(atomic_read(&kt_dev.power.usage_count), 1);
	pm_runtime_put_noidle(&kt_dev);

	/* Autosuspend: espera al retardo desde la ultima actividad. */
	pm_runtime_use_autosuspend(&kt_dev);
	pm_runtime_set_autosuspend_delay(&kt_dev, 100);
	pm_runtime_get_sync(&kt_dev);
	pm_runtime_mark_last_busy(&kt_dev);
	pm_runtime_put_autosuspend(&kt_dev);
	msleep(30);
	KT_ASSERT(status_is(&kt_dev, RPM_ACTIVE), "autosuspend: aun no (30 ms de 100)");
	msleep(400);
	KT_ASSERT(status_is(&kt_dev, RPM_SUSPENDED), "autosuspend: suspendido pasado el retardo");
	/* Una actividad intermedia retrasa el sueno. */
	pm_runtime_get_sync(&kt_dev);
	pm_runtime_mark_last_busy(&kt_dev);
	pm_runtime_put_autosuspend(&kt_dev);
	msleep(60);
	pm_runtime_get_sync(&kt_dev);
	pm_runtime_mark_last_busy(&kt_dev);
	pm_runtime_put_autosuspend(&kt_dev);
	msleep(60);
	KT_ASSERT(status_is(&kt_dev, RPM_ACTIVE), "autosuspend: la actividad lo retrasa");
	msleep(400);
	KT_ASSERT(status_is(&kt_dev, RPM_SUSPENDED), "autosuspend: al final duerme");
	pm_runtime_dont_use_autosuspend(&kt_dev);

	/* forbid / allow (power/control). */
	pm_runtime_get_sync(&kt_dev);
	pm_runtime_put_noidle(&kt_dev);
	pm_runtime_forbid(&kt_dev);
	KT_ASSERT(!kt_dev.power.runtime_auto, "forbid -> control=on");
	KT_EQ(atomic_read(&kt_dev.power.usage_count), 1);
	KT_EQ(pm_runtime_suspend(&kt_dev), -EAGAIN);
	pm_runtime_allow(&kt_dev);
	msleep(100);
	KT_ASSERT(status_is(&kt_dev, RPM_SUSPENDED), "allow deja dormir");

	/* Deshabilitado: nada cambia de estado. */
	pm_runtime_disable(&kt_dev);
	KT_EQ(pm_runtime_suspend(&kt_dev), 1);	/* ya suspendido */
	pm_runtime_get_sync(&kt_dev);		/* disable_depth > 0: no hay callback */
	KT_ASSERT(status_is(&kt_dev, RPM_SUSPENDED), "deshabilitado no despierta");
	pm_runtime_put_noidle(&kt_dev);
	pm_runtime_set_active(&kt_dev);
	KT_ASSERT(status_is(&kt_dev, RPM_ACTIVE), "set_active");
}

static void test_system_sleep(void)
{
	/* Orden de fases de un ciclo completo con IOMMU/PCI de por medio. */
	kt_seq[0] = '\0';
	KT_EQ(kt_pm_cycle(), 0);
	/* Aparece una sola vez por fase en nuestro dispositivo: P S L N, n l s C. */
	KT_STREQ(kt_seq, "P S L N n l s C ");

	/* Un fallo en suspend deshace las fases ya hechas y se propaga. */
	kt_seq[0] = '\0';
	kt_fail_suspend = -EBUSY;
	KT_EQ(kt_pm_suspend(), -EBUSY);
	kt_fail_suspend = 0;
	KT_STREQ(kt_seq, "P S C ");		/* sin resume: su suspend fallo */
	/* Y el sistema queda utilizable: otro ciclo funciona. */
	kt_seq[0] = '\0';
	KT_EQ(kt_pm_cycle(), 0);
	KT_STREQ(kt_seq, "P S L N n l s C ");

	/* Dos suspend seguidos: el segundo se rechaza. */
	KT_EQ(kt_pm_suspend(), 0);
	KT_EQ(kt_pm_suspend(), -EBUSY);
	kt_pm_resume();
	kt_pm_resume();				/* resume de mas: no hace nada */
}


/* ------------------------------------------------------------- TDR */

static bool kt_tdr_busy_v;
static u64 kt_tdr_prog;
static int kt_tdr_resets, kt_tdr_recover;

static bool kt_tdr_busy(void *p) { return kt_tdr_busy_v; }
static u64 kt_tdr_progress(void *p) { return kt_tdr_prog; }
static int kt_tdr_reset(void *p)
{
	kt_tdr_resets++;
	if (kt_tdr_recover)
		kt_tdr_busy_v = false;	/* el reinicio libera el trabajo atascado */
	return 0;
}

static void test_tdr(void)
{
	struct lkpi_tdr t = {
		.name = "kt", .dev = &kt_dev, .timeout_ms = 80, .max_resets = 2, .window_ms = 5000,
		.busy = kt_tdr_busy, .progress = kt_tdr_progress, .reset = kt_tdr_reset,
	};

	KT_EQ(lkpi_tdr_register(&t), 0);
	/* Sin trabajo pendiente nunca hay cuelgue. */
	msleep(250);
	KT_EQ(t.hangs, 0);
	/* Trabajo que avanza: tampoco. */
	kt_tdr_busy_v = true;
	for (int i = 0; i < 10; i++) {
		kt_tdr_prog++;
		msleep(40);
	}
	KT_EQ(t.hangs, 0);
	/* Trabajo atascado: se detecta y el reinicio lo recupera. */
	kt_tdr_recover = 1;
	msleep(400);
	KT_EQ(t.hangs, 1);
	KT_EQ(kt_tdr_resets, 1);
	KT_EQ(t.resets, 1);
	KT_ASSERT(!t.wedged, "recuperado");
	/* Si el reinicio no recupera, tras max_resets se da por perdido. */
	kt_tdr_recover = 0;
	kt_tdr_busy_v = true;
	msleep(1200);
	KT_ASSERT(t.wedged, "dispositivo perdido tras %u reinicios", t.max_resets);
	KT_EQ(kt_tdr_resets, 2);		/* 1 recuperado + 1 sin efecto; el tercero no se intenta */
	msleep(400);
	KT_EQ(kt_tdr_resets, 2);		/* ya no reinicia mas */
	lkpi_tdr_unregister(&t);
	kt_tdr_busy_v = false;
}

/* ----------------------------------------------------- PCI: D3hot real */

static int kt_pci_rs, kt_pci_rr, kt_pci_s, kt_pci_r;
static int kt_pci_rt_suspend(struct device *d) { kt_pci_rs++; return 0; }
static int kt_pci_rt_resume(struct device *d) { kt_pci_rr++; return 0; }
static int kt_pci_suspend(struct device *d) { kt_pci_s++; return 0; }
static int kt_pci_resume(struct device *d) { kt_pci_r++; return 0; }
static const struct dev_pm_ops kt_pci_ops = {
	.suspend = kt_pci_suspend, .resume = kt_pci_resume,
	.runtime_suspend = kt_pci_rt_suspend, .runtime_resume = kt_pci_rt_resume,
};
static int kt_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	return pcim_enable_device(pdev);
}
static void kt_pci_remove(struct pci_dev *pdev) {}
static const struct pci_device_id kt_pci_ids[] = {
	{ PCI_DEVICE(0x1af4, 0x1005) },	/* virtio-rng (transitional) */
	{ PCI_DEVICE(0x1af4, 0x1044) },	/* virtio-rng (moderno) */
	{ }
};
static struct pci_driver kt_pci_driver = {
	.name = "kt_pm_pci",
	.id_table = kt_pci_ids,
	.probe = kt_pci_probe,
	.remove = kt_pci_remove,
	.driver.pm = &kt_pci_ops,
};

static u16 pm_state(struct pci_dev *pdev)
{
	u16 v;

	pci_read_config_word(pdev, pdev->pm_cap + PCI_PM_CTRL, &v);
	return v & PCI_PM_CTRL_STATE_MASK;
}

static void test_pci_d3(void)
{
	struct pci_dev *pdev = pci_get_device(0x1af4, 0x1044, NULL);
	u32 bar0, bar0b;
	u16 cmd, cmdb;

	if (!pdev)
		pdev = pci_get_device(0x1af4, 0x1005, NULL);
	if (!pdev) {
		KT_SKIP("QEMU sin virtio-rng-pci");
		return;
	}
	if (!pdev->pm_cap) {
		KT_SKIP("el dispositivo no tiene capability PM");
		goto put;
	}
	KT_EQ(pci_register_driver(&kt_pci_driver), 0);
	KT_ASSERT(pdev->driver == &kt_pci_driver, "enlazado");
	KT_EQ(pm_state(pdev), PCI_D0);
	pci_read_config_dword(pdev, PCI_BASE_ADDRESS_0, &bar0);
	pci_read_config_word(pdev, PCI_COMMAND, &cmd);

	/* Sueno del sistema: D3hot y vuelta con la config restaurada. */
	KT_EQ(kt_pm_suspend(), 0);
	KT_ASSERT(kt_pci_s >= 1, "suspend del driver PCI");
	KT_EQ(pm_state(pdev), PCI_D3hot);
	KT_EQ(pdev->current_state, PCI_D3hot);
	kt_pm_resume();
	KT_ASSERT(kt_pci_r >= 1, "resume del driver PCI");
	KT_EQ(pm_state(pdev), PCI_D0);
	pci_read_config_dword(pdev, PCI_BASE_ADDRESS_0, &bar0b);
	pci_read_config_word(pdev, PCI_COMMAND, &cmdb);
	KT_EQ(bar0b, bar0);
	KT_EQ(cmdb, cmd);

	{
		/* Reset de la funcion sin FLR: por PM (o -ENOTTY si el dispositivo
		 * declara NoSoftRst). Pase lo que pase, la config vuelve igual. */
		int rrc = pci_reset_function(pdev);

		KT_ASSERT(rrc == 0 || rrc == -ENOTTY, "pci_reset_function: %d", rrc);
		KT_EQ(pm_state(pdev), PCI_D0);
		pci_read_config_dword(pdev, PCI_BASE_ADDRESS_0, &bar0b);
		KT_EQ(bar0b, bar0);
	}
	if (kt_pm_policy()) {
		/* Runtime PM: allow + put -> D3hot; get_sync -> D0. */
		pm_runtime_enable(&pdev->dev);
		pm_runtime_allow(&pdev->dev);
		msleep(100);
		KT_ASSERT(kt_pci_rs >= 1, "runtime_suspend del driver PCI");
		KT_EQ(pm_state(pdev), PCI_D3hot);
		KT_ASSERT(pm_runtime_suspended(&pdev->dev), "PCI suspendido");
		KT_EQ(pm_runtime_get_sync(&pdev->dev), 0);
		KT_ASSERT(kt_pci_rr >= 1, "runtime_resume del driver PCI");
		KT_EQ(pm_state(pdev), PCI_D0);
		pci_read_config_dword(pdev, PCI_BASE_ADDRESS_0, &bar0b);
		KT_EQ(bar0b, bar0);
		pm_runtime_put_noidle(&pdev->dev);
		pm_runtime_forbid(&pdev->dev);
		pm_runtime_disable(&pdev->dev);
	}
	pci_unregister_driver(&kt_pci_driver);
put:
	pci_dev_put(pdev);
}

static int __init kt_pm_init(void)
{
	int rc;

	device_initialize(&kt_dev);
	dev_set_name(&kt_dev, "kt_pm0");
	kt_dev.type = &kt_type;
	rc = device_add(&kt_dev);
	KT_EQ(rc, 0);
	if (rc)
		return 0;
	if (kt_pm_policy())
		test_runtime_on();
	else
		test_runtime_off();
	pm_runtime_disable(&kt_dev);	/* idempotente en el modo apagado */
	test_system_sleep();
	test_pci_d3();
	test_tdr();
	device_del(&kt_dev);
	put_device(&kt_dev);
	return 0;
}
static void __exit kt_pm_exit(void) {}
module_init(kt_pm_init);
module_exit(kt_pm_exit);
MODULE_LICENSE("GPL");
