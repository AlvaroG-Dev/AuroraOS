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
  // Despertar a UN waiter. Los demás siguen esperando.
  wake_up_one_locked(&c->wq);
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
  long r = wait_event_interruptible(&c->wq, completion_cond, c);
  return (r < 0) ? (int)r : 0;
}

long wait_for_completion_timeout(completion_t *c, uint64_t timeout_ticks) {
  if (!c)
    return -1;
  return wait_event_interruptible_timeout(&c->wq, completion_cond, c,
                                          timeout_ticks);
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