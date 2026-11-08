// SPDX-License-Identifier: GPL-2.0-only
/*
 * Gestion de energia del modelo de dispositivos (L9.2 y L9.3).
 *
 * 1. Runtime PM (pm_runtime_*): motor con estados ACTIVE / SUSPENDING /
 *    SUSPENDED / RESUMING, contador de usos, autosuspend con temporizador y
 *    callbacks runtime_suspend / runtime_resume / runtime_idle elegidos como
 *    Linux (pm_domain > type > class > bus > driver). Solo es real con
 *    `lkpi.runtime_pm=on`; sin eso las funciones solo cuentan y el
 *    dispositivo esta siempre activo (ver linux/pm_runtime.h).
 *
 * 2. Suspend/resume del sistema: lkpi_pm_suspend_devices() recorre los
 *    dispositivos en orden inverso al de registro (hijos antes que padres)
 *    con las fases prepare / suspend / suspend_late / suspend_noirq y
 *    lkpi_pm_resume_devices() las deshace en orden de registro. Apaga la
 *    IOMMU al final y la vuelve a encender al principio. La fase noirq se
 *    ejecuta con las IRQ habilitadas (Aurora no las enmascara globalmente
 *    desde aqui). La entrada real en S3 depende de Plan_Power.md; mientras
 *    tanto `echo devices > /sys/power/state` hace el ciclo completo sin
 *    dormir (equivalente a pm_test=devices de Linux).
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/kstrtox.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/pm_runtime.h>
#include "../aurora.h"
#include "../lkpi_internal.h"

/* ------------------------------------------------------------ politica */

bool lkpi_pm_runtime_policy(void)
{
	static int state;	/* 0 sin leer, 1 apagado, 2 activo */
	char v[16];

	if (!state) {
		state = (aurora_cmdline_get("lkpi.runtime_pm", v, sizeof(v)) &&
			 (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "auto"))) ? 2 : 1;
		if (state == 2)
			pr_info("linuxkpi: runtime PM activo (lkpi.runtime_pm=on)\n");
	}
	return state == 2;
}
EXPORT_SYMBOL(lkpi_pm_runtime_policy);

/* ------------------------------------------------------------- estado */

enum rpm_req { RPM_REQ_NONE, RPM_REQ_IDLE, RPM_REQ_SUSPEND, RPM_REQ_AUTOSUSPEND, RPM_REQ_RESUME };

struct lkpi_rpm {
	struct device *dev;
	struct mutex lock;		/* serializa los callbacks */
	struct delayed_work work;
	enum rpm_req req;
};

static DEFINE_SPINLOCK(lkpi_rpm_alloc_lock);

static void rpm_work_fn(struct work_struct *w);

static struct lkpi_rpm *rpm_of(struct device *dev)
{
	struct lkpi_rpm *r = dev->power.rpm;
	unsigned long flags;

	if (r)
		return r;
	r = kzalloc(sizeof(*r), GFP_ATOMIC);
	if (!r)
		return NULL;
	r->dev = dev;
	mutex_init(&r->lock);
	INIT_DELAYED_WORK(&r->work, rpm_work_fn);
	spin_lock_irqsave(&lkpi_rpm_alloc_lock, flags);
	if (dev->power.rpm) {
		spin_unlock_irqrestore(&lkpi_rpm_alloc_lock, flags);
		kfree(r);
		return dev->power.rpm;
	}
	dev->power.rpm = r;
	spin_unlock_irqrestore(&lkpi_rpm_alloc_lock, flags);
	return r;
}

/* Operaciones de energia del dispositivo, como Linux. */
static const struct dev_pm_ops *rpm_ops(struct device *dev)
{
	if (dev->pm_domain)
		return &dev->pm_domain->ops;
	if (dev->type && dev->type->pm)
		return dev->type->pm;
	if (dev->class && dev->class->pm)
		return dev->class->pm;
	if (dev->bus && dev->bus->pm)
		return dev->bus->pm;
	if (dev->driver && dev->driver->pm)
		return dev->driver->pm;
	return NULL;
}

/* ------------------------------------------------------- motor: resume */

