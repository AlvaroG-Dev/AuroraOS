// kernel/completion.c
#include "completion.h"
#include "sched.h"
#include <stddef.h>

// ---------------------------------------------------------------------------
// Condición interna para wait_common. La completion es "done" cuando
// el contador es > 0.
// ---------------------------------------------------------------------------
static bool completion_cond(void *arg) {
  completion_t *c = (completion_t *)arg;
  return c->done > 0;
}

void completion_init(completion_t *c) {
  if (!c)
    return;
  wait_queue_init(&c->wq);
  c->done = 0;
}

void complete(completion_t *c) {
  if (!c)
    return;

  unsigned long flags = spin_lock_irqsave(&c->wq.lock);
  if (c->done < UINT32_MAX)
    c->done++;
  // La cola de completion es privada; no propaga eventos de epoll.
  wake_up_one_direct_locked(&c->wq);
  spin_unlock_irqrestore(&c->wq.lock, flags);
}

void complete_all(completion_t *c) {
  if (!c)
    return;

  unsigned long flags = spin_lock_irqsave(&c->wq.lock);
  c->done = UINT32_MAX;
  // Despertar a TODOS. Tras esto, done ya no es 0 y cualquier nuevo
  // waiter verá la condición cumplida inmediatamente.
  wake_up_all_locked(&c->wq);
  spin_unlock_irqrestore(&c->wq.lock, flags);
}

int wait_for_completion(completion_t *c) {
  if (!c)
    return -1;
  wait_event(&c->wq, completion_cond, c);
  unsigned long flags = spin_lock_irqsave(&c->wq.lock);
  if (c->done > 0 && c->done < UINT32_MAX)
    c->done--;
  spin_unlock_irqrestore(&c->wq.lock, flags);
  return 0;
}

long wait_for_completion_timeout(completion_t *c, uint64_t timeout_ticks) {
  if (!c)
    return -1;
  long ret = wait_event_timeout(&c->wq, completion_cond, c, timeout_ticks);
  if (ret > 0) {
    unsigned long flags = spin_lock_irqsave(&c->wq.lock);
    if (c->done > 0 && c->done < UINT32_MAX)
      c->done--;
    spin_unlock_irqrestore(&c->wq.lock, flags);
  }
  return ret;
}

bool completion_done(completion_t *c) {
  if (!c)
    return true;
  return c->done > 0;
}

void completion_reinit(completion_t *c) {
  if (!c)
    return;
  // No hay waiters por contrato; limpiamos el contador.
  c->done = 0;
}

// ---------------------------------------------------------------------------
// [FIX] wait_for_completion_uninterruptible: bloquear HASTA que el bio
// termine, sin ceder ante señales.
//
// wait_for_completion (interruptible) puede devolver -EINTR si llega
// una señal (SIGCHLD, SIGWINCH, ...). Eso es correcto para syscalls de
// usuario (read() puede ser interrumpido), pero INCORRECTO para un
// bio en vuelo: el hardware sigue haciendo DMA y llamará a bio_endio
// cuando termine. Si el llamante se fue, el bio apunta a stack muerta
// y el IRQ handler dispara #GP o peor.
//
// La solución es exactamente la que usa Linux: block I/O síncrono se
// espera con wait_for_completion() UNINTERRUPTIBLE.
// ---------------------------------------------------------------------------
void wait_for_completion_uninterruptible(completion_t *c) {
  if (!c)
    return;
  wait_event(&c->wq, completion_cond, c);
  unsigned long flags = spin_lock_irqsave(&c->wq.lock);
  if (c->done > 0 && c->done < UINT32_MAX)
    c->done--;
  spin_unlock_irqrestore(&c->wq.lock, flags);
}