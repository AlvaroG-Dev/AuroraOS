// kernel/wait.c
#include "wait.h"
#include "klog.h"
#include "process.h"
#include "sched.h"
#include <stddef.h>

#define EINTR 4

// [FIX JOB] Señales que el wait debe procesar internamente, sin
// devolver EINTR. Linux hace lo mismo: nanosleep/pause/read no
// retornan EINTR por SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU/SIGCONT.
//
//   - Las cuatro de parada: el wait pasa a TASK_STOPPED y solo sale
//     cuando llega SIGCONT (que lo reactiva).
//   - SIGCONT: no-op si no estaba stopped, y si lo estaba ya lo
//     reanudó process_signal_pid_ex. En ambos casos, el wait debe
//     seguir durmiendo sin devolver EINTR.
#define SIG_STOP_CONT_MASK                                                     \
  ((1ULL << SIGSTOP) | (1ULL << SIGTSTP) | (1ULL << SIGTTIN) |                 \
   (1ULL << SIGTTOU) | (1ULL << SIGCONT))

void wait_queue_init(wait_queue_t *wq) {
  spin_init(&wq->lock);
  wq->head = NULL;
  wq->nr_waiting = 0;
}

// --- helpers, llamar con wq->lock cogido ---
// La entrada de cada tarea vive dentro de task_t. Así bloquear una tarea
// nunca depende de una asignación dinámica que pueda fallar justo antes
// de publicar TASK_BLOCKED.
void wait_queue_add_locked(wait_queue_t *wq, task_t *t) {
  wait_queue_entry_t *e = &t->wait_entry;

  e->task = t;
  __atomic_fetch_add(&t->wait_seq, 1, __ATOMIC_ACQ_REL);
  e->next = wq->head;
  wq->head = e;
  wq->nr_waiting++;
  t->waiting_on = wq;
  task_get(t);
}

