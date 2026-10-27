// kernel/ptimer.c
//
// Temporizadores de proceso, resolucion de 1 tick (1 ms):
//   - alarm(2), setitimer(2)/getitimer(2) con ITIMER_REAL (SIGALRM),
//     ITIMER_VIRTUAL (SIGVTALRM) e ITIMER_PROF (SIGPROF).
//   - timer_create/settime/gettime/getoverrun/delete (POSIX) sobre
//     CLOCK_REALTIME, CLOCK_MONOTONIC y CLOCK_BOOTTIME, con SIGEV_SIGNAL,
//     SIGEV_NONE y SIGEV_THREAD_ID (glibc y musl montan SIGEV_THREAD sobre
//     este ultimo con un hilo auxiliar).
//
// ITIMER_REAL y los POSIX viven en una lista global por pid; un kthread
// unico duerme hasta el vencimiento mas cercano y manda la senal con
// process_kill() (contexto de tarea, nunca desde la IRQ), como timerfd.c.
// ITIMER_VIRTUAL/PROF cuentan los ticks de CPU del proceso en el propio tick
// (Aurora no separa tiempo de usuario y de sistema: los dos cuentan todo).
//
// Diferencias con Linux:
//   - Las senales pendientes son un bitmap (por proceso y por hilo): dos
//     temporizadores con la misma senal se funden si llegan antes de
//     entregarse.
//   - Mientras la senal de un temporizador POSIX sigue pendiente, los
//     vencimientos siguientes cuentan como overrun (como Linux).

#include "ptimer.h"
#include "heap.h"
#include "klog.h"
#include "kthread.h"
#include "process.h"
#include "sched.h"
#include "signal.h"
#include "spinlock.h"
#include "string.h"
#include "timerfd.h"
#include "uaccess.h"
#include "wait.h"
#include <stddef.h>
#include <stdint.h>

#define ITIMER_REAL 0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF 2

#define SIGEV_SIGNAL 0
#define SIGEV_NONE 1
#define SIGEV_THREAD 2
#define SIGEV_THREAD_ID 4

#define TIMER_ABSTIME 1
#define PTIMER_ID_REAL (-1) // la entrada de ITIMER_REAL de un proceso
#define PTIMER_MAX_PER_PROC 1024
#define FIRE_BATCH 32

struct k_timeval {
  int64_t sec;
  int64_t usec;
};
struct k_itimerval {
  struct k_timeval interval;
  struct k_timeval value;
};
_Static_assert(sizeof(struct k_itimerval) == 32, "itimerval x86_64");

struct k_ts {
  int64_t sec;
  int64_t nsec;
};
struct k_itimerspec {
  struct k_ts interval;
  struct k_ts value;
};
_Static_assert(sizeof(struct k_itimerspec) == 32, "itimerspec x86_64");

// struct sigevent de Linux x86-64 (64 bytes).
struct k_sigevent {
  uint64_t value;
  int32_t signo;
  int32_t notify;
  int32_t tid; // _sigev_un._tid
  int32_t pad[11];
};
_Static_assert(sizeof(struct k_sigevent) == 64, "sigevent x86_64");

typedef struct ptimer {
  uint32_t pid;
  int id; // PTIMER_ID_REAL o id POSIX (>= 0)
  int clockid;
  int signo;  // 0 con SIGEV_NONE
  int notify; // SIGEV_*
  uint32_t tid; // SIGEV_THREAD_ID: hilo destino
  uint64_t value;
  bool armed;
  uint64_t expiry;   // tick absoluto
  uint64_t interval; // ticks, 0 = una vez
  bool queued;       // su senal esta pendiente y aun no se ha entregado
  int overrun;       // vencimientos perdidos desde que se encolo la senal
  int last_overrun;  // los de la ultima entrega (timer_getoverrun)
  struct ptimer *next;
} ptimer_t;

static spinlock_t g_lock;
static ptimer_t *g_list;
static wait_queue_t g_kick_wq;
static uint32_t g_kick_gen;
static int g_started;

// ---------------------------------------------------------------------------
// Conversiones (redondeo hacia arriba al tick, como hace Linux al jiffy)
// ---------------------------------------------------------------------------
static uint64_t ts_to_ticks(const struct k_ts *ts) {
  if (ts->sec < 0 || ts->nsec < 0 || ts->nsec >= 1000000000LL)
    return (uint64_t)-1;
  return (uint64_t)ts->sec * 1000ULL + ((uint64_t)ts->nsec + 999999) / 1000000;
}