static int rpm_resume_dev(struct device *dev)
{
	struct lkpi_rpm *r;
	const struct dev_pm_ops *ops;
	int rc = 0;

	if (dev->power.runtime_status == RPM_ACTIVE)
		return 1;
	if (dev->power.disable_depth > 0)
		return -EACCES;
	if (dev->power.no_callbacks) {
		dev->power.runtime_status = RPM_ACTIVE;
		return 0;
	}
	r = rpm_of(dev);
	if (!r)
		return -ENOMEM;
	mutex_lock(&r->lock);
	if (dev->power.runtime_status == RPM_ACTIVE) {
		mutex_unlock(&r->lock);
		return 1;
	}
	/* El padre despierta antes (si gestiona su energia). */
	if (dev->parent && dev->parent->power.runtime_status == RPM_SUSPENDED &&
	    !dev->parent->power.disable_depth)
		rpm_resume_dev(dev->parent);
	dev->power.runtime_status = RPM_RESUMING;
	ops = rpm_ops(dev);
	if (ops && ops->runtime_resume)
		rc = ops->runtime_resume(dev);
	if (rc) {
		dev->power.runtime_status = RPM_SUSPENDED;
		dev->power.runtime_error = rc;
		dev_err(dev, "runtime_resume fallo: %d\n", rc);
	} else {
		dev->power.runtime_status = RPM_ACTIVE;
		dev->power.runtime_error = 0;
		dev->power.rpm_resumes++;
		dev->power.last_busy = jiffies;
	}
	mutex_unlock(&r->lock);
	return rc;
}

/* ---------------------------------------------------- motor: suspender */

static int rpm_suspend_dev(struct device *dev, bool autosuspend)
{
	struct lkpi_rpm *r;
	const struct dev_pm_ops *ops;
	int rc = 0;

	if (dev->power.runtime_status == RPM_SUSPENDED)
		return 1;
	if (dev->power.disable_depth > 0)
		return -EACCES;
	if (atomic_read(&dev->power.usage_count) > 0)
		return -EAGAIN;
	r = rpm_of(dev);
	if (!r)
		return -ENOMEM;
	if (autosuspend && dev->power.use_autosuspend) {
		unsigned long expires = dev->power.last_busy +
					msecs_to_jiffies(dev->power.autosuspend_delay);

		if (time_before(jiffies, expires)) {
			r->req = RPM_REQ_AUTOSUSPEND;
			mod_delayed_work(system_wq, &r->work, expires - jiffies);
			return -EAGAIN;
		}
	}
	mutex_lock(&r->lock);
	if (dev->power.runtime_status != RPM_ACTIVE) {
		mutex_unlock(&r->lock);
		return dev->power.runtime_status == RPM_SUSPENDED ? 1 : -EAGAIN;
	}
	if (atomic_read(&dev->power.usage_count) > 0) {
		mutex_unlock(&r->lock);
		return -EAGAIN;
	}
	if (dev->power.no_callbacks) {
		dev->power.runtime_status = RPM_SUSPENDED;
		mutex_unlock(&r->lock);
		return 0;
	}
	dev->power.runtime_status = RPM_SUSPENDING;
	ops = rpm_ops(dev);
	if (ops && ops->runtime_suspend)
		rc = ops->runtime_suspend(dev);
	if (rc) {
		dev->power.runtime_status = RPM_ACTIVE;
		/* -EAGAIN / -EBUSY: el driver no quiere dormir ahora; no es un error. */
		if (rc != -EAGAIN && rc != -EBUSY) {
			dev->power.runtime_error = rc;
			dev_err(dev, "runtime_suspend fallo: %d\n", rc);
		}
		dev->power.last_busy = jiffies;
	} else {
		dev->power.runtime_status = RPM_SUSPENDED;
		dev->power.rpm_suspends++;
	}
	mutex_unlock(&r->lock);
	return rc;
}

