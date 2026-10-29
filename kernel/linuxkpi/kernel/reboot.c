// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apagado y reinicio para drivers de Linux (Plan_Power.md, P7).
 *
 * Aurora decide el orden (power.c: procesos, discos, drivers nativos) y la
 * capa se engancha con un notificador de 3 fases:
 *   PREPARE  cadena reboot_notifier_list y handlers *_PREPARE.
 *   DEVICES  device_shutdown(): bus->shutdown / driver->shutdown, en orden
 *            inverso al de registro.
 *   FINAL    (interrupciones desactivadas) handlers sys_off de apagado o
 *            reinicio, cadena de reinicio y pm_power_off. Si vuelven, Aurora
 *            sigue con ACPI/UEFI/puertos.
 * kernel_restart(), kernel_power_off() y demas llaman a power_request().
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/reboot.h>
#include <linux/notifier.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/workqueue.h>

#include "../aurora.h"
#include "../lkpi_internal.h"

enum system_states system_state = SYSTEM_RUNNING;
EXPORT_SYMBOL(system_state);

enum reboot_mode reboot_mode = REBOOT_COLD;
EXPORT_SYMBOL(reboot_mode);
enum reboot_mode panic_reboot_mode = REBOOT_UNDEFINED;
EXPORT_SYMBOL(panic_reboot_mode);
enum reboot_type reboot_type = BOOT_ACPI;
EXPORT_SYMBOL(reboot_type);
int reboot_default = 1;
EXPORT_SYMBOL(reboot_default);
int reboot_cpu;
EXPORT_SYMBOL(reboot_cpu);
int reboot_force;
EXPORT_SYMBOL(reboot_force);
bool kexec_in_progress;
EXPORT_SYMBOL(kexec_in_progress);

void (*pm_power_off)(void);
EXPORT_SYMBOL(pm_power_off);
void (*pm_power_off_prepare)(void);
EXPORT_SYMBOL(pm_power_off_prepare);

static BLOCKING_NOTIFIER_HEAD(reboot_notifier_list);
static ATOMIC_NOTIFIER_HEAD(restart_handler_list);

int register_reboot_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&reboot_notifier_list, nb);
}
EXPORT_SYMBOL(register_reboot_notifier);

int unregister_reboot_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&reboot_notifier_list, nb);
}
EXPORT_SYMBOL(unregister_reboot_notifier);

static void lkpi_devm_unregister_reboot_notifier(void *res)
{
	WARN_ON(unregister_reboot_notifier(res));
}

int devm_register_reboot_notifier(struct device *dev, struct notifier_block *nb)
{
	int ret = register_reboot_notifier(nb);

	if (ret)
		return ret;
	return devm_add_action_or_reset(dev, lkpi_devm_unregister_reboot_notifier, nb);
}
EXPORT_SYMBOL(devm_register_reboot_notifier);

int register_restart_handler(struct notifier_block *nb)
{
	return atomic_notifier_chain_register(&restart_handler_list, nb);
}
EXPORT_SYMBOL(register_restart_handler);

int unregister_restart_handler(struct notifier_block *nb)
{
	return atomic_notifier_chain_unregister(&restart_handler_list, nb);
}
EXPORT_SYMBOL(unregister_restart_handler);

/* ------------------------------------------------------------------------ */
/* sys_off                                                                  */
/* ------------------------------------------------------------------------ */
struct sys_off_handler {
	struct list_head node;
	enum sys_off_mode mode;
	int priority;
	int (*callback)(struct sys_off_data *data);
	void *cb_data;
	struct device *dev;
};

static struct list_head lkpi_sys_off[4] = {
	LIST_HEAD_INIT(lkpi_sys_off[0]), LIST_HEAD_INIT(lkpi_sys_off[1]),
	LIST_HEAD_INIT(lkpi_sys_off[2]), LIST_HEAD_INIT(lkpi_sys_off[3]),
};
static DEFINE_MUTEX(lkpi_sys_off_mutex);

struct sys_off_handler *
register_sys_off_handler(enum sys_off_mode mode, int priority,
			 int (*callback)(struct sys_off_data *data), void *cb_data)
{
	struct sys_off_handler *h, *it;

	if (!callback || (unsigned int)mode > SYS_OFF_MODE_RESTART)
		return ERR_PTR(-EINVAL);
	h = kzalloc(sizeof(*h), GFP_KERNEL);
	if (!h)
		return ERR_PTR(-ENOMEM);
	h->mode = mode;
	h->priority = priority;
	h->callback = callback;
	h->cb_data = cb_data;

	mutex_lock(&lkpi_sys_off_mutex);
	/* Orden de prioridad descendente; a igual prioridad, el ultimo antes. */
	list_for_each_entry(it, &lkpi_sys_off[mode], node)
		if (it->priority <= priority)
			break;
	list_add_tail(&h->node, &it->node == &lkpi_sys_off[mode] ? &lkpi_sys_off[mode] : &it->node);
	mutex_unlock(&lkpi_sys_off_mutex);
	return h;
}
EXPORT_SYMBOL(register_sys_off_handler);