static void ticks_to_ts(uint64_t t, struct k_ts *ts) {
  ts->sec = (int64_t)(t / 1000);
  ts->nsec = (int64_t)(t % 1000) * 1000000;
}

static uint64_t tv_to_ticks(const struct k_timeval *tv) {
  if (tv->sec < 0 || tv->usec < 0 || tv->usec >= 1000000)
    return (uint64_t)-1;
  return (uint64_t)tv->sec * 1000ULL + ((uint64_t)tv->usec + 999) / 1000;
}

static void ticks_to_tv(uint64_t t, struct k_timeval *tv) {
  tv->sec = (int64_t)(t / 1000);
  tv->usec = (int64_t)(t % 1000) * 1000;
}

// ---------------------------------------------------------------------------
// Lista y kthread
// ---------------------------------------------------------------------------
typedef struct {
  uint32_t gen;
} kick_arg_t;

static bool kick_changed(void *a) {
  return __atomic_load_n(&g_kick_gen, __ATOMIC_ACQUIRE) !=
         ((kick_arg_t *)a)->gen;
}

static void kick(void) {
  __atomic_fetch_add(&g_kick_gen, 1, __ATOMIC_ACQ_REL);
  wake_up_all(&g_kick_wq);
}

// Con g_lock cogido.
static ptimer_t *find_locked(uint32_t pid, int id) {
  for (ptimer_t *t = g_list; t; t = t->next)
    if (t->pid == pid && t->id == id)
      return t;
  return NULL;
}

typedef struct {
  uint32_t pid;
  uint32_t tid; // != 0: senal dirigida a ese hilo
  int sig;
} fire_t;

// La senal anterior del temporizador sigue pendiente (en su hilo o en el
// proceso).
static bool still_pending(const ptimer_t *t) {
  if (t->notify == SIGEV_THREAD_ID)
    return signal_tid_is_pending(t->tid, t->signo);
  return process_signal_is_pending(t->pid, t->signo);
}

// Con g_lock cogido. Avanza `t` si ha vencido y apunta la senal a mandar.
static void expire_locked(ptimer_t *t, uint64_t now, fire_t *out, int *nout) {
  if (!t->armed || now < t->expiry)
    return;
  uint64_t n = 1;
  if (t->interval) {
    n = 1 + (now - t->expiry) / t->interval;
    t->expiry += n * t->interval;
  } else {
    t->armed = false;
  }
  if (t->notify == SIGEV_NONE || t->signo == 0)
    return;
  if (t->id != PTIMER_ID_REAL) {
    // Si la senal anterior de este temporizador sigue pendiente, no se
    // encola otra: se cuenta como overrun. Si ya no esta pendiente (la
    // consumio un camino que no pasa por ptimer_take_siginfo, p. ej. una
    // accion por defecto), se vuelve a encolar.
    if (t->queued && still_pending(t)) {
      uint64_t o = (uint64_t)t->overrun + n;
      t->overrun = o > 0x7fffffff ? 0x7fffffff : (int)o;
      return;
    }
    t->queued = true;
    t->overrun = n - 1 > 0x7fffffff ? 0x7fffffff : (int)(n - 1);
  }
  if (*nout < FIRE_BATCH) {
    out[*nout].pid = t->pid;
    out[*nout].tid = t->notify == SIGEV_THREAD_ID ? t->tid : 0;
    out[*nout].sig = t->signo;
    (*nout)++;
  }
}

static int ptimer_thread(void *arg) {
  (void)arg;
  for (;;) {
    kick_arg_t ka;
    ka.gen = __atomic_load_n(&g_kick_gen, __ATOMIC_ACQUIRE);
    fire_t fire[FIRE_BATCH];
    int nfire = 0;
    uint64_t now = sched_get_ticks();
    uint64_t next = 0;
    unsigned long fl = spin_lock_irqsave(&g_lock);
    for (ptimer_t *t = g_list; t; t = t->next) {
      if (nfire < FIRE_BATCH)
        expire_locked(t, now, fire, &nfire);
      if (t->armed && (next == 0 || t->expiry < next))
        next = t->expiry;
    }
    spin_unlock_irqrestore(&g_lock, fl);

    for (int i = 0; i < nfire; i++) {
      if (fire[i].tid)
        (void)signal_send_tid(fire[i].tid, fire[i].sig);
      else
        (void)process_kill(fire[i].pid, fire[i].sig);
    }

    uint64_t timeout = 0; // 0 = indefinido
    if (next) {
      now = sched_get_ticks();
      timeout = next > now ? next - now : 1;
    }
    wait_event_timeout(&g_kick_wq, kick_changed, &ka, timeout);
  }
  return 0;
}

