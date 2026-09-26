// kernel/wait.h
#ifndef KERNEL_WAIT_H
#define KERNEL_WAIT_H

#include "spinlock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct task;

typedef struct wait_queue_entry {
  struct task *task;
  struct wait_queue_entry *next;
} wait_queue_entry_t;

typedef struct wait_queue {
  spinlock_t lock;
  wait_queue_entry_t *head;
  int nr_waiting;
} wait_queue_t;

#define WAIT_QUEUE_INIT(name)                                                  \
  {.lock = {.locked = 0}, .head = NULL, .nr_waiting = 0}

void wait_queue_init(wait_queue_t *wq);

// [FIX] Duerme hasta que cond(arg) sea true o hasta que pasen timeout_ticks.
// Devuelve:
//   >0  si despertó porque cond() se cumplió (ticks restantes, >=1)
//    0   si timeout expiró sin cumplirse la condición
//   <0  si fue interrumpida (-EINTR)
//
// timeout_ticks == 0 significa "esperar indefinidamente".
long wait_event_interruptible_timeout(wait_queue_t *wq, bool (*cond)(void *),
                                      void *arg, uint64_t timeout_ticks);

int wait_event_interruptible(wait_queue_t *wq, bool (*cond)(void *), void *arg);
void wait_event(wait_queue_t *wq, bool (*cond)(void *), void *arg);

void wake_up_all(wait_queue_t *wq);
void wake_up_all_locked(wait_queue_t *wq);
void wake_up_one(wait_queue_t *wq);
void wake_up_interruptible_all(wait_queue_t *wq);

// [NUEVO] Versión de wake_up_one que asume wq->lock ya cogido.
void wake_up_one_locked(wait_queue_t *wq);


void wait_queue_wake_timeout_task(wait_queue_t *wq, struct task *task,
                                  uint64_t wait_seq);
void wait_queue_interrupt_task(struct task *task);
#endif