void unregister_sys_off_handler(struct sys_off_handler *h)
{
	if (IS_ERR_OR_NULL(h))
		return;
	mutex_lock(&lkpi_sys_off_mutex);
	list_del(&h->node);
	mutex_unlock(&lkpi_sys_off_mutex);
	kfree(h);
}
EXPORT_SYMBOL(unregister_sys_off_handler);

static void lkpi_devm_unregister_sys_off(void *res)
{
	unregister_sys_off_handler(res);
}

int devm_register_sys_off_handler(struct device *dev, enum sys_off_mode mode, int priority,
				  int (*callback)(struct sys_off_data *data), void *cb_data)
{
	struct sys_off_handler *h = register_sys_off_handler(mode, priority, callback, cb_data);

	if (IS_ERR(h))
		return PTR_ERR(h);
	h->dev = dev;
	return devm_add_action_or_reset(dev, lkpi_devm_unregister_sys_off, h);
}
EXPORT_SYMBOL(devm_register_sys_off_handler);

int devm_register_power_off_handler(struct device *dev,
				    int (*callback)(struct sys_off_data *data), void *cb_data)
{
	return devm_register_sys_off_handler(dev, SYS_OFF_MODE_POWER_OFF, SYS_OFF_PRIO_DEFAULT,
					     callback, cb_data);
}
EXPORT_SYMBOL(devm_register_power_off_handler);

int devm_register_restart_handler(struct device *dev,
				  int (*callback)(struct sys_off_data *data), void *cb_data)
{
	return devm_register_sys_off_handler(dev, SYS_OFF_MODE_RESTART, SYS_OFF_PRIO_DEFAULT,
					     callback, cb_data);
}
EXPORT_SYMBOL(devm_register_restart_handler);

static int lkpi_platform_power_off_cb(struct sys_off_data *data)
{
	void (*fn)(void) = data->cb_data;

	fn();
	return NOTIFY_DONE;
}

int register_platform_power_off(void (*power_off)(void))
{
	struct sys_off_handler *h = register_sys_off_handler(
		SYS_OFF_MODE_POWER_OFF, SYS_OFF_PRIO_PLATFORM, lkpi_platform_power_off_cb,
		power_off);

	return IS_ERR(h) ? PTR_ERR(h) : 0;
}
EXPORT_SYMBOL(register_platform_power_off);

void unregister_platform_power_off(void (*power_off)(void))
{
	struct sys_off_handler *h, *found = NULL;

	mutex_lock(&lkpi_sys_off_mutex);
	list_for_each_entry(h, &lkpi_sys_off[SYS_OFF_MODE_POWER_OFF], node)
		if (h->callback == lkpi_platform_power_off_cb && h->cb_data == power_off) {
			found = h;
			break;
		}
	mutex_unlock(&lkpi_sys_off_mutex);
	unregister_sys_off_handler(found);
}
EXPORT_SYMBOL(unregister_platform_power_off);

/* Sin bloqueo: en la fase FINAL las interrupciones estan desactivadas. */
static void lkpi_run_sys_off(enum sys_off_mode mode, const char *cmd)
{
	struct sys_off_handler *h;

	list_for_each_entry(h, &lkpi_sys_off[mode], node) {
		struct sys_off_data data = {
			.mode = mode, .cb_data = h->cb_data, .cmd = cmd, .dev = h->dev,
		};

		if (h->callback(&data) & NOTIFY_STOP_MASK)
			break;
	}
}

void do_kernel_restart(char *cmd)
{
	atomic_notifier_call_chain(&restart_handler_list, reboot_mode, cmd);
}
EXPORT_SYMBOL(do_kernel_restart);

void do_kernel_power_off(void)
{
	lkpi_run_sys_off(SYS_OFF_MODE_POWER_OFF, NULL);
	if (pm_power_off)
		pm_power_off();
}
EXPORT_SYMBOL(do_kernel_power_off);

bool kernel_can_power_off(void)
{
	return pm_power_off || !list_empty(&lkpi_sys_off[SYS_OFF_MODE_POWER_OFF]);
}
EXPORT_SYMBOL(kernel_can_power_off);