static void ensure_started(void) {
  if (!__atomic_exchange_n(&g_started, 1, __ATOMIC_ACQ_REL)) {
    wait_queue_init(&g_kick_wq);
    kthread_run(ptimer_thread, NULL, "ptimer");
  }
}

static void remove_where(uint32_t pid, bool posix_only) {
  ptimer_t *dead = NULL;
  unsigned long fl = spin_lock_irqsave(&g_lock);
  ptimer_t **pp = &g_list;
  while (*pp) {
    ptimer_t *t = *pp;
    if (t->pid == pid && (!posix_only || t->id != PTIMER_ID_REAL)) {
      *pp = t->next;
      t->next = dead;
      dead = t;
    } else {
      pp = &t->next;
    }
  }
  spin_unlock_irqrestore(&g_lock, fl);
  while (dead) {
    ptimer_t *n = dead->next;
    kfree(dead);
    dead = n;
  }
}

void ptimer_exec(uint32_t pid) { remove_where(pid, true); }

void ptimer_exit(uint32_t pid) { remove_where(pid, false); }

// ---------------------------------------------------------------------------
// ITIMER_VIRTUAL / ITIMER_PROF (en el tick, contexto de IRQ)
// ---------------------------------------------------------------------------
static void cpu_itimer_tick(process_t *p, uint64_t *value, uint64_t *interval,
                            int sig) {
  uint64_t v = __atomic_load_n(value, __ATOMIC_RELAXED);
  if (v == 0)
    return;
  // Varios hilos del proceso pueden contar a la vez en CPUs distintas.
  if (__atomic_sub_fetch(value, 1, __ATOMIC_ACQ_REL) != 0)
    return;
  uint64_t itv = __atomic_load_n(interval, __ATOMIC_RELAXED);
  if (itv)
    __atomic_store_n(value, itv, __ATOMIC_RELEASE);
  // El proceso esta corriendo en esta CPU: basta con dejarla pendiente;
  // se entrega al volver a userland (irq_try_deliver_signals).
  __atomic_fetch_or(&p->pending_signals, 1ULL << sig, __ATOMIC_RELEASE);
}

void ptimer_account_tick(task_t *t) {
  if (!t || !t->proc)
    return;
  process_t *p = t->proc;
  cpu_itimer_tick(p, &p->itimer_virt_value, &p->itimer_virt_interval,
                  SIGVTALRM);
  cpu_itimer_tick(p, &p->itimer_prof_value, &p->itimer_prof_interval, SIGPROF);
}

// ---------------------------------------------------------------------------
// siginfo de las senales de temporizadores POSIX
// ---------------------------------------------------------------------------
bool ptimer_take_siginfo(process_t *p, int sig, ptimer_siginfo_t *out) {
  if (!p || !__atomic_load_n(&g_started, __ATOMIC_ACQUIRE))
    return false;
  bool found = false;
  unsigned long fl = spin_lock_irqsave(&g_lock);
  for (ptimer_t *t = g_list; t; t = t->next) {
    if (t->pid == p->pid && t->id != PTIMER_ID_REAL && t->queued &&
        t->signo == sig) {
      t->queued = false;
      t->last_overrun = t->overrun;
      t->overrun = 0;
      if (out) {
        out->timerid = t->id;
        out->overrun = t->last_overrun;
        out->value = t->value;
      }
      found = true;
      break;
    }
  }
  spin_unlock_irqrestore(&g_lock, fl);
  return found;
}

// ---------------------------------------------------------------------------
// Armar / leer (comun a ITIMER_REAL y POSIX). Con g_lock cogido.
// ---------------------------------------------------------------------------
static void read_locked(ptimer_t *t, uint64_t now, uint64_t *value,
                        uint64_t *interval) {
  *value = 0;
  *interval = 0;
  if (!t || !t->armed)
    return;
  *value = t->expiry > now ? t->expiry - now : 1; // vencido sin procesar: 1
  *interval = t->interval;
}