/* runtime_idle: el driver decide; sin callback, o si devuelve 0, se suspende. */
static int rpm_idle_dev(struct device *dev, bool autosuspend)
{
	const struct dev_pm_ops *ops;
	int rc = 0;

	if (atomic_read(&dev->power.usage_count) > 0 || dev->power.disable_depth > 0 ||
	    dev->power.runtime_status != RPM_ACTIVE)
		return -EAGAIN;
	ops = rpm_ops(dev);
	if (ops && ops->runtime_idle)
		rc = ops->runtime_idle(dev);
	if (rc)
		return rc;
	return rpm_suspend_dev(dev, autosuspend);
}

static void rpm_work_fn(struct work_struct *w)
{
	struct lkpi_rpm *r = container_of(to_delayed_work(w), struct lkpi_rpm, work);
	enum rpm_req req = r->req;

	r->req = RPM_REQ_NONE;
	switch (req) {
	case RPM_REQ_IDLE:
		rpm_idle_dev(r->dev, true);
		break;
	case RPM_REQ_SUSPEND:
		rpm_suspend_dev(r->dev, false);
		break;
	case RPM_REQ_AUTOSUSPEND:
		rpm_suspend_dev(r->dev, true);
		break;
	case RPM_REQ_RESUME:
		rpm_resume_dev(r->dev);
		break;
	default:
		break;
	}
}

static void rpm_queue(struct device *dev, enum rpm_req req, unsigned long delay)
{
	struct lkpi_rpm *r = rpm_of(dev);

	if (!r)
		return;
	r->req = req;
	mod_delayed_work(system_wq, &r->work, delay);
}

/* --------------------------------------------------------- API publica */

void lkpi_pm_runtime_enable(struct device *dev)
{
	if (!lkpi_pm_runtime_policy()) {
		dev->power.disable_depth = 0;
		return;
	}
	if (dev->power.disable_depth > 0)
		dev->power.disable_depth--;
	else
		dev_warn(dev, "pm_runtime_enable sin su pm_runtime_disable\n");
}
EXPORT_SYMBOL(lkpi_pm_runtime_enable);

void lkpi_pm_runtime_disable(struct device *dev)
{
	if (!lkpi_pm_runtime_policy()) {
		dev->power.disable_depth = 1;
		return;
	}
	lkpi_pm_runtime_barrier(dev);
	dev->power.disable_depth++;
}
EXPORT_SYMBOL(lkpi_pm_runtime_disable);

void lkpi_pm_runtime_barrier(struct device *dev)
{
	struct lkpi_rpm *r = dev->power.rpm;

	if (r)
		cancel_delayed_work_sync(&r->work);
}
EXPORT_SYMBOL(lkpi_pm_runtime_barrier);

int lkpi_pm_runtime_get_sync(struct device *dev)
{
	atomic_inc(&dev->power.usage_count);
	if (!lkpi_pm_runtime_policy())
		return 1;
	return rpm_resume_dev(dev);
}
EXPORT_SYMBOL(lkpi_pm_runtime_get_sync);

int lkpi_pm_runtime_resume_and_get(struct device *dev)
{
	int rc;

	atomic_inc(&dev->power.usage_count);
	if (!lkpi_pm_runtime_policy())
		return 0;
	rc = rpm_resume_dev(dev);
	if (rc < 0) {
		atomic_dec(&dev->power.usage_count);
		return rc;
	}
	return 0;
}
EXPORT_SYMBOL(lkpi_pm_runtime_resume_and_get);

int lkpi_pm_runtime_get(struct device *dev)
{
	atomic_inc(&dev->power.usage_count);
	if (!lkpi_pm_runtime_policy())
		return 1;
	if (dev->power.runtime_status == RPM_ACTIVE)
		return 1;
	if (dev->power.disable_depth > 0)
		return -EACCES;
	rpm_queue(dev, RPM_REQ_RESUME, 0);
	return 0;
}
EXPORT_SYMBOL(lkpi_pm_runtime_get);

/* Solo si ya esta en uso (contador > 0) y activo; no despierta. */
int lkpi_pm_runtime_get_if_in_use(struct device *dev)
{
	if (!lkpi_pm_runtime_policy()) {
		if (atomic_read(&dev->power.usage_count) <= 0)
			return 0;
		atomic_inc(&dev->power.usage_count);
		return 1;
	}
	if (dev->power.disable_depth > 0)
		return -EINVAL;
	if (dev->power.runtime_status != RPM_ACTIVE || atomic_read(&dev->power.usage_count) <= 0)
		return 0;
	atomic_inc(&dev->power.usage_count);
	return 1;
}
EXPORT_SYMBOL(lkpi_pm_runtime_get_if_in_use);

