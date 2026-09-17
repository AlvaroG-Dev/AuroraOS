// kernel/input.c
#include "input.h"
#include "klog.h"
#include "sched.h"
#include "spinlock.h"
#include "string.h"
#include "wait.h"

static input_event_t buf[INPUT_BUF_SIZE];
static volatile uint16_t head = 0;
static volatile uint16_t tail = 0;
static volatile uint16_t count = 0;
static spinlock_t input_lock;
static wait_queue_t input_wq;

void input_init(void) {
  spin_init(&input_lock);
  wait_queue_init(&input_wq);
  head = tail = count = 0;
  LOG_INFO("[INPUT] Subsistema de input inicializado");
}

void input_push(const input_event_t *ev) {
  if (!ev)
    return;

  unsigned long flags = spin_lock_irqsave(&input_lock);

  if (count >= INPUT_BUF_SIZE) {
    // Buffer lleno: descartar el más viejo
    tail = (tail + 1) % INPUT_BUF_SIZE;
    count--;
  }
  buf[head] = *ev;
  head = (head + 1) % INPUT_BUF_SIZE;
  count++;

  spin_unlock_irqrestore(&input_lock, flags);

  // Despertar a todos los consumidores de input. Los que no tengan
  // nada que hacer volverán a dormir en su próximo wait_event.
  wake_up_all(&input_wq);

  // Despertar también al compositor. Vive en su propia wait queue
  // (compositor_wq) porque también espera por daño y clock tick.
  // Sin esto, un evento de input se quedaría en cola hasta el
  // próximo redibujado espontáneo.
  extern void compositor_notify_event(void);
  compositor_notify_event();
}

int input_pop(input_event_t *out) {
  if (!out)
    return 0;

  unsigned long flags = spin_lock_irqsave(&input_lock);
  if (count == 0) {
    spin_unlock_irqrestore(&input_lock, flags);
    return 0;
  }
  *out = buf[tail];
  tail = (tail + 1) % INPUT_BUF_SIZE;
  count--;
  spin_unlock_irqrestore(&input_lock, flags);
  return 1;
}

// Condición para wait_event_interruptible. Puede leer count sin lock:
// es benigno (sólo puede dar falso positivo, nunca falso negativo que
// haga perder un wakeup), porque input_push siempre hace wake_up_all
// después de incrementar.
static bool input_has_event(void *arg) {
  (void)arg;
  return count > 0;
}

int input_wait_event(input_event_t *out) {
  if (!out)
    return -1;

  while (1) {
    int rc = wait_event_interruptible(&input_wq, input_has_event, NULL);
    if (rc < 0)
      return rc;

    if (input_pop(out))
      return 0;

    // La condición decía que había evento, pero otro consumidor
    // se lo llevó entre el wake y el pop. Volver a dormir.
  }
}

size_t input_pending(void) { return count; }