static void arm_locked(ptimer_t *t, uint64_t expiry, uint64_t interval) {
  t->armed = expiry != 0;
  t->expiry = expiry;
  t->interval = t->armed ? interval : 0;
  t->queued = false;
  t->overrun = 0;
}

// ITIMER_REAL. value 0 desarma. Devuelve el estado anterior en old_*.
static int real_set(uint32_t pid, uint64_t value, uint64_t interval,
                    uint64_t *old_value, uint64_t *old_interval) {
  ptimer_t *fresh = NULL;
  if (value) {
    ensure_started();
    fresh = (ptimer_t *)kzalloc(sizeof(*fresh));
    if (!fresh)
      return -ENOMEM;
  }
  uint64_t now = sched_get_ticks();
  unsigned long fl = spin_lock_irqsave(&g_lock);
  ptimer_t *t = find_locked(pid, PTIMER_ID_REAL);
  uint64_t ov, oi;
  read_locked(t, now, &ov, &oi);
  if (!t && value) {
    t = fresh;
    fresh = NULL;
    t->pid = pid;
    t->id = PTIMER_ID_REAL;
    t->signo = SIGALRM;
    t->notify = SIGEV_SIGNAL;
    t->next = g_list;
    g_list = t;
  }
  if (t)
    arm_locked(t, value ? now + value : 0, interval);
  spin_unlock_irqrestore(&g_lock, fl);
  if (fresh)
    kfree(fresh);
  if (value)
    kick();
  if (old_value)
    *old_value = ov;
  if (old_interval)
    *old_interval = oi;
  return 0;
}

// ---------------------------------------------------------------------------
// alarm / setitimer / getitimer
// ---------------------------------------------------------------------------
int64_t k_alarm(uint64_t sec, uint64_t a2, uint64_t a3, uint64_t a4,
                uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  uint32_t s = (uint32_t)sec; // unsigned int en Linux
  uint64_t old = 0;
  int r = real_set(proc->pid, (uint64_t)s * 1000ULL, 0, &old, NULL);
  if (r < 0)
    return r;
  // Como alarm_setitimer() de Linux: segundos restantes redondeados, y
  // nunca 0 si quedaba algo.
  uint64_t secs = old / 1000;
  uint64_t ms = old % 1000;
  if ((secs == 0 && ms) || ms >= 500)
    secs++;
  return (int64_t)secs;
}

int64_t k_setitimer(uint64_t which, uint64_t new_ptr, uint64_t old_ptr,
                    uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (which > ITIMER_PROF)
    return -EINVAL;

  struct k_itimerval nw;
  memset(&nw, 0, sizeof(nw)); // new NULL: desarmar (como Linux)
  if (new_ptr && (!access_ok((void *)new_ptr, sizeof(nw)) ||
                  copy_from_user(&nw, (void *)new_ptr, sizeof(nw)) < 0))
    return -EFAULT;
  uint64_t val = tv_to_ticks(&nw.value);
  uint64_t itv = tv_to_ticks(&nw.interval);
  if (val == (uint64_t)-1 || itv == (uint64_t)-1)
    return -EINVAL;

  uint64_t ov = 0, oi = 0;
  if (which == ITIMER_REAL) {
    int r = real_set(proc->pid, val, itv, &ov, &oi);
    if (r < 0)
      return r;
  } else {
    uint64_t *pv = which == ITIMER_VIRTUAL ? &proc->itimer_virt_value
                                           : &proc->itimer_prof_value;
    uint64_t *pi = which == ITIMER_VIRTUAL ? &proc->itimer_virt_interval
                                           : &proc->itimer_prof_interval;
    ov = __atomic_load_n(pv, __ATOMIC_ACQUIRE);
    oi = __atomic_load_n(pi, __ATOMIC_ACQUIRE);
    __atomic_store_n(pv, 0, __ATOMIC_RELEASE); // el tick no lo ve a medias
    __atomic_store_n(pi, val ? itv : 0, __ATOMIC_RELEASE);
    __atomic_store_n(pv, val, __ATOMIC_RELEASE);
  }

  if (old_ptr) {
    struct k_itimerval old;
    ticks_to_tv(ov, &old.value);
    ticks_to_tv(oi, &old.interval);
    if (!access_ok((void *)old_ptr, sizeof(old)) ||
        copy_to_user((void *)old_ptr, &old, sizeof(old)) < 0)
      return -EFAULT;
  }
  return 0;
}