int lkpi_pm_runtime_get_if_active(struct device *dev)
{
	if (!lkpi_pm_runtime_policy()) {
		atomic_inc(&dev->power.usage_count);
		return 1;
	}
	if (dev->power.disable_depth > 0)
		return -EINVAL;
	if (dev->power.runtime_status != RPM_ACTIVE)
		return 0;
	atomic_inc(&dev->power.usage_count);
	return 1;
}
EXPORT_SYMBOL(lkpi_pm_runtime_get_if_active);

static int rpm_put(struct device *dev, bool sync, bool autosuspend)
{
	int rc = 0;

	if (!atomic_dec_and_test(&dev->power.usage_count))
		return 0;
	if (!lkpi_pm_runtime_policy())
		return 0;
	if (sync)
		rc = rpm_idle_dev(dev, autosuspend);
	else
		rpm_queue(dev, autosuspend ? RPM_REQ_IDLE : RPM_REQ_SUSPEND, 0);
	return rc;
}

int lkpi_pm_runtime_put(struct device *dev)
{
	if (!lkpi_pm_runtime_policy()) {
		atomic_dec(&dev->power.usage_count);
		return 0;
	}
	/* put sin autosuspend: idle asincrono (que acaba en suspender). */
	if (atomic_dec_and_test(&dev->power.usage_count))
		rpm_queue(dev, RPM_REQ_IDLE, 0);
	return 0;
}
EXPORT_SYMBOL(lkpi_pm_runtime_put);

int lkpi_pm_runtime_put_sync(struct device *dev)
{
	if (!lkpi_pm_runtime_policy()) {
		atomic_dec(&dev->power.usage_count);
		return 0;
	}
	return rpm_put(dev, true, false);
}
EXPORT_SYMBOL(lkpi_pm_runtime_put_sync);

int lkpi_pm_runtime_put_autosuspend(struct device *dev)
{
	if (!lkpi_pm_runtime_policy()) {
		atomic_dec(&dev->power.usage_count);
		return 0;
	}
	return rpm_put(dev, false, true);
}
EXPORT_SYMBOL(lkpi_pm_runtime_put_autosuspend);

int lkpi_pm_runtime_put_sync_autosuspend(struct device *dev)
{
	if (!lkpi_pm_runtime_policy()) {
		atomic_dec(&dev->power.usage_count);
		return 0;
	}
	return rpm_put(dev, true, true);
}
EXPORT_SYMBOL(lkpi_pm_runtime_put_sync_autosuspend);

void lkpi_pm_runtime_use_autosuspend(struct device *dev, bool use)
{
	dev->power.use_autosuspend = use;
}
EXPORT_SYMBOL(lkpi_pm_runtime_use_autosuspend);

void lkpi_pm_runtime_set_autosuspend_delay(struct device *dev, int delay)
{
	dev->power.autosuspend_delay = delay < 0 ? 0 : delay;
}
EXPORT_SYMBOL(lkpi_pm_runtime_set_autosuspend_delay);

int lkpi_pm_runtime_set_active(struct device *dev)
{
	if (!lkpi_pm_runtime_policy())
		return 0;
	if (dev->power.runtime_status == RPM_ACTIVE)
		return 0;
	dev->power.runtime_status = RPM_ACTIVE;
	dev->power.runtime_error = 0;
	return 0;
}
EXPORT_SYMBOL(lkpi_pm_runtime_set_active);

void lkpi_pm_runtime_set_suspended(struct device *dev)
{
	if (lkpi_pm_runtime_policy())
		dev->power.runtime_status = RPM_SUSPENDED;
}
EXPORT_SYMBOL(lkpi_pm_runtime_set_suspended);

