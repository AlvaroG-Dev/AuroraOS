// kernel/irq_threaded.c
#include "irq_threaded.h"
#include "apic.h" // lapic_eoi (no usado aquí pero por si acaso)
#include "idt.h"  // MSI_VECTOR_BASE, MSI_VECTOR_END
#include "klog.h"
#include "kthread.h"
#include "sched.h"
#include "spinlock.h"
#include "string.h"
#include "uaccess.h"
#include "wait.h"
#include <stddef.h>

// ---------------------------------------------------------------------------
// Tamaño de la tabla: 0x60 + 32 = 0x80 = 128 entradas.
// ---------------------------------------------------------------------------
#define IRQ_THREADED_MAX (MSI_VECTOR_END + 1)

struct irq_action {
  irq_handler_fn ack; // escrito bajo g_lock, leído con ACQUIRE
  irq_handler_fn thread_fn;
  void *dev_id;
  const char *name;
  struct kthread
      *thread;     // creado por irq_request_threaded, NULL si no hay thread_fn
  wait_queue_t wq; // el kthread duerme aquí
  volatile int pending; // 1 = hay trabajo pendiente
  volatile int active;  // dispatchers dentro del ack ahora mismo (irq_free)
};

static struct irq_action g_actions[IRQ_THREADED_MAX];
static spinlock_t g_lock;
static int g_initialized = 0;

// ---------------------------------------------------------------------------
// Rango de irq válido: 0..15 (IOAPIC) o 0x60..0x7F (MSI).
// ---------------------------------------------------------------------------
static int irq_is_valid(unsigned int irq) {
  if (irq <= 15)
    return 1;
  if (irq >= MSI_VECTOR_BASE && irq <= MSI_VECTOR_END)
    return 1;
  return 0;
}

// ---------------------------------------------------------------------------
// Thread fn: bucle que espera pending>0 o kthread_should_stop, y llama
// al thread_fn. Después de cada invocación vuelve a esperar.
//
// El `pending` se limpia ANTES de invocar el thread_fn: si llega otra
// IRQ mientras corre, el dispatch pone pending=1 y `wake_up` despierta
// al kthread en la siguiente iteración.
// ---------------------------------------------------------------------------
static bool irq_thread_should_run(void *arg) {
  struct irq_action *a = (struct irq_action *)arg;
  if (kthread_should_stop())
    return true;
  return __atomic_load_n(&a->pending, __ATOMIC_ACQUIRE) != 0;
}

