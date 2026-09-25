// kernel/wait.c
#include "wait.h"
#include "klog.h"
#include "sched.h"
#include <stddef.h>

#define EINTR 4

void wait_queue_init(wait_queue_t *wq) {
  spin_init(&wq->lock);
  wq->head = NULL;
  wq->nr_waiting = 0;
}

// --- helpers, llamar con wq->lock cogido ---
// La entrada de cada tarea vive dentro de task_t. Así bloquear una tarea
// nunca depende de una asignación dinámica que pueda fallar justo antes
// de publicar TASK_BLOCKED.
static void wq_add_locked(wait_queue_t *wq, task_t *t) {
  wait_queue_entry_t *e = &t->wait_entry;

  e->task = t;
  e->next = wq->head;
  wq->head = e;
  wq->nr_waiting++;
  t->waiting_on = wq;
  task_get(t);
}

static void wq_remove_locked(wait_queue_t *wq, task_t *t) {
  wait_queue_entry_t **pp = &wq->head;
  while (*pp) {
    if ((*pp)->task == t) {
      wait_queue_entry_t *victim = *pp;
      *pp = victim->next;
      wq->nr_waiting--;
      t->waiting_on = NULL;
      victim->task = NULL;
      victim->next = NULL;
      task_put(t);
      return;
    }
    pp = &(*pp)->next;
  }
}

// ---------------------------------------------------------------------------
// [FIX] Núcleo común con timeout. timeout_ticks == 0 => sin timeout.
// ---------------------------------------------------------------------------
//
// Devuelve:
//   >0  condición cumplida (ticks restantes, o 1 si no había timeout)
//    0   timeout
//   <0  interrumpida (-EINTR)
//
// El contador de ticks se obtiene de sched_get_ticks() (declarado en sched.h
// como extern). Lo incrementa el handler del LAPIC timer a 1000 Hz.
// ---------------------------------------------------------------------------
static long wait_common(wait_queue_t *wq, bool (*cond)(void *), void *arg,
                        bool interruptible, uint64_t timeout_ticks) {
  task_t *self = sched_current();
  if (!self)
    return -1;

  if (!cond || cond(arg))
    return (timeout_ticks == 0) ? 1 : (long)timeout_ticks;

  uint64_t deadline = 0;
  if (timeout_ticks > 0)
    deadline = sched_get_ticks() + timeout_ticks;

  while (1) {
    unsigned long flags = spin_lock_irqsave(&wq->lock);

    if (cond && cond(arg)) {
      spin_unlock_irqrestore(&wq->lock, flags);
      if (timeout_ticks == 0)
        return 1;
      uint64_t now = sched_get_ticks();
      long remaining = (deadline > now) ? (long)(deadline - now) : 1;
      return remaining;
    }

    if (self->waiting_on == NULL) {
      self->wake_reason = 0;
      wq_add_locked(wq, self);
      // [C4] Publicación atómica de (state, wake_deadline) bajo
      // sched_lock, para que sched_wake_expired (que lee ambos campos
      // bajo el mismo lock) nunca vea una tarea BLOCKED con deadline
      // aún sin escribir, ni al revés.
      sched_set_blocked_deadline(self, deadline);
    }

    spin_unlock_irqrestore(&wq->lock, flags);

    sched_yield();

    // [FIX timeout] Si despertamos por deadline, hay que limpiarlo
    // (sched_wake_expired ya lo hace, pero por si acaso).
    __atomic_store_n(&self->wake_deadline, 0, __ATOMIC_RELEASE);

    if (cond && cond(arg)) {
      if (self->waiting_on == wq) {
        flags = spin_lock_irqsave(&wq->lock);
        wq_remove_locked(wq, self);
        spin_unlock_irqrestore(&wq->lock, flags);
      }
      if (timeout_ticks == 0)
        return 1;
      uint64_t now = sched_get_ticks();
      long remaining = (deadline > now) ? (long)(deadline - now) : 1;
      return remaining;
    }

    if (interruptible && self->wake_reason == -EINTR) {
      if (self->waiting_on == wq) {
        flags = spin_lock_irqsave(&wq->lock);
        wq_remove_locked(wq, self);
        spin_unlock_irqrestore(&wq->lock, flags);
      }
      return -EINTR;
    }

    if (timeout_ticks > 0) {
      uint64_t now = sched_get_ticks();
      if (now >= deadline) {
        if (self->waiting_on == wq) {
          flags = spin_lock_irqsave(&wq->lock);
          wq_remove_locked(wq, self);
          spin_unlock_irqrestore(&wq->lock, flags);
        }
        return 0;
      }
    }
  }
}

// [FIX] Implementación de la nueva API con timeout.
long wait_event_interruptible_timeout(wait_queue_t *wq, bool (*cond)(void *),
                                      void *arg, uint64_t timeout_ticks) {
  return wait_common(wq, cond, arg, true, timeout_ticks);
}

int wait_event_interruptible(wait_queue_t *wq, bool (*cond)(void *),
                             void *arg) {
  long r = wait_common(wq, cond, arg, true, 0);
  return (r < 0) ? (int)r : 0;
}

void wait_event(wait_queue_t *wq, bool (*cond)(void *), void *arg) {
  (void)wait_common(wq, cond, arg, false, 0);
}

// ---------------------------------------------------------------------------
// wake_up_all_locked y amigos (sin cambios)
// ---------------------------------------------------------------------------
void wake_up_all_locked(wait_queue_t *wq) {
  wait_queue_entry_t *e = wq->head;
  wq->head = NULL;
  wq->nr_waiting = 0;

  while (e) {
    wait_queue_entry_t *next = e->next;
    task_t *t = e->task;
    t->waiting_on = NULL;
    t->wake_reason = 0;
    sched_make_ready(t);
    e->task = NULL;
    e->next = NULL;
    task_put(t);
    e = next;
  }
}

void wake_up_all(wait_queue_t *wq) {
  unsigned long flags = spin_lock_irqsave(&wq->lock);
  wake_up_all_locked(wq);
  spin_unlock_irqrestore(&wq->lock, flags);
}

void wake_up_one(wait_queue_t *wq) {
  unsigned long flags = spin_lock_irqsave(&wq->lock);
  wake_up_one_locked(wq);
  spin_unlock_irqrestore(&wq->lock, flags);
}

void wake_up_interruptible_all(wait_queue_t *wq) {
  unsigned long flags = spin_lock_irqsave(&wq->lock);
  wait_queue_entry_t *e = wq->head;
  wq->head = NULL;
  wq->nr_waiting = 0;

  while (e) {
    wait_queue_entry_t *next = e->next;
    task_t *t = e->task;
    t->waiting_on = NULL;
    t->wake_reason = -EINTR;
    sched_make_ready(t);
    e->task = NULL;
    e->next = NULL;
    task_put(t);
    e = next;
  }

  spin_unlock_irqrestore(&wq->lock, flags);
}

void wake_up_one_locked(wait_queue_t *wq) {
  if (!wq->head)
    return;
  wait_queue_entry_t *e = wq->head;
  wq->head = e->next;
  wq->nr_waiting--;

  task_t *t = e->task;
  t->waiting_on = NULL;
  t->wake_reason = 0;
  e->task = NULL;
  e->next = NULL;
  sched_make_ready(t);
  task_put(t);
}