/* power/control = auto: quita la referencia de "forbid" y permite dormir. */
void lkpi_pm_runtime_allow(struct device *dev)
{
	if (!lkpi_pm_runtime_policy() || dev->power.runtime_auto)
		return;
	dev->power.runtime_auto = true;
	if (atomic_dec_and_test(&dev->power.usage_count))
		rpm_queue(dev, RPM_REQ_IDLE, 0);
}
EXPORT_SYMBOL(lkpi_pm_runtime_allow);

/* power/control = on: toma una referencia y despierta el dispositivo. */
void lkpi_pm_runtime_forbid(struct device *dev)
{
	if (!lkpi_pm_runtime_policy() || !dev->power.runtime_auto)
		return;
	dev->power.runtime_auto = false;
	atomic_inc(&dev->power.usage_count);
	rpm_resume_dev(dev);
}
EXPORT_SYMBOL(lkpi_pm_runtime_forbid);

int lkpi_pm_runtime_idle(struct device *dev)
{
	if (!lkpi_pm_runtime_policy())
		return -ENOSYS;
	return rpm_idle_dev(dev, false);
}
EXPORT_SYMBOL(lkpi_pm_runtime_idle);

int lkpi_pm_runtime_suspend(struct device *dev)
{
	if (!lkpi_pm_runtime_policy())
		return -EAGAIN;
	return rpm_suspend_dev(dev, false);
}
EXPORT_SYMBOL(lkpi_pm_runtime_suspend);

int lkpi_pm_runtime_resume(struct device *dev)
{
	if (!lkpi_pm_runtime_policy())
		return 0;
	return rpm_resume_dev(dev);
}
EXPORT_SYMBOL(lkpi_pm_runtime_resume);

int lkpi_pm_request_resume(struct device *dev)
{
	if (!lkpi_pm_runtime_policy() || dev->power.runtime_status == RPM_ACTIVE)
		return 1;
	rpm_queue(dev, RPM_REQ_RESUME, 0);
	return 0;
}
EXPORT_SYMBOL(lkpi_pm_request_resume);

int lkpi_pm_schedule_suspend(struct device *dev, unsigned int delay)
{
	if (!lkpi_pm_runtime_policy())
		return 0;
	if (!delay)
		return rpm_suspend_dev(dev, false);
	rpm_queue(dev, RPM_REQ_SUSPEND, msecs_to_jiffies(delay));
	return 0;
}
EXPORT_SYMBOL(lkpi_pm_schedule_suspend);

bool lkpi_pm_runtime_suspended(struct device *dev)
{
	return lkpi_pm_runtime_policy() && dev->power.runtime_status == RPM_SUSPENDED &&
	       !dev->power.disable_depth;
}
EXPORT_SYMBOL(lkpi_pm_runtime_suspended);

bool lkpi_pm_runtime_active(struct device *dev)
{
	return !lkpi_pm_runtime_policy() || dev->power.runtime_status == RPM_ACTIVE ||
	       dev->power.disable_depth;
}
EXPORT_SYMBOL(lkpi_pm_runtime_active);

/* ----------------------------------------------- sysfs: <dev>/power/ */

static const char *rpm_status_name(struct device *dev)
{
	if (dev->power.disable_depth)
		return "unsupported";
	switch (dev->power.runtime_status) {
	case RPM_ACTIVE:
		return "active";
	case RPM_SUSPENDED:
		return "suspended";
	case RPM_SUSPENDING:
		return "suspending";
	default:
		return "resuming";
	}
}

static ssize_t control_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%s\n",
			  lkpi_pm_runtime_policy() && dev->power.runtime_auto ? "auto" : "on");
}
static ssize_t control_store(struct device *dev, struct device_attribute *a, const char *buf,
			     size_t n)
{
	if (sysfs_streq(buf, "auto")) {
		if (!lkpi_pm_runtime_policy())
			return -EPERM;	/* hace falta lkpi.runtime_pm=on */
		pm_runtime_allow(dev);
	} else if (sysfs_streq(buf, "on")) {
		pm_runtime_forbid(dev);
	} else {
		return -EINVAL;
	}
	return n;
}
static DEVICE_ATTR_RW(control);

