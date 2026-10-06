#ifndef MUTEX_H
#define MUTEX_H

#include "spinlock.h"
#include "wait.h"

typedef struct mutex {
  wait_queue_t waiters;
  volatile int locked;
} mutex_t;

static inline bool mutex_available(void *arg) {
  mutex_t *m = (mutex_t *)arg;
  return __atomic_load_n(&m->locked, __ATOMIC_ACQUIRE) == 0;
}

static inline void mutex_init(mutex_t *m) {
  wait_queue_init(&m->waiters);
  __atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE);
}

static inline void mutex_lock(mutex_t *m) {
  for (;;) {
    int expected = 0;
    if (__atomic_compare_exchange_n(&m->locked, &expected, 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      return;
    wait_event(&m->waiters, mutex_available, m);
  }
}

static inline void mutex_unlock(mutex_t *m) {
  unsigned long flags = spin_lock_irqsave(&m->waiters.lock);
  __atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE);
  wake_up_one_locked(&m->waiters);
  spin_unlock_irqrestore(&m->waiters.lock, flags);
}

#endif
