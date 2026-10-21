// kernel/driver.c
#include "driver.h"
#include "completion.h"
#include "klog.h"
#include "kthread.h"
#include "sched.h"
#include "string.h"
#include <stddef.h>

#define MAX_DRIVERS 32

// Timeout global (ticks a 1 kHz) para drivers_init_all_parallel.
// Debe ser mayor que el timeout por dep para que un dep colgado no
// haga expirar el global sin que el resto haya intentado nada.
#define DRIVER_GLOBAL_TIMEOUT_MS 30000
#define DRIVER_DEP_TIMEOUT_MS 30000

static struct driver *drivers[MAX_DRIVERS];
static int driver_count = 0;

void driver_register(struct driver *d) {
  if (!d) {
    LOG_ERR("[DRIVER] driver nulo");
    return;
  }
  if (driver_count >= MAX_DRIVERS) {
    LOG_ERR("[DRIVER] registro lleno (max=%u)", MAX_DRIVERS);
    return;
  }
  drivers[driver_count++] = d;
}

// ---------------------------------------------------------------------------
// [LEGACY] Init secuencial. Sin cambios de comportamiento.
// ---------------------------------------------------------------------------
int drivers_init_all(void) {
  int failures = 0;
  for (int i = 0; i < driver_count; i++) {
    LOG_INFO("[DRIVER] Inicializando '%s'...", drivers[i]->name);
    if (drivers[i]->init) {
      int ret = drivers[i]->init();
      if (ret != 0) {
        LOG_WARN("[DRIVER] '%s' falló con %d, continuando...", drivers[i]->name,
                 ret);
        failures++;
      }
    }
  }
  return failures ? -1 : 0;
}

// ---------------------------------------------------------------------------
// [FASE 2.1] Init paralelo con dependencias.
//
// Cada driver corre en su propio kthread. Antes de llamar a d->init(),
// el worker espera (con timeout) a que TODOS los drivers en d->depends
// hayan llamado a complete() sobre su slot.
//
// Por qué kthreads y no solo completions: un driver puede bloquear
// (ahci hace port_wait_ready con sched_yield, xhci hace USB enumeration
// con waits). Si todos corrieran en el mismo hilo caller, no
// paralelizaríamos nada. Los kthreads permiten que los drivers
// duerman mientras otros hacen progreso.
//
// Orden de log: los "Inicializando 'X'..." salen del worker, no del
// caller. Eso significa que en el boot los verás intercalados con
// otros drivers, no en un bloque ordenado. Es de esperar.
// ---------------------------------------------------------------------------

struct driver_slot {
  struct driver *drv;
  completion_t done;
  int rc;
  volatile int finished;
};

static struct driver_slot g_slots[MAX_DRIVERS];
static int g_slot_count = 0;

// Busca un slot por nombre y espera su completion.
// Devuelve 1 si completó (o si el nombre no está registrado, para no
// colgar por deps huérfanas), 0 en timeout.
static int wait_driver_by_name(const char *name, uint64_t timeout_ticks) {
  for (int i = 0; i < g_slot_count; i++) {
    if (g_slots[i].drv && strcmp(g_slots[i].drv->name, name) == 0) {
      return wait_for_completion_timeout(&g_slots[i].done, timeout_ticks) > 0;
    }
  }
  // Dep no registrada: tratarla como satisfecha y avisar una vez.
  LOG_WARN("[DRIVER] dependencia '%s' no registrada, ignorando", name);
  return 1;
}