static ssize_t runtime_status_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%s\n", rpm_status_name(dev));
}
static DEVICE_ATTR_RO(runtime_status);

static ssize_t runtime_usage_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&dev->power.usage_count));
}
static DEVICE_ATTR_RO(runtime_usage);

static ssize_t autosuspend_delay_ms_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%u\n", dev->power.autosuspend_delay);
}
static ssize_t autosuspend_delay_ms_store(struct device *dev, struct device_attribute *a,
					  const char *buf, size_t n)
{
	int v;

	if (kstrtoint(buf, 10, &v))
		return -EINVAL;
	pm_runtime_set_autosuspend_delay(dev, v);
	return n;
}
static DEVICE_ATTR_RW(autosuspend_delay_ms);

static ssize_t runtime_cycles_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%u %u\n", dev->power.rpm_suspends, dev->power.rpm_resumes);
}
static DEVICE_ATTR_RO(runtime_cycles);

static struct attribute *lkpi_power_attrs[] = {
	&dev_attr_control.attr,
	&dev_attr_runtime_status.attr,
	&dev_attr_runtime_usage.attr,
	&dev_attr_autosuspend_delay_ms.attr,
	&dev_attr_runtime_cycles.attr,
	NULL,
};
const struct attribute_group lkpi_power_attr_group = {
	.name = "power",
	.attrs = lkpi_power_attrs,
};
EXPORT_SYMBOL(lkpi_power_attr_group);

/* ======================================================================
 * Suspend / resume del sistema (L9.3)
 * ====================================================================== */

static LIST_HEAD(lkpi_dpm_list);
static DEFINE_MUTEX(lkpi_dpm_mutex);
static DEFINE_MUTEX(lkpi_dpm_cycle_mutex);	/* un ciclo a la vez */

struct lkpi_pm_stats lkpi_pm_stats;
EXPORT_SYMBOL(lkpi_pm_stats);

void lkpi_pm_device_added(struct device *dev)
{
	mutex_lock(&lkpi_dpm_mutex);
	if (list_empty(&dev->lkpi_pm_node))
		list_add_tail(&dev->lkpi_pm_node, &lkpi_dpm_list);
	mutex_unlock(&lkpi_dpm_mutex);
}

void lkpi_pm_device_removed(struct device *dev)
{
	mutex_lock(&lkpi_dpm_mutex);
	list_del_init(&dev->lkpi_pm_node);
	mutex_unlock(&lkpi_dpm_mutex);
}

/* Instantanea de los dispositivos con referencia (el recorrido llama a
 * drivers que pueden dormir y no puede tener el mutex de la lista). */
static struct device **dpm_snapshot(int *n)
{
	struct device *d, **v;
	int cnt = 0, i = 0;

	mutex_lock(&lkpi_dpm_mutex);
	list_for_each_entry(d, &lkpi_dpm_list, lkpi_pm_node)
		cnt++;
	v = kcalloc(cnt ? cnt : 1, sizeof(*v), GFP_KERNEL);
	if (v)
		list_for_each_entry(d, &lkpi_dpm_list, lkpi_pm_node)
			v[i++] = get_device(d);
	mutex_unlock(&lkpi_dpm_mutex);
	*n = v ? i : 0;
	return v;
}

static void dpm_snapshot_put(struct device **v, int n)
{
	for (int i = 0; i < n; i++)
		put_device(v[i]);
	kfree(v);
}

enum dpm_phase { DPM_PREPARE, DPM_SUSPEND, DPM_LATE, DPM_NOIRQ };

static int dpm_call(struct device *dev, enum dpm_phase ph, bool resume)
{
	const struct dev_pm_ops *ops = rpm_ops(dev);
	int (*cb)(struct device *) = NULL;

	if (!ops)
		return 0;
	switch (ph) {
	case DPM_SUSPEND:
		cb = resume ? ops->resume : ops->suspend;
		break;
	case DPM_LATE:
		cb = resume ? ops->resume_early : ops->suspend_late;
		break;
	case DPM_NOIRQ:
		cb = resume ? ops->resume_noirq : ops->suspend_noirq;
		break;
	default:
		break;
	}
	return cb ? cb(dev) : 0;
}

