// kernel/futex.c
#include "futex.h"
#include "heap.h"
#include "klog.h"
#include "process.h"
#include "sched.h"
#include "spinlock.h"
#include "uaccess.h"
#include "wait.h"
#include <stddef.h>

#define FUTEX_HASH_BITS 8
#define FUTEX_HASH_SIZE (1u << FUTEX_HASH_BITS)

// Una entrada por (uaddr, pml4). Los hilos de un mismo team comparten
// pml4, así que se ven entre ellos. Procesos distintos con la misma
// dirección virtual van a buckets distintos (aunque puedan colisionar
// en el hash — se filtran por comparación).
typedef struct futex_entry {
  uint64_t uaddr;
  uint64_t pml4;
  wait_queue_t wq;
  struct futex_entry *next;
} futex_entry_t;

static futex_entry_t *futex_buckets[FUTEX_HASH_SIZE];
static spinlock_t futex_table_lock;

static inline unsigned futex_hash(uint64_t uaddr, uint64_t pml4) {
  uint64_t h = uaddr * 0x9E3779B97F4A7C15ULL;
  h ^= pml4 * 0xC2B2AE3D27D4EB4FULL;
  return (unsigned)(h >> (64 - FUTEX_HASH_BITS));
}

static futex_entry_t *futex_lookup(uint64_t uaddr, uint64_t pml4) {
  unsigned h = futex_hash(uaddr, pml4);
  unsigned long flags = spin_lock_irqsave(&futex_table_lock);
  futex_entry_t *e = futex_buckets[h];
  while (e) {
    if (e->uaddr == uaddr && e->pml4 == pml4) {
      spin_unlock_irqrestore(&futex_table_lock, flags);
      return e;
    }
    e = e->next;
  }
  spin_unlock_irqrestore(&futex_table_lock, flags);
  return NULL;
}

static futex_entry_t *futex_get_or_create(uint64_t uaddr, uint64_t pml4) {
  unsigned h = futex_hash(uaddr, pml4);
  unsigned long flags = spin_lock_irqsave(&futex_table_lock);

  futex_entry_t *e = futex_buckets[h];
  while (e) {
    if (e->uaddr == uaddr && e->pml4 == pml4) {
      spin_unlock_irqrestore(&futex_table_lock, flags);
      return e;
    }
    e = e->next;
  }

  // Las entradas se alocan con kzalloc y no se liberan nunca. Están
  // acotadas por el número de direcciones futex usadas en el sistema
  // (unas decenas en la práctica). Si algún día es un problema, se
  // pueden liberar cuando wq quede vacía con un refcount.
  e = (futex_entry_t *)kzalloc(sizeof(*e));
  if (!e) {
    spin_unlock_irqrestore(&futex_table_lock, flags);
    return NULL;
  }
  e->uaddr = uaddr;
  e->pml4 = pml4;
  wait_queue_init(&e->wq);
  e->next = futex_buckets[h];
  futex_buckets[h] = e;

  spin_unlock_irqrestore(&futex_table_lock, flags);
  return e;
}

void futex_init(void) {
  for (unsigned i = 0; i < FUTEX_HASH_SIZE; i++)
    futex_buckets[i] = NULL;
  spin_init(&futex_table_lock);
}

