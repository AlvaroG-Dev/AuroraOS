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
#include "signal.h"
#include "idt.h"

#ifndef EDEADLK
#define EDEADLK 35
#endif

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

// FUTEX_WAKE_OP: *uaddr2 = op(*uaddr2, oparg) y despierta a `nr_wake` en uaddr y,
// si cmp(valor_antiguo, cmparg) se cumple, a `nr_wake2` en uaddr2. `encoded` =
// op(4 bits) | cmp(4) | oparg(12) | cmparg(12) (include/uapi/linux/futex.h).
// Las modificaciones del kernel se serializan con un cerrojo; no son atomicas
// frente a atomicos de usuario en otra CPU sobre la misma palabra, pero glibc
// solo lo usa (__lll_futex_wake_unlock) con la palabra protegida por su lock.
static spinlock_t futex_op_lock;
static int64_t futex_wake_op(uint64_t uaddr, uint32_t nr_wake, uint64_t uaddr2,
                             uint32_t nr_wake2, uint32_t encoded,
                             uint64_t pml4) {
  int op = (int)((encoded >> 28) & 0xF);
  int cmp = (int)((encoded >> 24) & 0xF);
  int32_t oparg = (int32_t)(encoded << 8) >> 20; // 12 bits con signo
  int32_t cmparg = (int32_t)(encoded << 20) >> 20;
  if (op & 8) { // FUTEX_OP_OPARG_SHIFT
    oparg = 1 << (oparg & 31);
    op &= 7;
  }
  uint32_t old;
  unsigned long fl = spin_lock_irqsave(&futex_op_lock);
  if (get_user_u32(&old, (const uint32_t *)uaddr2) < 0) {
    spin_unlock_irqrestore(&futex_op_lock, fl);
    return -EFAULT;
  }
  uint32_t nv;
  switch (op) {
  case 0: nv = (uint32_t)oparg; break;           // SET
  case 1: nv = old + (uint32_t)oparg; break;     // ADD
  case 2: nv = old | (uint32_t)oparg; break;     // OR
  case 3: nv = old & ~(uint32_t)oparg; break;    // ANDN
  case 4: nv = old ^ (uint32_t)oparg; break;     // XOR
  default:
    spin_unlock_irqrestore(&futex_op_lock, fl);
    return -ENOSYS;
  }
  if (put_user_u32((uint32_t *)uaddr2, nv) < 0) {
    spin_unlock_irqrestore(&futex_op_lock, fl);
    return -EFAULT;
  }
  spin_unlock_irqrestore(&futex_op_lock, fl);

  int32_t o = (int32_t)old;
  int hit;
  switch (cmp) {
  case 0: hit = o == cmparg; break; // EQ
  case 1: hit = o != cmparg; break; // NE
  case 2: hit = o < cmparg; break;  // LT
  case 3: hit = o <= cmparg; break; // LE
  case 4: hit = o > cmparg; break;  // GT
  case 5: hit = o >= cmparg; break; // GE
  default: return -ENOSYS;
  }
  int64_t woken = futex_do_wake(uaddr, nr_wake, pml4);
  if (hit)
    woken += futex_do_wake(uaddr2, nr_wake2, pml4);
  return woken;
}

// ---------------------------------------------------------------------------
// Futex PI (LOCK_PI / UNLOCK_PI / TRYLOCK_PI / LOCK_PI2).
//
// glibc los usa para los mutex PTHREAD_PRIO_INHERIT (Mesa, Wine...). La
// palabra vale TID del dueno | FUTEX_WAITERS | FUTEX_OWNER_DIED. Sin ellos
// (antes: -ENOSYS) el que soltaba un mutex con esperadores no despertaba a
// nadie y los demas hilos se quedaban esperando para siempre.
//
// Sin herencia de prioridad (el planificador no la tiene): LOCK_PI marca
// FUTEX_WAITERS y duerme en la cola del futex; UNLOCK_PI deja la palabra a 0
// y despierta a todos, que vuelven a competir (el que pierde vuelve a marcar
// FUTEX_WAITERS antes de dormir, asi nadie se queda sin despertar).
// ---------------------------------------------------------------------------
#define FUTEX_PI_WAITERS 0x80000000u
#define FUTEX_PI_OWNER_DIED 0x40000000u
#define FUTEX_PI_TID_MASK 0x3fffffffu

extern long raw_cmpxchg_user_u32(uint32_t *uaddr, uint32_t old, uint32_t nv,
                                 uint32_t *cur);
extern registers_t *syscall_current_regs(void);
extern int64_t kernel_realtime_ms(void);

// 0 = cambiado; 1 = el valor no era `old` (*cur lo trae); <0 error.
static int futex_cas_user(uint64_t uaddr, uint32_t old, uint32_t nv,
                          uint32_t *cur) {
  if (!access_ok((void *)uaddr, 4) || (uaddr & 3))
    return -EFAULT;
  uint32_t c = 0;
  if (raw_cmpxchg_user_u32((uint32_t *)uaddr, old, nv, &c) != 0)
    return -EFAULT;
  *cur = c;
  return c == old ? 0 : 1;
}