/* Los dispositivos que llegaron a una fase se anotan para deshacerla. */
#define DPM_DONE_PREPARE 1
#define DPM_DONE_SUSPEND 2
#define DPM_DONE_LATE 4
#define DPM_DONE_NOIRQ 8

static struct device **dpm_devs;
static u8 *dpm_done;
static int dpm_ndevs;
static bool dpm_suspended;

static void dpm_resume_all(void)
{
	int i;

	/* Orden de registro (padres primero): noirq, early, resume, complete. */
	for (i = 0; i < dpm_ndevs; i++) {
		if (dpm_done[i] & DPM_DONE_NOIRQ) {
			int rc = dpm_call(dpm_devs[i], DPM_NOIRQ, true);

			if (rc) {
				dev_err(dpm_devs[i], "resume_noirq fallo: %d\n", rc);
				lkpi_pm_stats.errors++;
			}
		}
	}
	for (i = 0; i < dpm_ndevs; i++) {
		if (dpm_done[i] & DPM_DONE_LATE) {
			int rc = dpm_call(dpm_devs[i], DPM_LATE, true);

			if (rc) {
				dev_err(dpm_devs[i], "resume_early fallo: %d\n", rc);
				lkpi_pm_stats.errors++;
			}
		}
	}
	for (i = 0; i < dpm_ndevs; i++) {
		if (dpm_done[i] & DPM_DONE_SUSPEND) {
			int rc = dpm_call(dpm_devs[i], DPM_SUSPEND, true);

			if (rc) {
				dev_err(dpm_devs[i], "resume fallo: %d\n", rc);
				lkpi_pm_stats.errors++;
			}
			dpm_devs[i]->power.is_suspended = false;
		}
	}
	for (i = 0; i < dpm_ndevs; i++) {
		if (dpm_done[i] & DPM_DONE_PREPARE) {
			const struct dev_pm_ops *ops = rpm_ops(dpm_devs[i]);

			if (ops && ops->complete)
				(ops->complete)(dpm_devs[i]);
			dpm_devs[i]->power.is_prepared = false;
		}
	}
	dpm_snapshot_put(dpm_devs, dpm_ndevs);
	kfree(dpm_done);
	dpm_devs = NULL;
	dpm_done = NULL;
	dpm_ndevs = 0;
	dpm_suspended = false;
}

int lkpi_pm_suspend_devices(void)
{
	int i, rc = 0;

	mutex_lock(&lkpi_dpm_cycle_mutex);
	if (dpm_suspended) {
		mutex_unlock(&lkpi_dpm_cycle_mutex);
		return -EBUSY;
	}
	dpm_devs = dpm_snapshot(&dpm_ndevs);
	if (!dpm_devs && dpm_ndevs == 0) {
		/* snapshot vacio o sin memoria */
	}
	dpm_done = kzalloc(dpm_ndevs ? dpm_ndevs : 1, GFP_KERNEL);
	if (!dpm_devs || !dpm_done) {
		kfree(dpm_done);
		dpm_snapshot_put(dpm_devs, dpm_ndevs);
		dpm_devs = NULL;
		dpm_done = NULL;
		dpm_ndevs = 0;
		mutex_unlock(&lkpi_dpm_cycle_mutex);
		return -ENOMEM;
	}
	dpm_suspended = true;

	/* prepare en orden de registro. */
	for (i = 0; i < dpm_ndevs && !rc; i++) {
		const struct dev_pm_ops *ops = rpm_ops(dpm_devs[i]);

		if (ops && ops->prepare)
			rc = ops->prepare(dpm_devs[i]);
		if (rc) {
			dev_err(dpm_devs[i], "prepare fallo: %d\n", rc);
			break;
		}
		dpm_devs[i]->power.is_prepared = true;
		dpm_done[i] |= DPM_DONE_PREPARE;
	}
	/* suspend / late / noirq en orden inverso (hijos antes que padres). */
	for (i = dpm_ndevs - 1; i >= 0 && !rc; i--) {
		rc = dpm_call(dpm_devs[i], DPM_SUSPEND, false);
		if (rc) {
			dev_err(dpm_devs[i], "suspend fallo: %d\n", rc);
			break;
		}
		dpm_devs[i]->power.is_suspended = true;
		dpm_done[i] |= DPM_DONE_SUSPEND;
	}
	for (i = dpm_ndevs - 1; i >= 0 && !rc; i--) {
		rc = dpm_call(dpm_devs[i], DPM_LATE, false);
		if (rc) {
			dev_err(dpm_devs[i], "suspend_late fallo: %d\n", rc);
			break;
		}
		dpm_done[i] |= DPM_DONE_LATE;
	}
	for (i = dpm_ndevs - 1; i >= 0 && !rc; i--) {
		rc = dpm_call(dpm_devs[i], DPM_NOIRQ, false);
		if (rc) {
			dev_err(dpm_devs[i], "suspend_noirq fallo: %d\n", rc);
			break;
		}
		dpm_done[i] |= DPM_DONE_NOIRQ;
	}
	if (rc) {
		/* Deshacer lo hecho; los demas dispositivos nunca se tocaron. */
		lkpi_pm_stats.errors++;
		dpm_resume_all();
		mutex_unlock(&lkpi_dpm_cycle_mutex);
		return rc;
	}
	aurora_iommu_suspend();
	lkpi_pm_stats.suspends++;
	mutex_unlock(&lkpi_dpm_cycle_mutex);
	return 0;
}
EXPORT_SYMBOL(lkpi_pm_suspend_devices);