// ---------------------------------------------------------------------------
// [A.1.3] futex_do_wait_impl: núcleo compartido de FUTEX_WAIT y
// FUTEX_WAIT_BITSET.
//
//   timeout_ns < 0  → esperar indefinidamente.
//   timeout_ns == 0 → no bloquear, retornar -ETIMEDOUT.
//   timeout_ns > 0  → dormir hasta el deadline.
//
//   absolute == 0 → timeout relativo (FUTEX_WAIT).
//   absolute == 1 → timeout absoluto contra CLOCK_MONOTONIC
//                   (FUTEX_WAIT_BITSET). Lo convertimos ns→ticks
//                   (KERNEL_HZ = 1000) y comparamos con el reloj
//                   monotónico del kernel.
//
// El bitset del waiter lo ignoramos. Todos los bitsets que usa glibc
// son FUTEX_BITSET_MATCH_ANY, así que no hay wakeups cruzados en la
// práctica. Si en el futuro aparece un caso real, hay que añadir
// bitset al futex_entry_t y filtrar en futex_do_wake.
// ---------------------------------------------------------------------------
static int64_t futex_do_wait_impl(uint64_t uaddr, uint32_t val,
                                  int64_t timeout_ns, int absolute) {
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  uint32_t cur;
  if (get_user_u32(&cur, (const uint32_t *)uaddr) < 0)
    return -EFAULT;
  if (cur != val)
    return -EAGAIN;

  // Timeout inmediato: no dormir, solo chequear.
  if (timeout_ns == 0)
    return -ETIMEDOUT;

  futex_entry_t *entry = futex_get_or_create(uaddr, proc->pml4_phys);
  if (!entry)
    return -ENOMEM;

  task_t *self = sched_current();
  if (!self)
    return -EINVAL;

  uint64_t deadline = 0;
  if (timeout_ns > 0) {
    uint64_t now = sched_get_ticks();
    if (absolute) {
      // timeout_ns es tiempo absoluto (ns) desde CLOCK_MONOTONIC.
      // sched_get_ticks() está en ms, así que ns/1e6 = ticks.
      uint64_t target_ticks = (uint64_t)timeout_ns / 1000000ULL;
      if (target_ticks <= now)
        return -ETIMEDOUT;
      deadline = target_ticks;
    } else {
      uint64_t ticks = (uint64_t)timeout_ns / 1000000ULL;
      if (ticks == 0)
        ticks = 1;
      deadline = now + ticks;
    }
  }

  // Registro atómico: bajo wq->lock re-checkeamos el valor y nos
  // añadimos. Cualquier FUTEX_WAKE toma el mismo lock, así que no
  // puede haber "missed wakeup".
  unsigned long flags = spin_lock_irqsave(&entry->wq.lock);

  if (get_user_u32(&cur, (const uint32_t *)uaddr) < 0) {
    spin_unlock_irqrestore(&entry->wq.lock, flags);
    return -EFAULT;
  }
  if (cur != val) {
    spin_unlock_irqrestore(&entry->wq.lock, flags);
    return -EAGAIN;
  }

  self->wake_reason = 0;
  wait_queue_add_locked(&entry->wq, self);
  sched_set_blocked_deadline(self, deadline);

  spin_unlock_irqrestore(&entry->wq.lock, flags);

  sched_yield();

  if (self->wake_reason == -EINTR)
    return -EINTR;
  if (deadline > 0 && sched_get_ticks() >= deadline)
    return -ETIMEDOUT;
  return 0;
}

// Wrapper para FUTEX_WAIT (timeout relativo).
static int64_t futex_do_wait(uint64_t uaddr, uint32_t val, int64_t timeout_ns) {
  return futex_do_wait_impl(uaddr, val, timeout_ns, /*absolute=*/0);
}

static int futex_do_wake(uint64_t uaddr, uint32_t nr_wake, uint64_t pml4) {
  futex_entry_t *entry = futex_lookup(uaddr, pml4);
  if (!entry)
    return 0;
  int woken = 0;
  unsigned long flags = spin_lock_irqsave(&entry->wq.lock);
  while (woken < (int)nr_wake && entry->wq.head) {
    wake_up_one_locked(&entry->wq);
    woken++;
  }
  spin_unlock_irqrestore(&entry->wq.lock, flags);
  return woken;
}

void futex_wake_user(uint64_t uaddr, int nr_wake, uint64_t pml4) {
  if (uaddr == 0)
    return;
  futex_do_wake(uaddr, (uint32_t)nr_wake, pml4);
}

