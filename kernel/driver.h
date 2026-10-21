// kernel/driver.h
#ifndef KERNEL_DRIVER_H
#define KERNEL_DRIVER_H

// ---------------------------------------------------------------------------
// Driver framework v2 (Fase 2.1).
//
// Cambios respecto a v1:
//   - .depends: array NULL-terminated de nombres de drivers que deben
//     haber terminado su init() antes de que el nuestro empiece. Se usa
//     solo en drivers_init_all_parallel(). En drivers_init_all() (v1
//     legacy) se ignora y se ejecuta en orden de registro.
//
// Por qué .depends y no bus_type/probe todavía:
//   - Los 6 drivers actuales (ps2, pci, ata_pio, atapi, ahci, xhci) no
//     tienen probe(device); usan init() global. Migrarlos a probe
//     requiere cambiar cada driver a la vez que se introduce bus_type,
//     lo cual es un cambio atómico grande. Se hace driver por driver en
//     Fase 2.1b.
//   - .depends es 100% aditivo: los drivers existentes funcionan
//     igual con drivers_init_all(), y drivers_init_all_parallel()
//     solo mejora el orden de arranque.
//
// Roadmap:
//   Fase 2.1a (esta entrega): .depends + init paralelo.
//   Fase 2.1b: bus_type + device + probe; migrar ahci/xhci primero.
//   Fase 3+: .kur loader con ksymtab.
//   Fase 4+: .aur user drivers.
// ---------------------------------------------------------------------------

struct driver;

// NULL-terminated list of driver names this driver depends on.
// NULL (the pointer itself) means "no dependencies".
typedef const char *const *driver_depends_t;

struct driver {
  const char *name;

  // [FASE 2.1] Dependencias. Puede ser NULL.
  driver_depends_t depends;

  int (*init)(void);
  void (*shutdown)(void);
};

// Registra un driver. Debe llamarse ANTES de drivers_init_all*().
// Idempotente respecto al orden: los drivers se procesan en el orden
// de registro en drivers_init_all(), y en orden topológico en
// drivers_init_all_parallel().
void driver_register(struct driver *d);

// [LEGACY] Inicializa secuencialmente, en orden de registro. Bloquea
// hasta completar todos. Devuelve 0 si todos OK, -1 si alguno falló.
// Ignora .depends (el llamante garantiza el orden).
int drivers_init_all(void);

// [FASE 2.1] Inicializa en paralelo respetando .depends. Cada driver
// corre en su propio kthread. Un driver espera (con timeout) a que sus
// deps terminen antes de llamar a su init(). Bloquea hasta que TODOS
// terminan o expira el timeout global. Devuelve 0 si todos OK, -1 si
// alguno falló (o timeout). Los fallos NO abortan el arranque: el
// resto de drivers sigue.
int drivers_init_all_parallel(void);

// Apaga todos los drivers en orden inverso al registro.
void drivers_shutdown_all(void);

#endif