// kernel/wait.c
#include "wait.h"
#include "heap.h"
#include "klog.h"
#include "sched.h"
#include <stddef.h>


#define EINTR 4

// Declarado en sched.c — lo añadimos allí (ver Paso 5).
extern void sched_make_ready(task_t *t);

void wait_queue_init(wait_queue_t *wq) {
  spin_init(&wq->lock);
  wq->head = NULL;
  wq->nr_waiting = 0;
}

// --- helpers, llamar con wq->lock cogido ---
static void wq_add_locked(wait_queue_t *wq, task_t *t) {
  wait_queue_entry_t *e = (wait_queue_entry_t *)kmalloc(sizeof(*e));
  if (!e) {
    // Sin memoria: no podemos dormir limpiamente. Mejor no bloquear.
    return;
  }
  e->task = t;
  e->next = wq->head;
  wq->head = e;
  wq->nr_waiting++;
  t->waiting_on = wq;
}

static void wq_remove_locked(wait_queue_t *wq, task_t *t) {
  wait_queue_entry_t **pp = &wq->head;
  while (*pp) {
    if ((*pp)->task == t) {
      wait_queue_entry_t *victim = *pp;
      *pp = victim->next;
      kfree(victim);
      wq->nr_waiting--;
      t->waiting_on = NULL;
      return;
    }
    pp = &(*pp)->next;
  }
}

// El núcleo del wait. IMPORTANTE: el lock de la wq se coge y se suelta
// en cada iteración, nunca se mantiene mientras se duerme. Esto evita
// el deadlock con IRQ handlers que también quieren el lock.
static int wait_common(wait_queue_t *wq, bool (*cond)(void *), void *arg,
                       bool interruptible) {
  task_t *self = sched_current();
  if (!self)
    return -1;

  // Camino rápido: condición ya cierta.
  if (!cond || cond(arg))
    return 0;

  while (1) {
    unsigned long flags = spin_lock_irqsave(&wq->lock);

    // Re-chequear con el lock cogido: cubre la race con wake_up.
    if (cond && cond(arg)) {
      spin_unlock_irqrestore(&wq->lock, flags);
      return 0;
    }

    // Si la tarea ya no está bloqueada (spurious wake, o
    // alguien la despertó justo antes de coger el lock), no
    // la añadimos: volvemos a chequear.
    if (self->waiting_on == NULL) {
      self->wake_reason = 0;
      wq_add_locked(wq, self);
      self->state = TASK_BLOCKED;
    }

    spin_unlock_irqrestore(&wq->lock, flags);

    // Dormir. Al volver, alguien nos despertó o nos canceló.
    sched_yield();

    // Re-chequear condición.
    if (cond && cond(arg)) {
      // Estamos despiertos por condición. Si aún figurábamos
      // en la wq (wake_up_one no nos sacó), sácanos.
      if (self->waiting_on == wq) {
        flags = spin_lock_irqsave(&wq->lock);
        wq_remove_locked(wq, self);
        spin_unlock_irqrestore(&wq->lock, flags);
      }
      return 0;
    }

    if (interruptible && self->wake_reason == -EINTR) {
      if (self->waiting_on == wq) {
        flags = spin_lock_irqsave(&wq->lock);
        wq_remove_locked(wq, self);
        spin_unlock_irqrestore(&wq->lock, flags);
      }
      return -EINTR;
    }

    // Spurious wakeup: volver a dormir.
    // (No puede pasar en tu implementación porque wake_up_* sólo
    //  se llama tras hacer un push, pero por seguridad.)
  }
}

int wait_event_interruptible(wait_queue_t *wq, bool (*cond)(void *),
                             void *arg) {
  return wait_common(wq, cond, arg, true);
}

void wait_event(wait_queue_t *wq, bool (*cond)(void *), void *arg) {
  (void)wait_common(wq, cond, arg, false);
}

void wake_up_all(wait_queue_t *wq) {
  unsigned long flags = spin_lock_irqsave(&wq->lock);
  wait_queue_entry_t *e = wq->head;
  wq->head = NULL;
  wq->nr_waiting = 0;
  spin_unlock_irqrestore(&wq->lock, flags);

  while (e) {
    wait_queue_entry_t *next = e->next;
    task_t *t = e->task;
    t->waiting_on = NULL;
    t->wake_reason = 0;
    sched_make_ready(t);
    kfree(e);
    e = next;
  }
}

void wake_up_one(wait_queue_t *wq) {
  unsigned long flags = spin_lock_irqsave(&wq->lock);
  if (!wq->head) {
    spin_unlock_irqrestore(&wq->lock, flags);
    return;
  }
  wait_queue_entry_t *e = wq->head;
  wq->head = e->next;
  wq->nr_waiting--;
  spin_unlock_irqrestore(&wq->lock, flags);

  e->task->waiting_on = NULL;
  e->task->wake_reason = 0;
  sched_make_ready(e->task);
  kfree(e);
}

void wake_up_interruptible_all(wait_queue_t *wq) {
  unsigned long flags = spin_lock_irqsave(&wq->lock);
  wait_queue_entry_t *e = wq->head;
  wq->head = NULL;
  wq->nr_waiting = 0;
  spin_unlock_irqrestore(&wq->lock, flags);

  while (e) {
    wait_queue_entry_t *next = e->next;
    task_t *t = e->task;
    t->waiting_on = NULL;
    t->wake_reason = -EINTR;
    sched_make_ready(t);
    kfree(e);
    e = next;
  }
}