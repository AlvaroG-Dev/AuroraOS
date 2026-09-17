// kernel/wait.h
#ifndef KERNEL_WAIT_H
#define KERNEL_WAIT_H

#include "spinlock.h"
#include <stdbool.h>
#include <stddef.h>

// Forward declaration: NO incluir sched.h aquí, porque sched.h incluye
// wait.h (para el campo mailbox.wq) y se produciría un ciclo.
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

int wait_event_interruptible(wait_queue_t *wq, bool (*cond)(void *), void *arg);
void wait_event(wait_queue_t *wq, bool (*cond)(void *), void *arg);

void wake_up_all(wait_queue_t *wq);
void wake_up_one(wait_queue_t *wq);
void wake_up_interruptible_all(wait_queue_t *wq);

#endif