/* ------------------------------------------------------------------------ */
/* Enganche con power.c                                                     */
/* ------------------------------------------------------------------------ */
static void lkpi_power_notify(int action, int phase, void *ctx)
{
	unsigned long ev = action == AURORA_POWER_OFF ? SYS_POWER_OFF :
			   action == AURORA_POWER_HALT ? SYS_HALT : SYS_RESTART;

	switch (phase) {
	case AURORA_POWER_PHASE_PREPARE:
		system_state = action == AURORA_POWER_OFF ? SYSTEM_POWER_OFF :
			       action == AURORA_POWER_HALT ? SYSTEM_HALT : SYSTEM_RESTART;
		blocking_notifier_call_chain(&reboot_notifier_list, ev, NULL);
		if (action == AURORA_POWER_OFF) {
			mutex_lock(&lkpi_sys_off_mutex);
			lkpi_run_sys_off(SYS_OFF_MODE_POWER_OFF_PREPARE, NULL);
			mutex_unlock(&lkpi_sys_off_mutex);
			if (pm_power_off_prepare)
				pm_power_off_prepare();
		} else if (action == AURORA_POWER_RESTART) {
			mutex_lock(&lkpi_sys_off_mutex);
			lkpi_run_sys_off(SYS_OFF_MODE_RESTART_PREPARE, NULL);
			mutex_unlock(&lkpi_sys_off_mutex);
		}
		break;
	case AURORA_POWER_PHASE_DEVICES:
		device_shutdown();
		break;
	case AURORA_POWER_PHASE_FINAL:
		if (action == AURORA_POWER_OFF) {
			do_kernel_power_off();
		} else if (action == AURORA_POWER_RESTART) {
			lkpi_run_sys_off(SYS_OFF_MODE_RESTART, NULL);
			do_kernel_restart(NULL);
		}
		break;
	}
}

void kernel_restart_prepare(char *cmd)
{
	blocking_notifier_call_chain(&reboot_notifier_list, SYS_RESTART, cmd);
}
EXPORT_SYMBOL(kernel_restart_prepare);

void kernel_restart(char *cmd)
{
	pr_emerg("Restarting system%s%s\n", cmd ? " with command " : "", cmd ? cmd : "");
	aurora_power_request(AURORA_POWER_RESTART, "linuxkpi kernel_restart");
}
EXPORT_SYMBOL(kernel_restart);

void kernel_halt(void)
{
	pr_emerg("System halted\n");
	aurora_power_request(AURORA_POWER_HALT, "linuxkpi kernel_halt");
}
EXPORT_SYMBOL(kernel_halt);

void kernel_power_off(void)
{
	pr_emerg("Power down\n");
	aurora_power_request(AURORA_POWER_OFF, "linuxkpi kernel_power_off");
}
EXPORT_SYMBOL(kernel_power_off);

void emergency_restart(void)
{
	aurora_power_request(AURORA_POWER_RESTART, "linuxkpi emergency_restart");
}
EXPORT_SYMBOL(emergency_restart);

void machine_restart(char *cmd) { kernel_restart(cmd); }
void machine_halt(void) { kernel_halt(); }
void machine_power_off(void) { kernel_power_off(); }
void machine_emergency_restart(void) { emergency_restart(); }
EXPORT_SYMBOL(machine_restart);
EXPORT_SYMBOL(machine_halt);
EXPORT_SYMBOL(machine_power_off);
EXPORT_SYMBOL(machine_emergency_restart);

void ctrl_alt_del(void)
{
	/* Aurora aun no enlaza Ctrl-Alt-Del con init (Plan_Power.md P6). */
}
EXPORT_SYMBOL(ctrl_alt_del);

/* orderly_*: el hilo de trabajo hace el apagado ordenado de Aurora (SIGTERM,
 * volcado de discos, drivers). Pueden llamarse desde contextos atomicos
 * (termico, RAS), por eso van por una workqueue. */
static void lkpi_orderly_fn(struct work_struct *w);
static DECLARE_WORK(lkpi_orderly_work, lkpi_orderly_fn);
static int lkpi_orderly_action = AURORA_POWER_OFF;

static void lkpi_orderly_fn(struct work_struct *w)
{
	aurora_power_request(lkpi_orderly_action, "linuxkpi orderly");
}

int orderly_poweroff(bool force)
{
	lkpi_orderly_action = AURORA_POWER_OFF;
	schedule_work(&lkpi_orderly_work);
	return 0;
}
EXPORT_SYMBOL(orderly_poweroff);

int orderly_reboot(void)
{
	lkpi_orderly_action = AURORA_POWER_RESTART;
	schedule_work(&lkpi_orderly_work);
	return 0;
}
EXPORT_SYMBOL(orderly_reboot);

void hw_failure_emergency_poweroff(int poweroff_delay_ms)
{
	aurora_power_request(AURORA_POWER_OFF, "linuxkpi hw_failure_emergency_poweroff");
}
EXPORT_SYMBOL(hw_failure_emergency_poweroff);

void __hw_protection_shutdown(const char *reason, int ms_until_forced, bool shutdown)
{
	pr_emerg("HARDWARE PROTECTION %s (%s)\n", shutdown ? "shutdown" : "restart", reason);
	if (shutdown)
		orderly_poweroff(true);
	else
		orderly_reboot();
}
EXPORT_SYMBOL(__hw_protection_shutdown);

int lkpi_reboot_init(void)
{
	system_state = SYSTEM_RUNNING;
	/* Prioridad alta: los dispositivos de Linux se apagan antes que nada nativo
	 * de fase DEVICES con prioridad menor. */
	return aurora_power_register_notifier(lkpi_power_notify, NULL, 0);
}

void lkpi_reboot_exit(void)
{
	aurora_power_unregister_notifier(lkpi_power_notify, NULL);
}
