// kernel/workqueue.c
#include "workqueue.h"
#include "cpu.h"
#include "heap.h"
#include "klog.h"
#include "kthread.h"
#include "sched.h"
#include "smp_boot.h"
#include "spinlock.h"
#include "string.h"
#include "uaccess.h"
#include <stddef.h>

#define WORKER_MAX_SLEEP_TICKS 100

struct workqueue_struct {
  char name[16];
  struct work_struct *pending_list; // inmediato, LIFO
  struct work_struct *delayed_list; // ordenado por deadline ascendente
  spinlock_t lock;
  wait_queue_t worker_wq;  // workers esperan aquí
  wait_queue_t drained_wq; // flush_workqueue espera aquí
  struct kthread **workers;
  int nr_workers;
  int nr_pending; // entradas en pending_list
  int nr_running; // fns ejecutándose ahora
  int ordered;
};

// Colas globales.
struct workqueue_struct *system_wq = NULL;
struct workqueue_struct *system_ordered_wq = NULL;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void format_worker_name(char *out, size_t outlen, const char *base,
                               int id) {
  size_t n = 0;
  while (base && *base && n + 2 < outlen)
    out[n++] = *base++;
  if (n + 1 < outlen)
    out[n++] = '/';
  if (id == 0) {
    if (n + 1 < outlen)
      out[n++] = '0';
  } else {
    char tmp[8];
    int t = 0;
    int v = id;
    while (v > 0 && t < (int)sizeof(tmp)) {
      tmp[t++] = (char)('0' + (v % 10));
      v /= 10;
    }
    while (t > 0 && n + 1 < outlen)
      out[n++] = tmp[--t];
  }
  out[n] = '\0';
}

// Lecturas sin lock permitidas: sólo se usan como condición de wake; si
// hay carrera, el wait_event_timeout con timeout corto la resuelve.
static bool worker_has_work(void *arg) {
  struct workqueue_struct *wq = (struct workqueue_struct *)arg;
  if (kthread_should_stop())
    return true;
  if (__atomic_load_n((void *volatile *)&wq->pending_list, __ATOMIC_ACQUIRE))
    return true;
  struct work_struct *d =
      __atomic_load_n((void *volatile *)&wq->delayed_list, __ATOMIC_ACQUIRE);
  if (d && d->deadline <= sched_get_ticks())
    return true;
  return false;
}

static bool wq_immediate_empty_locked(struct workqueue_struct *wq) {
  return wq->pending_list == NULL && wq->nr_running == 0;
}

static bool wq_immediate_empty_cond(void *arg) {
  struct workqueue_struct *wq = (struct workqueue_struct *)arg;
  unsigned long flags = spin_lock_irqsave(&wq->lock);
  int empty = wq_immediate_empty_locked(wq);
  spin_unlock_irqrestore(&wq->lock, flags);
  return empty != 0;
}