void lkpi_pm_resume_devices(void)
{
	mutex_lock(&lkpi_dpm_cycle_mutex);
	if (!dpm_suspended) {
		mutex_unlock(&lkpi_dpm_cycle_mutex);
		return;
	}
	if (aurora_iommu_resume())
		pr_err("linuxkpi: la IOMMU no vuelve tras suspender\n");
	dpm_resume_all();
	lkpi_pm_stats.resumes++;
	mutex_unlock(&lkpi_dpm_cycle_mutex);
}
EXPORT_SYMBOL(lkpi_pm_resume_devices);

int lkpi_pm_cycle_devices(void)
{
	int rc = lkpi_pm_suspend_devices();

	if (rc)
		return rc;
	lkpi_pm_resume_devices();
	return 0;
}
EXPORT_SYMBOL(lkpi_pm_cycle_devices);

/* ------------------------------------------------ /sys/power/state */

static struct kobject *lkpi_power_kobj;

static ssize_t state_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return sysfs_emit(buf, "devices\n");
}
static ssize_t state_store(struct kobject *k, struct kobj_attribute *a, const char *buf, size_t n)
{
	int rc;

	if (!sysfs_streq(buf, "devices"))
		return -EINVAL;	/* la entrada real en S3 llega con Plan_Power.md */
	rc = lkpi_pm_cycle_devices();
	return rc ? rc : n;
}
static struct kobj_attribute state_attr = __ATTR(state, 0644, state_show, state_store);

static ssize_t pm_stats_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return sysfs_emit(buf, "suspends %u\nresumes %u\nerrors %u\nruntime_pm %s\n",
			  lkpi_pm_stats.suspends, lkpi_pm_stats.resumes, lkpi_pm_stats.errors,
			  lkpi_pm_runtime_policy() ? "on" : "off");
}
static struct kobj_attribute pm_stats_attr = __ATTR(pm_stats, 0444, pm_stats_show, NULL);

int lkpi_pm_init(void)
{
	lkpi_power_kobj = kobject_create_and_add("power", NULL);
	if (!lkpi_power_kobj)
		return -ENOMEM;
	sysfs_create_file(lkpi_power_kobj, &state_attr.attr);
	sysfs_create_file(lkpi_power_kobj, &pm_stats_attr.attr);
	return 0;
}

void lkpi_pm_exit(void)
{
	if (!lkpi_power_kobj)
		return;
	sysfs_remove_file(lkpi_power_kobj, &state_attr.attr);
	sysfs_remove_file(lkpi_power_kobj, &pm_stats_attr.attr);
	kobject_put(lkpi_power_kobj);
	lkpi_power_kobj = NULL;
}