static int driver_worker(void *arg) {
  struct driver_slot *slot = (struct driver_slot *)arg;
  struct driver *d = slot->drv;

  // 1. Esperar dependencias.
  if (d->depends) {
    for (int i = 0; d->depends[i]; i++) {
      if (!wait_driver_by_name(d->depends[i], DRIVER_DEP_TIMEOUT_MS)) {
        // No abortamos: es peor fallar por ordering que intentar el
        // init con una dep lenta. Log y continuar.
        LOG_WARN("[DRIVER] %s: timeout esperando dep '%s'", d->name,
                 d->depends[i]);
      }
    }
  }

  // 2. Init del driver.
  int rc = 0;
  if (d->init) {
    LOG_INFO("[DRIVER] Inicializando '%s'...", d->name);
    rc = d->init();
    if (rc != 0) {
      LOG_WARN("[DRIVER] '%s' falló con %d, continuando...", d->name, rc);
    }
  }

  // 3. Publicar resultado.
  slot->rc = rc;
  slot->finished = 1;
  // complete_all: múltiples drivers pueden esperar la misma dep (ej: ata_pio, ahci, xhci
  // todos esperan pci). complete() solo despertaría a UNO, los demás esperarían
  // DRIVER_DEP_TIMEOUT_MS (30s). complete_all() despierta a todos a la vez.
  complete_all(&slot->done);
  return rc;
}

int drivers_init_all_parallel(void) {
  if (driver_count == 0)
    return 0;

  g_slot_count = driver_count;
  for (int i = 0; i < g_slot_count; i++) {
    g_slots[i].drv = drivers[i];
    completion_init(&g_slots[i].done);
    g_slots[i].rc = 0;
    g_slots[i].finished = 0;
  }

  struct kthread *threads[MAX_DRIVERS];
  memset(threads, 0, sizeof(threads));

  // Lanzar todos los workers. Los que tienen deps esperarán dentro.
  for (int i = 0; i < g_slot_count; i++) {
    // Nombre de kthread: "drv-<name>" truncado a 15 chars.
    char tname[16];
    int p = 0;
    const char *pre = "drv-";
    while (*pre && p < 15)
      tname[p++] = *pre++;
    const char *n = g_slots[i].drv->name;
    while (*n && p < 15)
      tname[p++] = *n++;
    tname[p] = '\0';

    threads[i] = kthread_run(driver_worker, &g_slots[i], tname);
    if (!threads[i]) {
      LOG_ERR("[DRIVER] no se pudo crear worker para '%s'",
              g_slots[i].drv->name);
      // Marcar completado para que los dependientes no se cuelguen.
      g_slots[i].finished = 1;
      g_slots[i].rc = -1;
      complete_all(&g_slots[i].done); // complete_all: despierta deps de este driver que fallaron al crear worker
    }
  }

  // Esperar a todos con timeout global. Este bucle consume las
  // completions una a una; como los workers corren en paralelo, el
  // tiempo total es ~max(t_init) + overhead, no la suma.
  uint64_t deadline = sched_get_ticks() + DRIVER_GLOBAL_TIMEOUT_MS;
  for (int i = 0; i < g_slot_count; i++) {
    uint64_t now = sched_get_ticks();
    uint64_t remaining = (deadline > now) ? (deadline - now) : 1;
    if (wait_for_completion_timeout(&g_slots[i].done, remaining) == 0) {
      LOG_ERR("[DRIVER] timeout global esperando '%s'", g_slots[i].drv->name);
    }
  }

  // Reapear los kthreads. Todos deberían haber retornado ya (sus
  // completions están seteadas); kthread_stop es no-op de cleanup.
  for (int i = 0; i < g_slot_count; i++) {
    if (threads[i])
      (void)kthread_stop(threads[i]);
  }

  int failures = 0;
  for (int i = 0; i < g_slot_count; i++) {
    if (!g_slots[i].finished || g_slots[i].rc != 0)
      failures++;
  }

  LOG_INFO("[DRIVER] init paralelo completado (%d/%d OK)",
           g_slot_count - failures, g_slot_count);
  return failures ? -1 : 0;
}

void drivers_shutdown_all(void) {
  for (int i = driver_count - 1; i >= 0; i--) {
    if (drivers[i]->shutdown) {
      LOG_INFO("[DRIVER] Apagando '%s'...", drivers[i]->name);
      drivers[i]->shutdown();
    }
  }
}