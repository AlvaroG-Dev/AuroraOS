// kernel/kthread.c
#include "kthread.h"
#include "heap.h"
#include "klog.h"
#include "sched.h"
#include "uaccess.h"
#include "wait.h"
#include <stddef.h>

// ---------------------------------------------------------------------------
// Handle interno. Vive en kthread.c; no se expone.
// ---------------------------------------------------------------------------
struct kthread {
  task_t *task; // back-pointer; NULL tras kthread_stop
  int (*fn)(void *);
  void *data;
  int exit_code;
  volatile int
      should_stop; // escrito por kthread_stop, leído por kthread_should_stop
  int started;     // 1 tras kthread_start
  wait_queue_t exited_wq; // despertada por kthread_do_exit
  char name[16];
};

// ---------------------------------------------------------------------------
// Cuerpo de salida compartido entre el trampolín y kthread_exit().
// ---------------------------------------------------------------------------
static __attribute__((noreturn)) void kthread_do_exit(int code) {
  task_t *cur = sched_current();
  struct kthread *kt = cur ? cur->kthread : NULL;

  if (kt) {
    kt->exit_code = code;
    __atomic_store_n((int *)&cur->state, TASK_DEAD, __ATOMIC_RELEASE);
    wake_up_all(&kt->exited_wq);
  } else if (cur) {
    __atomic_store_n((int *)&cur->state, TASK_DEAD, __ATOMIC_RELEASE);
  }

  __asm__ volatile("cli" ::: "memory");
  sched_yield();
  // Sólo se llega aquí si sched_yield no cambió de tarea (situación
  // teórica en single-CPU). El tick eventualmente nos cambiará al idle.
  while (1)
    __asm__ volatile("sti; hlt");
}

// ---------------------------------------------------------------------------
// Trampolín con el que se publica el kthread. task_entry_wrapper(fn) del
// scheduler llamará a esto, y nosotros resolvemos kt->fn(kt->data).
// ---------------------------------------------------------------------------
static void kthread_entry_wrapper(void) {
  task_t *cur = sched_current();
  struct kthread *kt = cur ? cur->kthread : NULL;

  if (!kt) {
    // Invariante rota: alguien publicó un kthread con un task_t que no
    // tiene back-pointer. No hay forma limpia de continuar.
    LOG_ERR("[KTHREAD] task %u sin back-pointer a struct kthread",
            cur ? cur->id : 0);
    if (cur)
      cur->state = TASK_DEAD;
    sched_yield();
    while (1)
      __asm__ volatile("sti; hlt");
  }

  int ret = kt->fn(kt->data);
  kthread_do_exit(ret);
}

// ---------------------------------------------------------------------------
// Creación
// ---------------------------------------------------------------------------
struct kthread *kthread_create(int (*fn)(void *), void *data,
                               const char *name) {
  if (!fn)
    return NULL;

  struct kthread *kt = (struct kthread *)kzalloc(sizeof(*kt));
  if (!kt)
    return NULL;

  kt->fn = fn;
  kt->data = data;
  kt->exit_code = 0;
  kt->should_stop = 0;
  kt->started = 0;
  kt->task = NULL;
  wait_queue_init(&kt->exited_wq);

  // Copiar nombre truncado a 15 chars.
  size_t nl = 0;
  if (name) {
    while (name[nl] && nl < sizeof(kt->name) - 1) {
      kt->name[nl] = name[nl];
      nl++;
    }
  }
  if (nl == 0) {
    const char *d = "kthread";
    while (*d && nl < sizeof(kt->name) - 1)
      kt->name[nl++] = *d++;
  }
  kt->name[nl] = '\0';

  // Crear el task en estado no-publicado.
  task_t *t = sched_create_kernel_task_stopped(kthread_entry_wrapper);
  if (!t) {
    kfree(kt);
    return NULL;
  }

  // Publicar el back-pointer ANTES de exponerlo por sched_publish_task.
  t->kthread = kt;
  kt->task = t;

  // Ref extra para el handle. La libera kthread_stop junto con kt.
  // A partir de aquí, el task_t no se libera aunque el reaper lo intente
  // reapear; refcount se queda >= 1 hasta kthread_stop.
  task_get(t);

  return kt;
}

int kthread_start(struct kthread *kt) {
  if (!kt || !kt->task)
    return -EINVAL;
  if (__atomic_exchange_n(&kt->started, 1, __ATOMIC_ACQ_REL))
    return -EINVAL;
  sched_publish_task(kt->task);
  return 0;
}

struct kthread *kthread_run(int (*fn)(void *), void *data, const char *name) {
  struct kthread *kt = kthread_create(fn, data, name);
  if (!kt)
    return NULL;
  // kthread_start sólo falla si ya estaba started; un kthread recién
  // creado nunca lo está. Ignoramos el retorno por contrato.
  (void)kthread_start(kt);
  return kt;
}

// ---------------------------------------------------------------------------
// Stop
// ---------------------------------------------------------------------------
static bool task_is_fully_dead(void *arg) {
  task_t *t = (task_t *)arg;
  int state = __atomic_load_n((int *)&t->state, __ATOMIC_ACQUIRE);
  if (state != TASK_DEAD)
    return false;
  int on_cpu = __atomic_load_n((int *)&t->on_cpu, __ATOMIC_ACQUIRE);
  return on_cpu == 0;
}

int kthread_stop(struct kthread *kt) {
  if (!kt || !kt->task)
    return -EINVAL;

  task_t *t = kt->task;

  // 1) Señal de parada. Release para que el worker vea fn/exit_code
  //    previos a la escritura del flag.
  __atomic_store_n(&kt->should_stop, 1, __ATOMIC_RELEASE);

  // 2) Despertar si está bloqueado en alguna wait queue (típicamente
  //    worker_wq, pero podría ser cualquier otra).
  if (t->waiting_on)
    wait_queue_wake_task(t);

  // 3) Esperar a state==DEAD && on_cpu==0. Usamos timeout corto porque
  //    el kthread hace sched_yield() y la CPU tarda hasta un tick en
  //    actualizar on_cpu. La condición se reevalúa también al despertar
  //    por exited_wq.
  for (;;) {
    long r = wait_event_timeout(&kt->exited_wq, task_is_fully_dead, t, 5);
    if (r > 0)
      break;
  }

  // 4) Recoger resultado y desvincular. A partir de aquí, el task_t
  //    sigue siendo válido (tenemos la ref extra), pero ya no referencia
  //    al handle.
  int code = kt->exit_code;
  t->kthread = NULL;
  kt->task = NULL;

  // 5) Liberar el handle y soltar la ref extra (probablemente dispara
  //    el free del task_t, stack incluido).
  kfree(kt);
  task_put(t);

  return code;
}

// ---------------------------------------------------------------------------
// Consultas
// ---------------------------------------------------------------------------
int kthread_should_stop(void) {
  task_t *cur = sched_current();
  struct kthread *kt = cur ? cur->kthread : NULL;
  return kt ? __atomic_load_n(&kt->should_stop, __ATOMIC_ACQUIRE) : 0;
}

task_t *kthread_task(struct kthread *kt) { return kt ? kt->task : NULL; }

const char *kthread_name(struct kthread *kt) { return kt ? kt->name : ""; }

struct kthread *kthread_current(void) {
  task_t *cur = sched_current();
  return cur ? cur->kthread : NULL;
}

__attribute__((noreturn)) void kthread_exit(int code) { kthread_do_exit(code); }