int64_t k_getitimer(uint64_t which, uint64_t cur_ptr, uint64_t a3, uint64_t a4,
                    uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (which > ITIMER_PROF)
    return -EINVAL;
  if (!cur_ptr || !access_ok((void *)cur_ptr, sizeof(struct k_itimerval)))
    return -EFAULT;

  uint64_t v = 0, i = 0;
  if (which == ITIMER_REAL) {
    uint64_t now = sched_get_ticks();
    unsigned long fl = spin_lock_irqsave(&g_lock);
    read_locked(find_locked(proc->pid, PTIMER_ID_REAL), now, &v, &i);
    spin_unlock_irqrestore(&g_lock, fl);
  } else if (which == ITIMER_VIRTUAL) {
    v = __atomic_load_n(&proc->itimer_virt_value, __ATOMIC_ACQUIRE);
    i = __atomic_load_n(&proc->itimer_virt_interval, __ATOMIC_ACQUIRE);
  } else {
    v = __atomic_load_n(&proc->itimer_prof_value, __ATOMIC_ACQUIRE);
    i = __atomic_load_n(&proc->itimer_prof_interval, __ATOMIC_ACQUIRE);
  }
  struct k_itimerval cur;
  ticks_to_tv(v, &cur.value);
  ticks_to_tv(i, &cur.interval);
  if (copy_to_user((void *)cur_ptr, &cur, sizeof(cur)) < 0)
    return -EFAULT;
  return 0;
}

// ---------------------------------------------------------------------------
// Temporizadores POSIX
// ---------------------------------------------------------------------------
// `tid` es un hilo de `proc` (como exige Linux para SIGEV_THREAD_ID).
static bool tid_in_process(process_t *proc, int32_t tid) {
  if (tid <= 0)
    return false;
  if ((uint32_t)tid == proc->pid)
    return true;
  task_t *t = sched_find_tid_get((uint32_t)tid);
  if (!t)
    return false;
  bool ok = t->proc == proc;
  task_put(t);
  return ok;
}

static bool clock_ok(uint64_t clockid) {
  // CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_BOOTTIME. Los de CPU y los
  // *_ALARM no estan.
  return clockid == 0 || clockid == 1 || clockid == 7;
}

int64_t k_timer_create(uint64_t clockid, uint64_t sevp, uint64_t idp,
                       uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (!clock_ok(clockid))
    return -EINVAL;
  if (!idp || !access_ok((void *)idp, sizeof(int32_t)))
    return -EFAULT;

  struct k_sigevent ev;
  memset(&ev, 0, sizeof(ev));
  if (sevp) {
    if (!access_ok((void *)sevp, sizeof(ev)) ||
        copy_from_user(&ev, (void *)sevp, sizeof(ev)) < 0)
      return -EFAULT;
    switch (ev.notify) {
    case SIGEV_NONE:
      break;
    case SIGEV_SIGNAL:
    case SIGEV_THREAD_ID:
      if (ev.signo <= 0 || ev.signo >= SIG_MAX)
        return -EINVAL;
      if (ev.notify == SIGEV_THREAD_ID && !tid_in_process(proc, ev.tid))
        return -EINVAL;
      break;
    default:
      // SIGEV_THREAD lo resuelve la libc (con SIGEV_THREAD_ID); el kernel
      // de Linux tambien lo rechaza.
      return -EINVAL;
    }
  } else {
    ev.notify = SIGEV_SIGNAL;
    ev.signo = SIGALRM;
  }

  ptimer_t *t = (ptimer_t *)kzalloc(sizeof(*t));
  if (!t)
    return -ENOMEM;
  ensure_started();

  unsigned long fl = spin_lock_irqsave(&g_lock);
  int id = -1;
  for (int cand = 0; cand < PTIMER_MAX_PER_PROC; cand++) {
    if (!find_locked(proc->pid, cand)) {
      id = cand;
      break;
    }
  }
  if (id >= 0) {
    t->pid = proc->pid;
    t->id = id;
    t->clockid = (int)clockid;
    t->notify = ev.notify;
    t->tid = ev.notify == SIGEV_THREAD_ID ? (uint32_t)ev.tid : 0;
    t->signo = ev.notify == SIGEV_NONE ? 0 : ev.signo;
    // Sin sigevent, si_value lleva el id (como Linux).
    t->value = sevp ? ev.value : (uint64_t)id;
    t->next = g_list;
    g_list = t;
  }
  spin_unlock_irqrestore(&g_lock, fl);
  if (id < 0) {
    kfree(t);
    return -EAGAIN;
  }

  int32_t uid = id;
  if (copy_to_user((void *)idp, &uid, sizeof(uid)) < 0) {
    (void)k_timer_delete((uint64_t)id, 0, 0, 0, 0);
    return -EFAULT;
  }
  return 0;
}

