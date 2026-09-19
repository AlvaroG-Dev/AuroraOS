// kernel/wait.c
#include "wait.h"
#include "heap.h"
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
static void wq_add_locked(wait_queue_t *wq, task_t *t) {
  wait_queue_entry_t *e = (wait_queue_entry_t *)kmalloc(sizeof(*e));
  if (!e) {
    return;
  }
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
      kfree(victim);
      wq->nr_waiting--;
      t->waiting_on = NULL;
      task_put(t);
      return;
    }
    pp = &(*pp)->next;
  }
}

static int wait_common(wait_queue_t *wq, bool (*cond)(void *), void *arg,
                       bool interruptible) {
  task_t *self = sched_current();
  if (!self)
    return -1;

  if (!cond || cond(arg))
    return 0;

  while (1) {
    unsigned long flags = spin_lock_irqsave(&wq->lock);

    if (cond && cond(arg)) {
      spin_unlock_irqrestore(&wq->lock, flags);
      return 0;
    }

    if (self->waiting_on == NULL) {
      self->wake_reason = 0;
      wq_add_locked(wq, self);
      self->state = TASK_BLOCKED;
    }

    spin_unlock_irqrestore(&wq->lock, flags);

    sched_yield();

    if (cond && cond(arg)) {
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
  }
}

int wait_event_interruptible(wait_queue_t *wq, bool (*cond)(void *),
                             void *arg) {
  return wait_common(wq, cond, arg, true);
}

void wait_event(wait_queue_t *wq, bool (*cond)(void *), void *arg) {
  (void)wait_common(wq, cond, arg, false);
}

// ---------------------------------------------------------------------------
// wake_up_all_locked: variante que asume el lock cogido.
// Saca todas las tareas de la wq y las despierta. Cada tarea pierde la
// referencia que tenía la wq sobre ella.
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
    kfree(e);
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
  if (!wq->head) {
    spin_unlock_irqrestore(&wq->lock, flags);
    return;
  }
  wait_queue_entry_t *e = wq->head;
  wq->head = e->next;
  wq->nr_waiting--;

  task_t *t = e->task;
  t->waiting_on = NULL;
  t->wake_reason = 0;
  sched_make_ready(t);
  kfree(e);
  task_put(t);

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
    kfree(e);
    task_put(t);
    e = next;
  }

  spin_unlock_irqrestore(&wq->lock, flags);
}