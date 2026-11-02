/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apagado y reinicio (API de linux/reboot.h hasta Linux 6.18). La implementacion
 * esta en kernel/linuxkpi/kernel/reboot.c y delega en power.c de Aurora
 * (Plan_Power.md, P7).
 */
#ifndef _LKPI_LINUX_REBOOT_H
#define _LKPI_LINUX_REBOOT_H

#include <linux/types.h>
#include <linux/notifier.h>
#include <linux/compiler.h>

struct device;

enum reboot_mode {
	REBOOT_UNDEFINED = -1,
	REBOOT_COLD = 0,
	REBOOT_WARM,
	REBOOT_HARD,
	REBOOT_SOFT,
	REBOOT_GPIO,
};
extern enum reboot_mode reboot_mode;
extern enum reboot_mode panic_reboot_mode;

enum reboot_type {
	BOOT_TRIPLE = 't',
	BOOT_KBD = 'k',
	BOOT_BIOS = 'b',
	BOOT_ACPI = 'a',
	BOOT_EFI = 'e',
	BOOT_CF9_FORCE = 'p',
	BOOT_CF9_SAFE = 'q',
};
extern enum reboot_type reboot_type;

extern int reboot_default;
extern int reboot_cpu;
extern int reboot_force;

/* Eventos de reboot_notifier_list (valor de `action`). */
#define SYS_DOWN	0x0001	/* Notificacion de reinicio/parada/apagado */
#define SYS_RESTART	SYS_DOWN
#define SYS_HALT	0x0002
#define SYS_POWER_OFF	0x0003

/* Comandos de reboot(2). */
#define LINUX_REBOOT_CMD_RESTART	0x01234567
#define LINUX_REBOOT_CMD_HALT		0xCDEF0123
#define LINUX_REBOOT_CMD_CAD_ON		0x89ABCDEF
#define LINUX_REBOOT_CMD_CAD_OFF	0x00000000
#define LINUX_REBOOT_CMD_POWER_OFF	0x4321FEDC
#define LINUX_REBOOT_CMD_RESTART2	0xA1B2C3D4
#define LINUX_REBOOT_CMD_SW_SUSPEND	0xD000FCE2
#define LINUX_REBOOT_CMD_KEXEC		0x45584543

int register_reboot_notifier(struct notifier_block *nb);
int unregister_reboot_notifier(struct notifier_block *nb);
int devm_register_reboot_notifier(struct device *dev, struct notifier_block *nb);

/* Cadena de reinicio (handlers de plataforma/driver). */
int register_restart_handler(struct notifier_block *nb);
int unregister_restart_handler(struct notifier_block *nb);
void do_kernel_restart(char *cmd);

/* API moderna sys_off (Linux >= 5.17). */
enum sys_off_mode {
	SYS_OFF_MODE_POWER_OFF_PREPARE,
	SYS_OFF_MODE_POWER_OFF,
	SYS_OFF_MODE_RESTART_PREPARE,
	SYS_OFF_MODE_RESTART,
};

enum sys_off_priority {
	SYS_OFF_PRIO_PLATFORM = -256,
	SYS_OFF_PRIO_LOW = -128,
	SYS_OFF_PRIO_DEFAULT = 0,
	SYS_OFF_PRIO_HIGH = 192,
	SYS_OFF_PRIO_FIRMWARE = 224,
};

struct sys_off_data {
	int mode;
	void *cb_data;
	const char *cmd;
	struct device *dev;
};

struct sys_off_handler;

struct sys_off_handler *
register_sys_off_handler(enum sys_off_mode mode, int priority,
			 int (*callback)(struct sys_off_data *data), void *cb_data);
void unregister_sys_off_handler(struct sys_off_handler *handler);
int devm_register_sys_off_handler(struct device *dev, enum sys_off_mode mode, int priority,
				  int (*callback)(struct sys_off_data *data), void *cb_data);
int devm_register_power_off_handler(struct device *dev,
				    int (*callback)(struct sys_off_data *data), void *cb_data);
int devm_register_restart_handler(struct device *dev,
				  int (*callback)(struct sys_off_data *data), void *cb_data);
int register_platform_power_off(void (*power_off)(void));
void unregister_platform_power_off(void (*power_off)(void));

extern void (*pm_power_off)(void);
extern void (*pm_power_off_prepare)(void);

void do_kernel_power_off(void);
bool kernel_can_power_off(void);

/* Acciones: delegan en power_request() de Aurora; las que no vuelven lo marcan. */
void kernel_restart_prepare(char *cmd);
void kernel_restart(char *cmd);
void kernel_halt(void);
void kernel_power_off(void);
void emergency_restart(void);
void machine_restart(char *cmd);
void machine_halt(void);
void machine_power_off(void);
void machine_emergency_restart(void);
void ctrl_alt_del(void);

int orderly_poweroff(bool force);
int orderly_reboot(void);
void hw_failure_emergency_poweroff(int poweroff_delay_ms);
void __hw_protection_shutdown(const char *reason, int ms_until_forced, bool shutdown);

static inline void hw_protection_reboot(const char *reason, int ms_until_forced)
{
	__hw_protection_shutdown(reason, ms_until_forced, false);
}

static inline void hw_protection_shutdown(const char *reason, int ms_until_forced)
{
	__hw_protection_shutdown(reason, ms_until_forced, true);
}

/* reboot_pid_ns / kexec: no hay contenedores ni kexec en Aurora. */
extern bool kexec_in_progress;

/* Recorre los dispositivos de todos los buses y llama a su shutdown. */
void device_shutdown(void);

#endif /* _LKPI_LINUX_REBOOT_H */