int64_t k_timer_settime(uint64_t id, uint64_t flags, uint64_t new_ptr,
                        uint64_t old_ptr, uint64_t a5) {
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (flags & ~(uint64_t)TIMER_ABSTIME)
    return -EINVAL;
  struct k_itimerspec nw;
  if (!new_ptr || !access_ok((void *)new_ptr, sizeof(nw)) ||
      copy_from_user(&nw, (void *)new_ptr, sizeof(nw)) < 0)
    return -EFAULT;
  uint64_t val = ts_to_ticks(&nw.value);
  uint64_t itv = ts_to_ticks(&nw.interval);
  if (val == (uint64_t)-1 || itv == (uint64_t)-1)
    return -EINVAL;
  bool arm = nw.value.sec != 0 || nw.value.nsec != 0;
  int64_t rt_now = kernel_realtime_ms();

  uint64_t ov = 0, oi = 0;
  uint64_t now = sched_get_ticks();
  unsigned long fl = spin_lock_irqsave(&g_lock);
  ptimer_t *t = find_locked(proc->pid, (int)(int32_t)id);
  if (!t || (int32_t)id < 0) {
    spin_unlock_irqrestore(&g_lock, fl);
    return -EINVAL;
  }
  read_locked(t, now, &ov, &oi);
  uint64_t expiry = 0;
  if (arm) {
    if (flags & TIMER_ABSTIME) {
      int64_t delta = t->clockid == 0 ? (int64_t)val - rt_now
                                      : (int64_t)val - (int64_t)now;
      expiry = delta > 0 ? now + (uint64_t)delta : now; // pasado: vence ya
    } else {
      expiry = now + (val ? val : 1);
    }
  }
  arm_locked(t, expiry, itv);
  spin_unlock_irqrestore(&g_lock, fl);
  if (arm)
    kick();

  if (old_ptr) {
    struct k_itimerspec old;
    ticks_to_ts(ov, &old.value);
    ticks_to_ts(oi, &old.interval);
    if (!access_ok((void *)old_ptr, sizeof(old)) ||
        copy_to_user((void *)old_ptr, &old, sizeof(old)) < 0)
      return -EFAULT;
  }
  return 0;
}

int64_t k_timer_gettime(uint64_t id, uint64_t cur_ptr, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (!cur_ptr || !access_ok((void *)cur_ptr, sizeof(struct k_itimerspec)))
    return -EFAULT;
  uint64_t v = 0, i = 0;
  uint64_t now = sched_get_ticks();
  unsigned long fl = spin_lock_irqsave(&g_lock);
  ptimer_t *t = (int32_t)id < 0 ? NULL : find_locked(proc->pid, (int)id);
  if (t)
    read_locked(t, now, &v, &i);
  spin_unlock_irqrestore(&g_lock, fl);
  if (!t)
    return -EINVAL;
  struct k_itimerspec cur;
  ticks_to_ts(v, &cur.value);
  ticks_to_ts(i, &cur.interval);
  if (copy_to_user((void *)cur_ptr, &cur, sizeof(cur)) < 0)
    return -EFAULT;
  return 0;
}

int64_t k_timer_getoverrun(uint64_t id, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  int r = -EINVAL;
  unsigned long fl = spin_lock_irqsave(&g_lock);
  ptimer_t *t = (int32_t)id < 0 ? NULL : find_locked(proc->pid, (int)id);
  if (t)
    r = t->last_overrun;
  spin_unlock_irqrestore(&g_lock, fl);
  return r;
}

int64_t k_timer_delete(uint64_t id, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if ((int32_t)id < 0)
    return -EINVAL;
  ptimer_t *dead = NULL;
  unsigned long fl = spin_lock_irqsave(&g_lock);
  for (ptimer_t **pp = &g_list; *pp; pp = &(*pp)->next) {
    if ((*pp)->pid == proc->pid && (*pp)->id == (int)id) {
      dead = *pp;
      *pp = dead->next;
      break;
    }
  }
  spin_unlock_irqrestore(&g_lock, fl);
  if (!dead)
    return -EINVAL;
  kfree(dead);
  return 0;
}