int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val,
                  uint64_t timeout_uptr, uint64_t uaddr2, uint64_t val3) {
  (void)uaddr2;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  uint64_t pml4 = proc->pml4_phys;

  // Los bits altos (PRIVATE / CLOCK_REALTIME) no cambian la operación
  // en nuestro caso. PRIVATE = "esta dirección solo la usa mi mm", que
  // es lo que asumimos siempre.
  uint32_t cmd = (uint32_t)op & 0x7F;

  // [A.1.3] Helper local: parsea el struct timespec de un puntero de
  // usuario y devuelve ns (o -1 si timeout_uptr == NULL, o error
  // negativo). Reusado por WAIT y WAIT_BITSET.
  int64_t timeout_ns = -1;
  if (timeout_uptr) {
    struct {
      int64_t sec;
      int64_t nsec;
    } ts;
    if (!access_ok((void *)timeout_uptr, sizeof(ts)))
      return -EFAULT;
    if (copy_from_user(&ts, (void *)timeout_uptr, sizeof(ts)) < 0)
      return -EFAULT;
    if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000LL)
      return -EINVAL;
    timeout_ns = ts.sec * 1000000000LL + ts.nsec;
  }

  switch (cmd) {
  case FUTEX_WAIT:
    return futex_do_wait(uaddr, (uint32_t)val, timeout_ns);

  case FUTEX_WAKE:
    return futex_do_wake(uaddr, (uint32_t)val, pml4);

  // [A.1.3] FUTEX_WAIT_BITSET: FUTEX_WAIT con timeout ABSOLUTO contra
  // CLOCK_MONOTONIC. val3 = bitset. Ignoramos el bitset de match
  // (ver comentario en futex_do_wait_impl).
  //
  // NO rechazamos FUTEX_CLOCK_REALTIME (bit 8). glibc lo pasa con
  // pthread_cond_* cuando el cond var se configuró con CLOCK_REALTIME,
  // y también lo pasa incondicionalmente en algunas rutas internas del
  // loader. Si devolvemos -ENOSYS, glibc NO cae a futex_wait: en
  // __futex_abstimed_wait_common64 con abstime != NULL convierte el
  // -ENOSYS en -ENOTSUP, y pthread_cond_wait hace __libc_fatal →
  // abort() → SIGABRT. Eso es exactamente el "The futex facility
  // returned an unexpected error code" que veíamos.
  //
  // Limitación: si el timeout es no-NULL y era CLOCK_REALTIME, lo
  // interpretamos como si fuera CLOCK_MONOTONIC → expiraremos tarde
  // (años, porque REALTIME ≈ 1.7e9 s vs MONOTONIC ≈ 100 s). Para
  // abstime == NULL (el caso común en Python/C) el resultado es
  // idéntico. Cuando aparezca un caller real que dependa de
  // CLOCK_REALTIME con timeout, implementamos la conversión
  // REALTIME→MONOTONIC usando rtc_get_epoch() + sched_get_ticks().
  case FUTEX_WAIT_BITSET: {
    if ((uint32_t)val3 == 0)
      return -EINVAL;
    return futex_do_wait_impl(uaddr, (uint32_t)val, timeout_ns,
                              /*absolute=*/1);
  }

  case FUTEX_WAKE_BITSET: {
    if ((uint32_t)val3 == 0)
      return -EINVAL;
    return futex_do_wake(uaddr, (uint32_t)val, pml4);
  }

  case FUTEX_REQUEUE:
  case FUTEX_CMP_REQUEUE:
  case FUTEX_WAKE_OP:
    LOG_WARN("[FUTEX] op %u no soportada", cmd);
    return -ENOSYS;

  default:
    return -ENOSYS;
  }
}

void futex_cleanup_pml4(uint64_t pml4) {
  if (!pml4)
    return;
  unsigned long flags = spin_lock_irqsave(&futex_table_lock);
  for (unsigned i = 0; i < FUTEX_HASH_SIZE; i++) {
    futex_entry_t **pp = &futex_buckets[i];
    while (*pp) {
      futex_entry_t *e = *pp;
      if (e->pml4 != pml4) {
        pp = &e->next;
        continue;
      }
      *pp = e->next;
      kfree(e);
    }
  }
  spin_unlock_irqrestore(&futex_table_lock, flags);
}