static int64_t futex_lock_pi(uint64_t uaddr, int64_t abs_ns, int realtime,
                             int trylock, uint64_t pml4) {
  (void)pml4;
  task_t *self = sched_current();
  if (!self)
    return -EINVAL;
  uint32_t tid = task_tid(self) & FUTEX_PI_TID_MASK;
  for (;;) {
    uint32_t v;
    if (get_user_u32(&v, (const uint32_t *)uaddr) < 0)
      return -EFAULT;
    if ((v & FUTEX_PI_TID_MASK) == 0) {
      uint32_t nv = tid | (v & (FUTEX_PI_WAITERS | FUTEX_PI_OWNER_DIED));
      uint32_t c;
      int r = futex_cas_user(uaddr, v, nv, &c);
      if (r < 0)
        return r;
      if (r == 0)
        return 0;
      continue;
    }
    if ((v & FUTEX_PI_TID_MASK) == tid)
      return -EDEADLK;
    if (trylock)
      return -EAGAIN;
    if (!(v & FUTEX_PI_WAITERS)) {
      uint32_t c;
      int r = futex_cas_user(uaddr, v, v | FUTEX_PI_WAITERS, &c);
      if (r < 0)
        return r;
      if (r == 1)
        continue;
      v |= FUTEX_PI_WAITERS;
    }
    // Espera hasta que la palabra cambie (UNLOCK_PI despierta).
    int64_t rel = -1;
    if (abs_ns >= 0) {
      int64_t now_ns = realtime ? kernel_realtime_ms() * 1000000LL
                                : (int64_t)sched_get_ticks() * 1000000LL;
      rel = abs_ns - now_ns;
      if (rel <= 0)
        return -ETIMEDOUT;
    }
    int64_t w = futex_do_wait_impl(uaddr, v, rel, /*absolute=*/0);
    if (w == -ETIMEDOUT)
      return -ETIMEDOUT;
    if (w == -EINTR) {
      // Linux reinicia LOCK_PI tras el manejador (nunca devuelve EINTR:
      // glibc lo trataria como error fatal). Rebobinamos la syscall: al
      // volver del manejador se ejecuta otra vez con los mismos argumentos.
      registers_t *regs = syscall_current_regs();
      if (regs) {
        regs->rip -= 2;
        return 202; /* SYS_futex: rax para repetir la syscall */
      }
      return -EINTR;
    }
    if (w == -EFAULT)
      return -EFAULT;
    // 0 o -EAGAIN (la palabra ya habia cambiado): reintentar.
  }
}

static int64_t futex_unlock_pi(uint64_t uaddr, uint64_t pml4) {
  task_t *self = sched_current();
  if (!self)
    return -EINVAL;
  uint32_t tid = task_tid(self) & FUTEX_PI_TID_MASK;
  for (;;) {
    uint32_t v;
    if (get_user_u32(&v, (const uint32_t *)uaddr) < 0)
      return -EFAULT;
    if ((v & FUTEX_PI_TID_MASK) != tid)
      return -EPERM;
    uint32_t c;
    int r = futex_cas_user(uaddr, v, 0, &c);
    if (r < 0)
      return r;
    if (r == 0)
      break;
  }
  futex_do_wake(uaddr, 0x7fffffff, pml4);
  return 0;
}

int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val,
                  uint64_t timeout_uptr, uint64_t uaddr2, uint64_t val3) {
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
  // Los argumentos no usados por otras operaciones FUTEX pueden contener
  // cualquier valor y no deben interpretarse como punteros.
  if ((cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) && timeout_uptr) {
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

  // val2 (nr_requeue / nr_wake2) viaja en el argumento de timeout.
  case FUTEX_WAKE_OP:
    return futex_wake_op(uaddr, (uint32_t)val, uaddr2, (uint32_t)timeout_uptr,
                         (uint32_t)val3, pml4);

  // REQUEUE: despierta `val` y *reencola* hasta `val2` en uaddr2. Sin colas
  // transferibles entre futex, se despiertan tambien los que habria que
  // reencolar: es correcto (los esperadores revalidan la palabra y vuelven a
  // dormir) y solo cuesta una estampida.
  case FUTEX_CMP_REQUEUE: {
    uint32_t cur;
    if (get_user_u32(&cur, (const uint32_t *)uaddr) < 0)
      return -EFAULT;
    if (cur != (uint32_t)val3)
      return -EAGAIN;
  }
  // fallthrough
  case FUTEX_REQUEUE: {
    uint64_t total = (uint64_t)(uint32_t)val + (uint64_t)(uint32_t)timeout_uptr;
    if (total > 0x7fffffff)
      total = 0x7fffffff;
    return futex_do_wake(uaddr, (uint32_t)total, pml4);
  }

  case FUTEX_LOCK_PI:
  case FUTEX_LOCK_PI2:
  case FUTEX_TRYLOCK_PI: {
    // Timeout absoluto: LOCK_PI siempre CLOCK_REALTIME; LOCK_PI2 MONOTONIC
    // salvo FUTEX_CLOCK_REALTIME.
    int64_t abs_ns = -1;
    if (cmd != FUTEX_TRYLOCK_PI && timeout_uptr) {
      struct {
        int64_t sec;
        int64_t nsec;
      } ts;
      if (copy_from_user(&ts, (void *)timeout_uptr, sizeof(ts)) < 0)
        return -EFAULT;
      if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000LL)
        return -EINVAL;
      abs_ns = ts.sec * 1000000000LL + ts.nsec;
    }
    int realtime = cmd == FUTEX_LOCK_PI || (op & FUTEX_CLOCK_REALTIME);
    return futex_lock_pi(uaddr, abs_ns, realtime, cmd == FUTEX_TRYLOCK_PI,
                         pml4);
  }

  case FUTEX_UNLOCK_PI:
    return futex_unlock_pi(uaddr, pml4);

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