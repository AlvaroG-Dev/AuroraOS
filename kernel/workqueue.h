// kernel/workqueue.h
#ifndef KERNEL_WORKQUEUE_H
#define KERNEL_WORKQUEUE_H

#include "wait.h"
#include <stdbool.h>
#include <stdint.h>

struct work_struct;
struct workqueue_struct;

// Estados de un work_struct (para inspección y asserts; no tocar desde
// fuera de workqueue.c).
#define WORK_IDLE 0
#define WORK_QUEUED 1
#define WORK_RUNNING 2

typedef void (*work_func_t)(struct work_struct *work);

#define WQ_SYSTEM_WORKERS 8

// ---------------------------------------------------------------------------
// Trabajo diferido.
//
// Se puede incrustar en structs propias. Inicialización:
//
//   static void my_work_fn(struct work_struct *w) { ... }
//   struct work_struct my_work;
//   INIT_WORK(&my_work, my_work_fn);
//
// Reglas:
//   - Un work_struct está ligado a UNA workqueue a la vez.
//   - queue_work / queue_delayed_work devuelven false si ya estaba
//     encolado o ejecutándose (idempotente: NO se re-encola).
//   - flush_work espera a que termine la instancia actual, si está
//     encolada o ejecutándose.
//   - cancel_work_sync quita el work de la cola o espera a que termine.
// ---------------------------------------------------------------------------
struct work_struct {
  work_func_t fn;
  struct work_struct *next;
  uint64_t deadline;           // tick absoluto; 0 = inmediato
  int state;                   // WORK_*
  struct workqueue_struct *wq; // workqueue actual (NULL si idle)
  wait_queue_t done_wq;        // despertada al terminar fn
};

#define __WORK_INIT_STATIC(f)                                                  \
  {.fn = (f),                                                                  \
   .next = NULL,                                                               \
   .deadline = 0,                                                              \
   .state = WORK_IDLE,                                                         \
   .wq = NULL,                                                                 \
   .done_wq = WAIT_QUEUE_INIT(work)}

#define WORK_INIT_STATIC(name, f)                                              \
  struct work_struct name = __WORK_INIT_STATIC(f)

static inline void init_work_impl(struct work_struct *w, work_func_t f) {
  w->fn = f;
  w->next = NULL;
  w->deadline = 0;
  w->state = WORK_IDLE;
  w->wq = NULL;
  wait_queue_init(&w->done_wq);
}

#define INIT_WORK(w, f) init_work_impl((w), (f))

// ---------------------------------------------------------------------------
// Workqueues.
//
//   alloc_workqueue(name, nr_workers, ordered):
//     nr_workers <= 0  → un worker por CPU (MAX_CPUS).
//     ordered != 0     → fuerza un solo worker (ejecución secuencial).
//
// Recomendación:
//   system_wq           → trabajo corto, paralelo (un worker por CPU).
//   system_ordered_wq   → trabajo que debe serializarse (AML, ACPI, ...).
// ---------------------------------------------------------------------------
struct workqueue_struct *alloc_workqueue(const char *name, int nr_workers,
                                         int ordered);

// Para la workqueue: espera a los workers en curso, para los kthreads y
// descarta el trabajo pendiente que nunca llegó a ejecutarse (con aviso).
void destroy_workqueue(struct workqueue_struct *wq);

bool queue_work(struct workqueue_struct *wq, struct work_struct *work);
bool queue_delayed_work(struct workqueue_struct *wq, struct work_struct *work,
                        uint64_t ticks);

// Espera a que `work` esté idle (nunca arrancó, ya terminó, o fue cancelado).
// No espera a futuras re-encolaciones.
bool flush_work(struct work_struct *work);

// Espera a que no haya trabajo inmediato encolado ni en ejecución.
// NO espera a `delayed_list` (eso lo maneja cancel_work_sync o
// destroy_workqueue).
void flush_workqueue(struct workqueue_struct *wq);

// Quita el trabajo de la cola si aún no empezó; si ya está corriendo,
// espera a que termine. Devuelve true si había trabajo pendiente.
bool cancel_work_sync(struct work_struct *work);

// Colas globales (creadas por workqueue_init).
extern struct workqueue_struct *system_wq;
extern struct workqueue_struct *system_ordered_wq;

// One-shot. Debe llamarse después de sched_init() e ipi_init().
// Idempotente. Devuelve 0 en éxito, -ENOMEM si no pudo crear workers.
int workqueue_init(void);

// Parada ordenada. flush_workqueue sobre las globales + destroy.
void workqueue_shutdown(void);

#endif