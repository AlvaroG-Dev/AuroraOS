// kernel/power.h
//
// Apagado, reinicio y parada del sistema (Plan_Power.md, P0-P2).
//
// power_request() hace el apagado ordenado (notificadores, SIGTERM/SIGKILL,
// flush de discos, shutdown de drivers) y despues recorre una cadena de
// backends hasta que uno apaga o reinicia la maquina. No vuelve.
//
// Opciones de cmdline:
//   reboot=efi|pci|kbd|triple    backend de reinicio que se prueba primero
//   poweroff=efi                 probar UEFI ResetSystem antes que ACPI S5
//   efi=noruntime                no usar UEFI ResetSystem
//   power.debug=1                (reservado) trazas extra
#ifndef KERNEL_POWER_H
#define KERNEL_POWER_H

#include <stdint.h>

typedef enum {
  POWER_ACTION_OFF = 0,
  POWER_ACTION_RESTART,
  POWER_ACTION_HALT, // parar CPUs, equipo encendido
} power_action_t;

// Fases en las que se llama a los notificadores, en orden de prioridad
// descendente dentro de cada fase:
//   PREPARE  al empezar, antes de tocar procesos y discos (puede dormir).
//   DEVICES  tras vaciar discos y drivers nativos, antes de `cli`: apagar
//            dispositivos (LinuxKPI: device_shutdown). Puede dormir.
//   FINAL    con interrupciones desactivadas, justo antes de los backends:
//            handlers de apagado/reinicio de drivers. No debe dormir.
#define POWER_PHASE_PREPARE 0
#define POWER_PHASE_DEVICES 1
#define POWER_PHASE_FINAL 2

// Notificadores (equivalente a reboot_notifier de Linux).
typedef void (*power_notifier_fn)(power_action_t action, int phase, void *ctx);
int power_register_notifier(power_notifier_fn fn, void *ctx, int prio);
int power_unregister_notifier(power_notifier_fn fn, void *ctx);

// Apagado ordenado + backends. No vuelve. `reason` solo para el log.
__attribute__((noreturn)) void power_request(power_action_t action,
                                             const char *reason);

// Backends de bajo nivel, sin preparacion (para panic y pruebas). Vuelven
// si el intento falla. Las interrupciones ya deben estar desactivadas.
void power_backend_restart(void);
void power_backend_poweroff(void);

#endif
