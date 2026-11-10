// SPDX-License-Identifier: GPL-2.0-only
/* Watchdog de GPU (L9.4). Ver linux/lkpi_tdr.h. */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/pci.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/lkpi_tdr.h>

int lkpi_tdr_default_reset(struct device *dev)
{
	if (!dev || !dev_is_pci(dev))
		return -ENOTTY;
	return pci_reset_function(to_pci_dev(dev));
}
EXPORT_SYMBOL(lkpi_tdr_default_reset);

static unsigned long tdr_poll(struct lkpi_tdr *t)
{
	unsigned long p = msecs_to_jiffies(t->timeout_ms / 4);

	return p ? p : 1;
}

static void tdr_uevent(struct lkpi_tdr *t, const char *what)
{
	char name[64], ev[32];
	char *env[] = { name, ev, NULL };

	snprintf(name, sizeof(name), "NAME=%s", t->name ? t->name : "tdr");
	snprintf(ev, sizeof(ev), "%s=1", what);
	kobject_uevent_env(&t->dev->kobj, KOBJ_CHANGE, env);
}

static void tdr_work(struct work_struct *w)
{
	struct lkpi_tdr *t = container_of(to_delayed_work(w), struct lkpi_tdr, work);
	unsigned long now = jiffies;
	u64 prog;
	int rc;

	if (!t->active || t->wedged)
		return;
	prog = t->progress(t->priv);
	if (!t->busy(t->priv) || prog != t->last_progress) {
		t->last_progress = prog;
		t->stamp = now;
		goto rearm;
	}
	if (time_before(now, t->stamp + msecs_to_jiffies(t->timeout_ms)))
		goto rearm;

	/* Cuelgue: trabajo pendiente sin avance durante timeout_ms. */
	t->hangs++;
	dev_err(t->dev, "TDR[%s]: sin progreso %u ms con trabajo pendiente (cuelgue %u)\n",
		t->name ? t->name : "?", t->timeout_ms, t->hangs);
	if (time_after(now, t->window_start + msecs_to_jiffies(t->window_ms))) {
		t->window_start = now;
		t->window_resets = 0;
	}
	if (t->window_resets >= t->max_resets) {
		t->wedged = true;
		dev_err(t->dev, "TDR[%s]: %u reinicios en %u ms; dispositivo dado por perdido\n",
			t->name ? t->name : "?", t->window_resets, t->window_ms);
		/* Un dispositivo perdido no debe seguir haciendo DMA. */
		if (dev_is_pci(t->dev))
			pci_clear_master(to_pci_dev(t->dev));
		tdr_uevent(t, "WEDGED");
		return;
	}
	t->window_resets++;
	rc = t->reset ? t->reset(t->priv) : lkpi_tdr_default_reset(t->dev);
	if (rc) {
		t->failed_resets++;
		dev_err(t->dev, "TDR[%s]: el reinicio falla (%d)\n", t->name ? t->name : "?", rc);
	} else {
		t->resets++;
		dev_info(t->dev, "TDR[%s]: dispositivo reiniciado\n", t->name ? t->name : "?");
		tdr_uevent(t, "RESET");
	}
	t->last_progress = t->progress(t->priv);
	t->stamp = jiffies;
rearm:
	if (t->active && !t->wedged)
		schedule_delayed_work(&t->work, tdr_poll(t));
}

int lkpi_tdr_register(struct lkpi_tdr *t)
{
	if (!t || !t->dev || !t->busy || !t->progress)
		return -EINVAL;
	if (!t->timeout_ms)
		t->timeout_ms = 2000;
	if (!t->max_resets)
		t->max_resets = 3;
	if (!t->window_ms)
		t->window_ms = 60000;
	t->hangs = t->resets = t->failed_resets = 0;
	t->wedged = false;
	t->window_resets = 0;
	t->window_start = jiffies;
	t->last_progress = t->progress(t->priv);
	t->stamp = jiffies;
	t->active = true;
	INIT_DELAYED_WORK(&t->work, tdr_work);
	schedule_delayed_work(&t->work, tdr_poll(t));
	return 0;
}
EXPORT_SYMBOL(lkpi_tdr_register);

void lkpi_tdr_unregister(struct lkpi_tdr *t)
{
	if (!t || !t->work.work.func)
		return;		/* nunca se registro */
	t->active = false;
	cancel_delayed_work_sync(&t->work);
}
EXPORT_SYMBOL(lkpi_tdr_unregister);