static bool work_is_idle_cond(void *arg) {
  struct work_struct *w = (struct work_struct *)arg;
  return __atomic_load_n(&w->state, __ATOMIC_ACQUIRE) == WORK_IDLE;
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------
static int worker_main(void *arg) {
  struct workqueue_struct *wq = (struct workqueue_struct *)arg;

  while (!kthread_should_stop()) {
    struct work_struct *w = NULL;
    uint64_t wait_ticks = WORKER_MAX_SLEEP_TICKS;

    unsigned long flags = spin_lock_irqsave(&wq->lock);

    // Promocionar delayed ya vencidos a pending.
    uint64_t now = sched_get_ticks();
    struct work_struct **dpp = &wq->delayed_list;
    while (*dpp) {
      struct work_struct *d = *dpp;
      if (d->deadline <= now) {
        *dpp = d->next;
        d->next = wq->pending_list;
        wq->pending_list = d;
        // nr_pending no cambia (ya contaba).
      } else {
        dpp = &d->next;
      }
    }

    if (wq->pending_list) {
      w = wq->pending_list;
      wq->pending_list = w->next;
      w->next = NULL;
      wq->nr_pending--;
      wq->nr_running++;
      __atomic_store_n(&w->state, WORK_RUNNING, __ATOMIC_RELEASE);
    } else if (wq->delayed_list) {
      uint64_t nd = wq->delayed_list->deadline;
      uint64_t now2 = sched_get_ticks();
      wait_ticks = (nd > now2) ? (nd - now2) : 1;
      if (wait_ticks > WORKER_MAX_SLEEP_TICKS)
        wait_ticks = WORKER_MAX_SLEEP_TICKS;
    }

    spin_unlock_irqrestore(&wq->lock, flags);

    if (w) {
      w->fn(w);

      flags = spin_lock_irqsave(&wq->lock);
      wq->nr_running--;
      __atomic_store_n(&w->state, WORK_IDLE, __ATOMIC_RELEASE);
      w->wq = NULL;
      int empty = wq_immediate_empty_locked(wq);
      spin_unlock_irqrestore(&wq->lock, flags);

      // Wake fuera del lock, para no incurrir en AB-BA con wait_common
      // (que evalúa la condición bajo el lock de wq).
      wake_up_all(&w->done_wq);
      if (empty)
        wake_up_all(&wq->drained_wq);
      continue;
    }

    // Nada que hacer: dormir hasta trabajo, timeout o stop.
    wait_event_timeout(&wq->worker_wq, worker_has_work, wq, wait_ticks);
  }

  return 0;
}

// ---------------------------------------------------------------------------
// alloc / destroy
// ---------------------------------------------------------------------------
struct workqueue_struct *alloc_workqueue(const char *name, int nr_workers,
                                         int ordered) {
  struct workqueue_struct *wq = (struct workqueue_struct *)kzalloc(sizeof(*wq));
  if (!wq)
    return NULL;

  size_t nl = 0;
  if (name) {
    while (name[nl] && nl < sizeof(wq->name) - 1) {
      wq->name[nl] = name[nl];
      nl++;
    }
  }
  if (nl == 0) {
    const char *d = "wq";
    while (*d && nl < sizeof(wq->name) - 1)
      wq->name[nl++] = *d++;
  }
  wq->name[nl] = '\0';

  spin_init(&wq->lock);
  wait_queue_init(&wq->worker_wq);
  wait_queue_init(&wq->drained_wq);

  if (nr_workers <= 0) {
    nr_workers = MAX_CPUS;
    if (nr_workers > 16)
      nr_workers = 16;
  }
  if (ordered && nr_workers > 1)
    nr_workers = 1;
  if (nr_workers < 1)
    nr_workers = 1;

  wq->ordered = ordered;
  wq->nr_workers = nr_workers;
  wq->workers =
      (struct kthread **)kzalloc(sizeof(struct kthread *) * nr_workers);
  if (!wq->workers) {
    kfree(wq);
    return NULL;
  }

  for (int i = 0; i < nr_workers; i++) {
    char wname[16];
    format_worker_name(wname, sizeof(wname), wq->name, i);
    wq->workers[i] = kthread_run(worker_main, wq, wname);
    if (!wq->workers[i]) {
      LOG_ERR("[WQ] no se pudo crear worker %d de '%s'", i, wq->name);
      // Rollback: parar los que sí creamos.
      for (int j = 0; j < i; j++) {
        kthread_stop(wq->workers[j]);
        wq->workers[j] = NULL;
      }
      kfree(wq->workers);
      kfree(wq);
      return NULL;
    }
  }

  LOG_INFO("[WQ] '%s' creada (%d worker%s%s)", wq->name, nr_workers,
           nr_workers == 1 ? "" : "s", ordered ? ", ordered" : "");
  return wq;
}

void destroy_workqueue(struct workqueue_struct *wq) {
  if (!wq)
    return;

  // Drenar trabajo inmediato. Si un worker se queda atascado en una fn,
  // esto bloquea — pero eso es un bug del cliente, no de la wq.
  flush_workqueue(wq);

  for (int i = 0; i < wq->nr_workers; i++) {
    if (wq->workers[i]) {
      kthread_stop(wq->workers[i]);
      wq->workers[i] = NULL;
    }
  }

  // Descartar delayed que nunca llegaron a dispararse.
  if (wq->delayed_list) {
    LOG_WARN("[WQ] '%s': descartando trabajo delayed sin ejecutar", wq->name);
    struct work_struct *w = wq->delayed_list;
    wq->delayed_list = NULL;
    while (w) {
      struct work_struct *n = w->next;
      w->next = NULL;
      w->wq = NULL;
      __atomic_store_n(&w->state, WORK_IDLE, __ATOMIC_RELEASE);
      wake_up_all(&w->done_wq); // por si alguien espera en flush_work
      w = n;
    }
    wq->nr_pending = 0;
  }

  kfree(wq->workers);
  kfree(wq);
}

// ---------------------------------------------------------------------------
// queue
// ---------------------------------------------------------------------------
bool queue_work(struct workqueue_struct *wq, struct work_struct *work) {
  if (!wq || !work || !work->fn)
    return false;

  int expected = WORK_IDLE;
  if (!__atomic_compare_exchange_n(&work->state, &expected, WORK_QUEUED, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return false;

  work->deadline = 0;
  work->wq = wq;

  unsigned long flags = spin_lock_irqsave(&wq->lock);
  work->next = wq->pending_list;
  wq->pending_list = work;
  wq->nr_pending++;
  spin_unlock_irqrestore(&wq->lock, flags);

  wake_up_all(&wq->worker_wq);
  return true;
}

bool queue_delayed_work(struct workqueue_struct *wq, struct work_struct *work,
                        uint64_t ticks) {
  if (!wq || !work || !work->fn)
    return false;
  if (ticks == 0)
    return queue_work(wq, work);

  int expected = WORK_IDLE;
  if (!__atomic_compare_exchange_n(&work->state, &expected, WORK_QUEUED, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return false;

  work->deadline = sched_get_ticks() + ticks;
  work->wq = wq;

  unsigned long flags = spin_lock_irqsave(&wq->lock);
  struct work_struct **pp = &wq->delayed_list;
  while (*pp && (*pp)->deadline <= work->deadline)
    pp = &(*pp)->next;
  work->next = *pp;
  *pp = work;
  wq->nr_pending++;
  spin_unlock_irqrestore(&wq->lock, flags);

  wake_up_all(&wq->worker_wq);
  return true;
}

// ---------------------------------------------------------------------------
// flush / cancel
// ---------------------------------------------------------------------------
bool flush_work(struct work_struct *work) {
  if (!work)
    return true;
  wait_event(&work->done_wq, work_is_idle_cond, work);
  return true;
}

void flush_workqueue(struct workqueue_struct *wq) {
  if (!wq)
    return;
  wait_event(&wq->drained_wq, wq_immediate_empty_cond, wq);
}

// Quita `work` de la lista que corresponda. `work->wq->lock` debe estar
// cogido. Devuelve 1 si lo encontró y quitó, 0 si no estaba en ninguna
// lista (carrera con el worker que ya lo tomó).
static int _detach_from_lists_locked(struct workqueue_struct *wq,
                                     struct work_struct *work) {
  struct work_struct **pp = &wq->pending_list;
  while (*pp && *pp != work)
    pp = &(*pp)->next;
  if (*pp == work) {
    *pp = work->next;
    work->next = NULL;
    if (wq->nr_pending > 0)
      wq->nr_pending--;
    return 1;
  }
  pp = &wq->delayed_list;
  while (*pp && *pp != work)
    pp = &(*pp)->next;
  if (*pp == work) {
    *pp = work->next;
    work->next = NULL;
    if (wq->nr_pending > 0)
      wq->nr_pending--;
    return 1;
  }
  return 0;
}

bool cancel_work_sync(struct work_struct *work) {
  if (!work)
    return false;

  for (;;) {
    int state = __atomic_load_n(&work->state, __ATOMIC_ACQUIRE);
    if (state == WORK_IDLE)
      return false;
    if (state == WORK_RUNNING) {
      wait_event(&work->done_wq, work_is_idle_cond, work);
      return true;
    }

    struct workqueue_struct *wq = work->wq;
    if (!wq) {
      // Invariante rota; esperar un poco y reintentar.
      wait_event_timeout(&work->done_wq, work_is_idle_cond, work, 5);
      continue;
    }

    unsigned long flags = spin_lock_irqsave(&wq->lock);
    if (__atomic_load_n(&work->state, __ATOMIC_ACQUIRE) != WORK_QUEUED) {
      spin_unlock_irqrestore(&wq->lock, flags);
      continue;
    }
    if (_detach_from_lists_locked(wq, work)) {
      work->wq = NULL;
      __atomic_store_n(&work->state, WORK_IDLE, __ATOMIC_RELEASE);
      int empty = wq_immediate_empty_locked(wq);
      spin_unlock_irqrestore(&wq->lock, flags);

      wake_up_all(&work->done_wq);
      if (empty)
        wake_up_all(&wq->drained_wq);
      return true;
    }
    // Otra CPU se lo llevó; volver a comprobar.
    spin_unlock_irqrestore(&wq->lock, flags);
  }
}

// ---------------------------------------------------------------------------
// Global init / shutdown
// ---------------------------------------------------------------------------
int workqueue_init(void) {
  if (system_wq)
    return 0;

  system_wq = alloc_workqueue("events", WQ_SYSTEM_WORKERS, 0);
  if (!system_wq)
    return -ENOMEM;

  system_ordered_wq = alloc_workqueue("ordered", 1, 1);
  if (!system_ordered_wq) {
    destroy_workqueue(system_wq);
    system_wq = NULL;
    return -ENOMEM;
  }

  LOG_INFO("[WQ] workqueues globales inicializadas (%d workers)",
           WQ_SYSTEM_WORKERS);
  return 0;
}

void workqueue_shutdown(void) {
  if (system_ordered_wq) {
    destroy_workqueue(system_ordered_wq);
    system_ordered_wq = NULL;
  }
  if (system_wq) {
    destroy_workqueue(system_wq);
    system_wq = NULL;
  }
}