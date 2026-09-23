// kernel/completion.h
#ifndef KERNEL_COMPLETION_H
#define KERNEL_COMPLETION_H

#include "wait.h"
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Completion: primitivo de sincronización "uno-a-muchos".
//
// Un emisor llama a complete() cuando el trabajo termina. Uno o varios
// receptores esperan con wait_for_completion() hasta que el contador
// `done` sea > 0.
//
// Semántica:
//   - complete(): incrementa done y despierta a UN waiter.
//   - complete_all(): pone done a UINT32_MAX y despierta a TODOS.
//   - wait_for_completion(): bloquea hasta done > 0.
//   - wait_for_completion_timeout(): idem, con timeout en ticks.
//   - completion_done(): true si done > 0 (sin bloquear).
//   - completion_reinit(): vuelve a estado inicial (0).
//
// Diferencia con wait_queue:
//   - wait_queue espera a una CONDICIÓN booleana evaluada por el llamante.
//   - completion espera a que un CONTADOR sea > 0, gestionado por el
//     emisor. No hay que pasar una función de condición.
//
// No se puede usar desde IRQ handler para esperar (solo para completar).
// ---------------------------------------------------------------------------

typedef struct completion {
  wait_queue_t wq;
  volatile uint32_t done;
} completion_t;

// Inicializa una completion en estado "no completada".
void completion_init(completion_t *c);

// Marca la completion como completada y despierta a UN waiter.
// Se puede llamar desde contexto de IRQ.
void complete(completion_t *c);

// Marca la completion como completada y despierta a TODOS los waiters.
// Se puede llamar desde contexto de IRQ.
void complete_all(completion_t *c);

// Bloquea hasta que done > 0. Devuelve 0.
int wait_for_completion(completion_t *c);

// Bloquea hasta que done > 0 o pasen timeout_ticks. Devuelve:
//   >0  si done > 0 (ticks restantes)
//    0   si timeout
//   <0  si interrumpida (-EINTR)
long wait_for_completion_timeout(completion_t *c, uint64_t timeout_ticks);

// True si done > 0, sin bloquear.
bool completion_done(completion_t *c);

// Reinicia la completion a estado inicial (done = 0, wq vacía).
// Solo se debe llamar cuando NO hay waiters.
void completion_reinit(completion_t *c);

#endif