void wait_queue_remove_locked(wait_queue_t *wq, task_t *t) {
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
      wait_queue_add_locked(wq, self);
      // [C4] Publicación atómica de (state, wake_deadline) bajo
      // sched_lock, para que sched_wake_expired (que lee ambos campos
      // bajo el mismo lock) nunca vea una tarea BLOCKED con deadline
      // aún sin escribir, ni al revés.
      sched_set_blocked_deadline(self, deadline);

      // [FIX signal] Solo EINTR si la señal es realmente entregable
      // (no bloqueada). Antes mirábamos pending_signals != 0, lo que
      // incluía señales bloqueadas: cualquier proceso con SIGCHLD
      // pendiente y bloqueado salía del wait con EINTR espurio. Ash
      // bloquea SIGCHLD en secciones críticas, así que esto ocurría
      // en la práctica constantemente.
      if (interruptible && self->proc) {
        uint64_t pend =
            __atomic_load_n(&self->proc->pending_signals, __ATOMIC_ACQUIRE);
        uint64_t blk = self->proc->blocked_signals;
        if ((pend & ~blk) != 0) {
          self->wake_reason = -EINTR;
          wait_queue_remove_locked(wq, self);
          sched_make_ready(self);
        }
      }
    }

    spin_unlock_irqrestore(&wq->lock, flags);

    sched_yield();

    // [FIX timeout] Si despertamos por deadline, hay que limpiarlo
    // (sched_wake_expired ya lo hace, pero por si acaso).
    __atomic_store_n(&self->wake_deadline, 0, __ATOMIC_RELEASE);

    if (cond && cond(arg)) {
      if (self->waiting_on == wq) {
        flags = spin_lock_irqsave(&wq->lock);
        wait_queue_remove_locked(wq, self);
        spin_unlock_irqrestore(&wq->lock, flags);
      }
      if (timeout_ticks == 0)
        return 1;
      uint64_t now = sched_get_ticks();
      long remaining = (deadline > now) ? (long)(deadline - now) : 1;
      return remaining;
    }

    if (interruptible && self->wake_reason == -EINTR) {
      // [FIX signal] Re-verificar que sigue habiendo una señal
      // entregable. Si fue un wake_up_interruptible_all() por una señal
      // que ya está bloqueada o ya consumida, reanudamos el wait en
      // vez de devolver EINTR espurio.
      uint64_t pend = self->proc ? __atomic_load_n(&self->proc->pending_signals,
                                                   __ATOMIC_ACQUIRE)
                                 : 0;
      uint64_t blk = self->proc ? self->proc->blocked_signals : 0;
      uint64_t deliverable = pend & ~blk;

      if (deliverable == 0) {
        self->wake_reason = 0;
        // volver al top del loop: re-add a la wq y seguir durmiendo
      } else if ((deliverable & ~SIG_STOP_CONT_MASK) == 0 && self->proc) {
        // [FIX JOB] Solo señales de parada o SIGCONT pendientes.
        //
        // En Linux, nanosleep/pause/read NO devuelven EINTR por
        // SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU ni por SIGCONT no-op. La
        // parada se procesa DENTRO del wait (equivalente a
        // do_signal_stop() en kernel/signal.c de Linux), y el wait
        // se reanuda al llegar SIGCONT.
        uint64_t stops = deliverable & ~(1ULL << SIGCONT);
        uint64_t cont_only = deliverable & (1ULL << SIGCONT);

        if (stops != 0) {
          // Hay al menos una señal de parada. Consumirla del pending
          // y parar el proceso actual. ctzll devuelve el bit más bajo,
          // que es lo que Linux reporta como stop_signal (una sola).
          __atomic_fetch_and(&self->proc->pending_signals, ~stops,
                             __ATOMIC_ACQ_REL);
          int stop_sig = __builtin_ctzll(stops);

          // Sacar la tarea de la wq para que no aparezca "durmiendo"
          // en la cola mientras está STOPPED. Al reanudar por SIGCONT
          // volverá a añadirse en la siguiente iteración.
          if (self->waiting_on == wq) {
            flags = spin_lock_irqsave(&wq->lock);
            wait_queue_remove_locked(wq, self);
            spin_unlock_irqrestore(&wq->lock, flags);
          }
          self->wake_reason = 0;

          process_stop_current(stop_sig); // retorna tras SIGCONT
          continue;
        }

        // Solo SIGCONT pendiente: era un no-op (el proceso no estaba
        // stopped). Consumirlo y reanudar el wait sin más.
        __atomic_fetch_and(&self->proc->pending_signals, ~cont_only,
                           __ATOMIC_ACQ_REL);
        self->wake_reason = 0;
        continue;
      } else {
        // Hay una señal no-parada, no-SIGCONT pendiente: devolver
        // EINTR como antes.
        if (self->waiting_on == wq) {
          flags = spin_lock_irqsave(&wq->lock);
          wait_queue_remove_locked(wq, self);
          spin_unlock_irqrestore(&wq->lock, flags);
        }
        return -EINTR;
      }
    }

    if (timeout_ticks > 0) {
      uint64_t now = sched_get_ticks();
      if (now >= deadline) {
        if (self->waiting_on == wq) {
          flags = spin_lock_irqsave(&wq->lock);
          wait_queue_remove_locked(wq, self);
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
void wait_queue_wake_timeout_task(wait_queue_t *wq, task_t *task,
                                  uint64_t wait_seq) {
  if (!wq || !task)
    return;
  int removed = 0;
  unsigned long flags = spin_lock_irqsave(&wq->lock);
  if (task->waiting_on == wq &&
      __atomic_load_n(&task->wait_seq, __ATOMIC_ACQUIRE) == wait_seq &&
      task->state == TASK_BLOCKED) {
    wait_queue_entry_t **pp = &wq->head;
    while (*pp) {
      if ((*pp)->task == task) {
        wait_queue_entry_t *victim = *pp;
        *pp = victim->next;
        wq->nr_waiting--;
        task->waiting_on = NULL;
        victim->task = NULL;
        victim->next = NULL;
        removed = 1;
        break;
      }
      pp = &(*pp)->next;
    }
  }
  spin_unlock_irqrestore(&wq->lock, flags);
  if (removed) {
    sched_make_ready(task);
    task_put(task);
  }
}

void wait_queue_wake_task(task_t *task) {
  if (!task)
    return;

  wait_queue_t *wq = task->waiting_on;
  if (!wq)
    return;

  unsigned long flags = spin_lock_irqsave(&wq->lock);
  if (task->waiting_on == wq && task->state == TASK_BLOCKED) {
    wait_queue_entry_t **pp = &wq->head;
    while (*pp) {
      if ((*pp)->task == task) {
        wait_queue_entry_t *victim = *pp;
        *pp = victim->next;
        wq->nr_waiting--;
        task->waiting_on = NULL;
        task->wake_reason = 0;
        victim->task = NULL;
        victim->next = NULL;
        sched_make_ready(task);
        spin_unlock_irqrestore(&wq->lock, flags);
        task_put(task);
        return;
      }
      pp = &(*pp)->next;
    }
  }
  spin_unlock_irqrestore(&wq->lock, flags);
}

void wait_queue_interrupt_task(task_t *task) {
  if (!task)
    return;

  wait_queue_t *wq = task->waiting_on;
  if (!wq)
    return;

  unsigned long flags = spin_lock_irqsave(&wq->lock);
  if (task->waiting_on == wq && task->state == TASK_BLOCKED) {
    wait_queue_entry_t **pp = &wq->head;
    while (*pp) {
      if ((*pp)->task == task) {
        wait_queue_entry_t *victim = *pp;
        *pp = victim->next;
        wq->nr_waiting--;
        task->waiting_on = NULL;
        task->wake_reason = -EINTR;
        victim->task = NULL;
        victim->next = NULL;
        sched_make_ready(task);
        spin_unlock_irqrestore(&wq->lock, flags);
        task_put(task);
        return;
      }
      pp = &(*pp)->next;
    }
  }
  spin_unlock_irqrestore(&wq->lock, flags);
}