static int irq_thread_fn_main(void *arg) {
  struct irq_action *a = (struct irq_action *)arg;

  while (!kthread_should_stop()) {
    wait_event_interruptible(&a->wq, irq_thread_should_run, a);
    if (kthread_should_stop())
      break;
    if (!__atomic_load_n(&a->pending, __ATOMIC_ACQUIRE))
      continue;

    __atomic_store_n(&a->pending, 0, __ATOMIC_RELEASE);

    irq_handler_fn fn = __atomic_load_n(&a->thread_fn, __ATOMIC_ACQUIRE);
    if (fn) {
      void *dev = __atomic_load_n(&a->dev_id, __ATOMIC_ACQUIRE);
      int irq = 0;
      for (int i = 0; i < IRQ_THREADED_MAX; i++) {
        if (&g_actions[i] == a) {
          irq = i;
          break;
        }
      }
      (void)fn(irq, dev);
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Dispatch unificado. Lo llaman:
//   - irq_handler() (vector 32..47) tras traducir irq = int_num - 32
//   - msi_handler() (vector 0x60..0x7F) con irq = int_num
//
// Devuelve:
//   IRQ_NONE      → no hay handler threaded registrado (el caller debe
//                   hacer fallback al path legacy).
//   IRQ_HANDLED   → el ack ya atendió la fuente.
//   IRQ_WAKE_THREAD → el ack ya atendió la fuente y pidió despertar
//                     el thread.
//
// Nota: esta función NO manda el EOI. El caller lo hace en el momento
// adecuado (después del ack, antes del thread_fn).
// ---------------------------------------------------------------------------
irqreturn_t irq_threaded_dispatch(int irq) {
  if (!irq_is_valid((unsigned int)irq))
    return IRQ_NONE;

  struct irq_action *a = &g_actions[irq];
  // `active` se publica ANTES de leer los punteros (seq_cst en los dos
  // lados): o irq_free ve active > 0 y espera, o aquí se ve ack == NULL.
  __atomic_fetch_add(&a->active, 1, __ATOMIC_SEQ_CST);
  irq_handler_fn ack = __atomic_load_n(&a->ack, __ATOMIC_SEQ_CST);
  irq_handler_fn tfn = __atomic_load_n(&a->thread_fn, __ATOMIC_SEQ_CST);

  if (!ack && !tfn) {
    __atomic_fetch_sub(&a->active, 1, __ATOMIC_RELEASE);
    return IRQ_NONE;
  }

  irqreturn_t ret = IRQ_HANDLED;

  if (ack) {
    void *dev = __atomic_load_n(&a->dev_id, __ATOMIC_ACQUIRE);
    ret = ack(irq, dev);
  }

  // Solo despertamos el thread si el ack lo pidió explícitamente.
  // Si ret==IRQ_NONE, la fuente no era para este handler; no tocamos.
  if (ret == IRQ_WAKE_THREAD && tfn) {
    __atomic_store_n(&a->pending, 1, __ATOMIC_RELEASE);
    wake_up_one(&a->wq);
  }

  __atomic_fetch_sub(&a->active, 1, __ATOMIC_RELEASE);
  return ret;
}

// ---------------------------------------------------------------------------
// Registro
// ---------------------------------------------------------------------------
int irq_request_threaded(unsigned int irq, irq_handler_fn ack,
                         irq_handler_fn thread_fn, const char *name,
                         void *dev_id) {
  if (!g_initialized)
    return -EINVAL;
  if (!irq_is_valid(irq))
    return -EINVAL;
  if (!ack && !thread_fn)
    return -EINVAL;

  struct irq_action *a = &g_actions[irq];

  unsigned long flags = spin_lock_irqsave(&g_lock);

  // Rechazar doble registro. Comparamos ack Y thread_fn por si alguien
  // registró el mismo irq para dos drivers distintos (compartida).
  // Como no soportamos IRQs compartidas todavía, cualquier segundo
  // request falla con -EBUSY.
  if (a->ack || a->thread_fn) {
    spin_unlock_irqrestore(&g_lock, flags);
    return -EBUSY;
  }

  // Reserva (aún sin thread).
  __atomic_store_n(&a->ack, ack, __ATOMIC_RELEASE);
  __atomic_store_n(&a->thread_fn, thread_fn, __ATOMIC_RELEASE);
  __atomic_store_n(&a->dev_id, dev_id, __ATOMIC_RELEASE);
  a->name = name ? name : "irq";
  __atomic_store_n(&a->pending, 0, __ATOMIC_RELEASE);
  a->thread = NULL;
  wait_queue_init(&a->wq);

  spin_unlock_irqrestore(&g_lock, flags);

  // Crear el kthread. Debe ser fuera del lock porque kthread_run puede
  // dormir (kmalloc, sched_publish_task con IPIs).
  if (thread_fn) {
    char tname[16];
    // Formato: "irq/NN-name" truncado a 15 chars.
    int p = 0;
    const char *pre = "irq/";
    while (*pre && p < (int)sizeof(tname) - 1)
      tname[p++] = *pre++;
    // Número
    char nbuf[4];
    int n = 0;
    unsigned int v = irq;
    if (v == 0) {
      nbuf[n++] = '0';
    } else {
      while (v > 0 && n < (int)sizeof(nbuf)) {
        nbuf[n++] = (char)('0' + (v % 10));
        v /= 10;
      }
    }
    while (n > 0 && p < (int)sizeof(tname) - 1)
      tname[p++] = nbuf[--n];
    if (p < (int)sizeof(tname) - 1)
      tname[p++] = '-';
    const char *tail = (name && name[0]) ? name : "handled";
    while (*tail && p < (int)sizeof(tname) - 1)
      tname[p++] = *tail++;
    tname[p] = '\0';

    struct kthread *kt = kthread_run(irq_thread_fn_main, a, tname);
    if (!kt) {
      LOG_ERR("[IRQ] irq=%u: no se pudo crear kthread '%s'", irq, tname);
      // Rollback de la tabla.
      unsigned long f2 = spin_lock_irqsave(&g_lock);
      __atomic_store_n(&a->ack, (irq_handler_fn)NULL, __ATOMIC_RELEASE);
      __atomic_store_n(&a->thread_fn, (irq_handler_fn)NULL, __ATOMIC_RELEASE);
      __atomic_store_n(&a->dev_id, (void *)NULL, __ATOMIC_RELEASE);
      spin_unlock_irqrestore(&g_lock, f2);
      return -ENOMEM;
    }

    // Publicar el thread en la entrada.
    unsigned long f3 = spin_lock_irqsave(&g_lock);
    a->thread = kt;
    spin_unlock_irqrestore(&g_lock, f3);
  }

  LOG_INFO("[IRQ] irq=%u registrado (ack=%s thread=%s name='%s')", irq,
           ack ? "yes" : "no", thread_fn ? "yes" : "no", a->name);
  return 0;
}

// ---------------------------------------------------------------------------
// Unregister
// ---------------------------------------------------------------------------
void irq_free(unsigned int irq) {
  if (!irq_is_valid(irq))
    return;

  struct irq_action *a = &g_actions[irq];

  unsigned long flags = spin_lock_irqsave(&g_lock);
  if (!a->ack && !a->thread_fn) {
    spin_unlock_irqrestore(&g_lock, flags);
    return;
  }

  struct kthread *kt = a->thread;

  // Despublicar la tabla ANTES de parar el kthread: si llega una IRQ
  // mientras esperamos al kthread, dispatch ve ack=NULL y retorna
  // IRQ_NONE, no entra en el handler del driver que puede estar en
  // proceso de desmontaje.
  __atomic_store_n(&a->ack, (irq_handler_fn)NULL, __ATOMIC_SEQ_CST);
  __atomic_store_n(&a->thread_fn, (irq_handler_fn)NULL, __ATOMIC_SEQ_CST);
  a->thread = NULL;
  a->name = NULL;
  __atomic_store_n(&a->pending, 0, __ATOMIC_RELEASE);

  spin_unlock_irqrestore(&g_lock, flags);

  // Esperar a que salga un ack que ya estuviera corriendo en otra CPU: al
  // volver, el llamante puede liberar dev_id o descargar el .kur del handler.
  // (Llamarla desde el propio handler se interbloquea, como free_irq en
  // Linux.)
  while (__atomic_load_n(&a->active, __ATOMIC_ACQUIRE) != 0)
    __asm__ volatile("pause");
  __atomic_store_n(&a->dev_id, (void *)NULL, __ATOMIC_RELEASE);

  if (kt) {
    kthread_stop(kt);
    kt = NULL;
  }

  // Vaciar la wait queue por si el kthread dejó entradas.
  wait_queue_init(&a->wq);

  LOG_INFO("[IRQ] irq=%u liberado", irq);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
int irq_threaded_init(void) {
  if (g_initialized)
    return 0;

  spin_init(&g_lock);
  for (int i = 0; i < IRQ_THREADED_MAX; i++) {
    g_actions[i].ack = NULL;
    g_actions[i].thread_fn = NULL;
    g_actions[i].dev_id = NULL;
    g_actions[i].name = NULL;
    g_actions[i].thread = NULL;
    g_actions[i].pending = 0;
    g_actions[i].active = 0;
    wait_queue_init(&g_actions[i].wq);
  }

  g_initialized = 1;
  LOG_INFO("[IRQ] subsistema threaded listo (%d slots)", IRQ_THREADED_MAX);
  return 0;
}

// ---------------------------------------------------------------------------
// Simulación para tests. Llama al ack desde el contexto actual y
// despierta el thread si procede. No manda EOI (no toca el LAPIC).
// ---------------------------------------------------------------------------
void irq_threaded_simulate_for_test(unsigned int irq) {
  if (!irq_is_valid(irq))
    return;
  (void)irq_threaded_dispatch(irq);
}