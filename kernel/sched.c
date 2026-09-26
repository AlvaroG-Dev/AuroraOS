// kernel/sched.c
// Scheduler preemptivo Round-Robin con soporte SMP

#include "sched.h"
#include "cpu.h"
#include "gdt.h"
#include "heap.h"
#include "ipi.h"
#include "klog.h"
#include "panic.h"
#include "serial.h"
#include "spinlock.h"
#include "string.h"
#include <stddef.h>

extern void task_trampoline(void);
extern void user_trampoline(void);

// ---------------------------------------------------------------------------
// Estado per-CPU
// ---------------------------------------------------------------------------
cpu_local_t cpu_local_data[MAX_CPUS] = {{0}};

_Static_assert(offsetof(cpu_local_t, cpu_id) == CPU_LOCAL_CPU_ID_OFFSET,
               "CPU_LOCAL_CPU_ID_OFFSET desincronizado con cpu_local_t");

#define TASK_STACK_SIZE (32 * 1024)
#define SCHED_INTERVAL 10

static spinlock_t sched_lock;
static task_t *task_list_head = NULL;
static task_t *idle_tasks[MAX_CPUS] = {NULL};
static uint32_t next_id = 0;
static uint64_t kernel_cr3 = 0;

static unsigned long sched_lock_irqsave(void) {
  unsigned long flags;
  __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
  while (__sync_lock_test_and_set(&sched_lock.locked, 1)) {
    while (sched_lock.locked) {
      __asm__ volatile("pause");
    }
  }
  return flags;
}

static void sched_unlock_irqrestore(unsigned long flags) {
  __sync_lock_release(&sched_lock.locked);
  __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
}

static void idle_loop(void) {
  while (1) {
    task_t *cur = sched_current();
    if (cur && __atomic_exchange_n((int *)&cur->need_resched, 0, __ATOMIC_ACQ_REL)) {
      sched_yield();
    }
    __asm__ volatile("sti; hlt");
  }
}

// ---------------------------------------------------------------------------
// SMP: Fase 0/3/4
// ---------------------------------------------------------------------------
void smp_init(void) {
  for (int i = 0; i < MAX_CPUS; i++) {
    cpu_local_data[i].cpu_id = i;
    if (i > 0) {
      cpu_local_data[i].kernel_stack = 0;
      cpu_local_data[i].user_rsp = 0;
      cpu_local_data[i].lapic_id = 0;
      cpu_local_data[i].current_task = NULL;
      cpu_local_data[i].ticks_since_resched = 0;
    }
  }

  wrmsr(MSR_GS_BASE, (uint64_t)&cpu_local_data[0]);
  wrmsr(MSR_KERNEL_GS_BASE, 0);

  LOG_DEBUG("[SMP] cpu_local_data[0] inicializado, %%gs -> %p",
            (void *)&cpu_local_data[0]);
}

void smp_set_bsp_lapic_id(uint32_t lapic_id) {
  cpu_local_data[0].lapic_id = lapic_id;
}

void smp_dump(void) {
  LOG_DEBUG("[SMP] Estado per-CPU:");
  for (int i = 0; i < MAX_CPUS; i++) {
    cpu_local_t *c = &cpu_local_data[i];
    LOG_DEBUG("  cpu[%d]: cpu_id=%d lapic_id=%d kernel_stack=%p "
              "user_rsp=%p current_task=%p ticks_since_resched=%lu",
              i, c->cpu_id, c->lapic_id, (void *)c->kernel_stack,
              (void *)c->user_rsp, c->current_task,
              (unsigned long)c->ticks_since_resched);
  }
}

// ---------------------------------------------------------------------------
// Refcounting y Preemption
// ---------------------------------------------------------------------------
void task_get(task_t *t) {
  if (t) {
    __sync_fetch_and_add(&t->refcount, 1);
  }
}

void task_put(task_t *t) {
  if (!t)
    return;
  if (__sync_sub_and_fetch(&t->refcount, 1) == 0) {
    if (t->stack) {
      kfree(t->stack);
      t->stack = NULL;
    }
    kfree(t);
  }
}

void preempt_disable(void) {
  task_t *cur = sched_current();
  if (cur) {
    cur->preempt_count++;
  }
  __asm__ volatile("" ::: "memory");
}

void preempt_enable(void) {
  task_t *cur = sched_current();
  if (cur && cur->preempt_count > 0) {
    cur->preempt_count--;
  }
  __asm__ volatile("" ::: "memory");
}

int preempt_count(void) {
  task_t *cur = sched_current();
  return cur ? cur->preempt_count : 0;
}

// ---------------------------------------------------------------------------
// Inicialización común de tarea
//   - id atómico (se llama desde cualquier CPU, p. ej. spawn en una syscall)
//   - inicializa la entrada de wait queue embebida
// ---------------------------------------------------------------------------
static void task_init_common(task_t *task, uint64_t *sp, uint64_t cr3) {
  task->rsp = (uint64_t)sp;
  task->id = __sync_fetch_and_add(&next_id, 1);
  task->state = TASK_READY;
  task->cr3 = cr3;
  task->is_idle = 0;
  task->proc = NULL;
  task->waiting_on = NULL;
  task->wake_reason = 0;
  task->preempt_count = 0;
  task->next = task;
  task->refcount = 1;
  task->need_resched = 0;
  task->cpu_affinity = -1;
  task->on_cpu = 0;

  // [FIX timeout] Sin timeout por defecto.
  task->wake_deadline = 0;

  // [FIX sched_yield] Sin petición de yield pendiente.
  task->yield_requested = 0;

  task->wait_entry.task = NULL;
  task->wait_entry.next = NULL;

  task->mailbox.head = 0;
  task->mailbox.tail = 0;
  task->mailbox.count = 0;
  wait_queue_init(&task->mailbox.wq);

  task->fpu_state = ((uint64_t)task->fpu_raw + 15) & ~0xFULL;
  __asm__ volatile("fninit; fxsave64 (%0)" : : "r"(task->fpu_state) : "memory");
}

// ---------------------------------------------------------------------------
// Insertar una tarea en la lista circular (con spinlock)
// ---------------------------------------------------------------------------
static void task_list_insert(task_t *task) {
  unsigned long flags = spin_lock_irqsave(&sched_lock);

  if (!task_list_head) {
    task_list_head = task;
    task->next = task;
  } else {
    task_t *tail = task_list_head;
    while (tail->next != task_list_head)
      tail = tail->next;
    task->next = task_list_head;
    tail->next = task;
  }

  spin_unlock_irqrestore(&sched_lock, flags);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void sched_init(void) {
  __asm__ volatile("mov %%cr3, %0" : "=r"(kernel_cr3));
  spin_init(&sched_lock);

  // Crear una tarea idle para cada CPU
  for (int i = 0; i < MAX_CPUS; i++) {
    task_t *idle = (task_t *)kmalloc(sizeof(task_t));
    if (!idle) {
      LOG_PANIC("[SCHED] Fallo al crear idle task %d", i);
    }
    uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!stack) {
      LOG_PANIC("[SCHED] Fallo al crear stack para idle task %d", i);
    }
    uint64_t *sp =
        (uint64_t *)(((uint64_t)(stack + TASK_STACK_SIZE)) & ~0xFULL);
    *(--sp) = (uint64_t)task_trampoline;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = (uint64_t)idle_loop;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;

    task_init_common(idle, sp, kernel_cr3);
    idle->stack = (uint64_t *)stack;
    idle->is_idle = 1;
    idle->state = TASK_READY;
    idle->next = NULL;
    // La idle de cada CPU solo puede correr en su CPU.
    idle->cpu_affinity = i;
    idle_tasks[i] = idle;
  }

  __atomic_store_n((task_t *volatile *)&this_cpu(current_task), idle_tasks[0], __ATOMIC_RELEASE);

  LOG_INFO("[SCHED] Scheduler + SSE/FPU SMP inicializado (idle tasks creadas)");
}

// ---------------------------------------------------------------------------
// Creación de tareas
// ---------------------------------------------------------------------------
task_t *sched_create_task(void (*fn)(void)) {
  task_t *task = (task_t *)kmalloc(sizeof(task_t));
  if (!task)
    return NULL;

  uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
  if (!stack) {
    kfree(task);
    return NULL;
  }

  uint64_t *sp = (uint64_t *)(((uint64_t)(stack + TASK_STACK_SIZE)) & ~0xFULL);
  *(--sp) = (uint64_t)task_trampoline;
  *(--sp) = 0;
  *(--sp) = 0;
  *(--sp) = (uint64_t)fn;
  *(--sp) = 0;
  *(--sp) = 0;
  *(--sp) = 0;

  task_init_common(task, sp, kernel_cr3);
  task->stack = (uint64_t *)stack;
  task_list_insert(task);

  return task;
}

// ---------------------------------------------------------------------------
// Reap de tareas muertas (llamado con sched_lock cogido)
//
// Solo cambia el nivel de log: LOG_INFO -> LOG_TRACE. Escribir por el
// serial con sched_lock cogido e IRQs off bloquea a TODAS las CPUs que
// esperen el lock durante milisegundos.
// Solo se liberan tareas con on_cpu == 0 (otra CPU ya no las usa).
// ---------------------------------------------------------------------------
static void reap_dead_tasks(void) {
  if (!task_list_head)
    return;

  task_t *cur = sched_current();

  if (task_list_head->next == task_list_head) {
    if (task_list_head->state == TASK_DEAD && !task_list_head->is_idle &&
        task_list_head != cur && __atomic_load_n(&task_list_head->on_cpu, __ATOMIC_ACQUIRE) == 0) {
      task_t *dead = task_list_head;
      task_list_head = NULL;
      LOG_TRACE("[SCHED] Limpiando última tarea zombie ID=%u (refcount=%d)",
                dead->id, dead->refcount);
      task_put(dead);
    }
    return;
  }

  task_t *prev = task_list_head;
  for (int i = 0; i < 32 && task_list_head; i++) {
    task_t *target = prev->next;
    if (!target)
      break;

    if (target->state == TASK_DEAD && !target->is_idle && target != cur &&
        target->on_cpu == 0) {
      if (target->next == target) {
        task_list_head = NULL;
        task_put(target);
        break;
      }
      prev->next = target->next;
      if (target == task_list_head) {
        task_list_head = prev->next;
      }
      LOG_TRACE("[SCHED] Limpiando tarea zombie ID=%u (refcount=%d)",
                target->id, target->refcount);
      task_put(target);
    } else {
      prev = prev->next;
      if (prev == task_list_head)
        break;
    }
  }
}

// ---------------------------------------------------------------------------
// Invariantes (+ nuevo: una tarea que se va a ejecutar no puede seguir
// ejecutándose en otra CPU)
// ---------------------------------------------------------------------------
static void sched_check_invariants(task_t *t) {
  if (!t)
    return;
  if (t->state == TASK_BLOCKED && t->waiting_on == NULL) {
    LOG_PANIC("sched: task %u BLOCKED sin waiting_on", t->id);
  }
  if (t->state == TASK_READY && t->waiting_on != NULL) {
    LOG_PANIC("sched: task %u READY con waiting_on != NULL", t->id);
  }
  if (t->state == TASK_RUNNING && t->waiting_on != NULL) {
    LOG_PANIC("sched: task %u RUNNING con waiting_on != NULL", t->id);
  }
  if (t->refcount <= 0) {
    LOG_PANIC("sched: task %u con refcount=%d invalido", t->id, t->refcount);
  }
  if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) {
    LOG_PANIC("sched: task %u elegida con on_cpu=1 (sigue en otra CPU)", t->id);
  }
}

// ---------------------------------------------------------------------------
// Tick
//
//  1) El selector ignora tareas con on_cpu == 1. Antes, una tarea que se
//     estaba durmiendo en la CPU A (ya BLOCKED, aún ejecutando en su pila)
//     podía ser marcada READY por un waker en la CPU B y elegida por la CPU
//     C con su rsp antiguo: dos CPUs sobre la misma pila de kernel.
//  2) Si la tarea actual fue despertada mientras aún corría (state == READY)
//     y no hay otra que ejecutar, se normaliza a RUNNING y sigue: no se
//     pierde el wake-up y no hace falta dormirla.
//  3) [FIX sched_yield] Si la tarea actual pidió ceder explícitamente
//     (yield_requested) y no hay otra READY, se cede al idle del CPU.
//     Sin esto, sched_yield no cede cuando todas las demás tareas están
//     en otros CPUs, y la tarea actual se queda en un busy-loop.
// ---------------------------------------------------------------------------
void sched_tick(void) {
  task_t *curr = (task_t *)this_cpu(current_task);
  if (!curr)
    return;

  this_cpu(ticks_since_resched)++;

  // [C3] Antes: read + write separados. Un sched_kick_idle_cpu() en otra
  // CPU podía escribir need_resched=1 en la ventana entre el read y el
  // write, y la petición se perdía. xchg es atómico y captura el valor
  // anterior.
  //
  // need_resched es 'volatile int' en task_t; casteamos para usar el
  // builtin atómico (el volatile no aporta nada aquí, todas las
  // escrituras ya pasan por xchg o por escrituras locales de la propia
  // CPU).
  int force =
      __atomic_exchange_n((int *)&curr->need_resched, 0, __ATOMIC_ACQ_REL);

  if (!force && this_cpu(ticks_since_resched) < SCHED_INTERVAL)
    return;
  this_cpu(ticks_since_resched) = 0;

  if (curr->preempt_count > 0) {
    if (force)
      __atomic_store_n((int *)&curr->need_resched, 1, __ATOMIC_RELEASE);
    return;
  }

  unsigned long flags = sched_lock_irqsave();

  reap_dead_tasks();

  int cpu = smp_processor_id();

  task_t *next = NULL;
  if (task_list_head) {
    task_t *p = task_list_head;
    task_t *start = p;
    do {
      if (p->state == TASK_READY && !p->is_idle &&
          __atomic_load_n(&p->on_cpu, __ATOMIC_ACQUIRE) == 0 &&
          (p->cpu_affinity < 0 || p->cpu_affinity == cpu)) {
        next = p;
        task_list_head = p->next;
        break;
      }
      p = p->next;
    } while (p && p != start);
  }

  if (!next) {
    if (curr->is_idle) {
      // El idle no tiene a quién ceder. Limpiar yield_requested por si
      // acaso y volver a hlt.
      curr->yield_requested = 0;
      sched_unlock_irqrestore(flags);
      return;
    }
    if (curr->state == TASK_BLOCKED || curr->state == TASK_DEAD) {
      next = idle_tasks[cpu];
    } else if (curr->yield_requested) {
      // [FIX sched_yield] La tarea actual pidió ceder explícitamente.
      // Aunque siga RUNNING y ejecutable, saltamos al idle para dar
      // oportunidad a otras CPUs/tareas y evitar el busy-loop.
      curr->yield_requested = 0;
      next = idle_tasks[cpu];
    } else {
      // curr sigue siendo ejecutable. Si state == READY es que nos
      // despertaron en la ventana entre "BLOCKED" y este punto.
      if (curr->state == TASK_READY)
        curr->state = TASK_RUNNING;
      sched_unlock_irqrestore(flags);
      return;
    }
  } else {
    // [FIX sched_yield] Se encontró otra tarea. Limpiar la petición
    // de yield de la actual: ya se va a ceder de todos modos.
    curr->yield_requested = 0;
  }

  if (next == curr) {
    if (curr->state == TASK_READY)
      curr->state = TASK_RUNNING;
    curr->yield_requested = 0;
    sched_unlock_irqrestore(flags);
    return;
  }

  sched_check_invariants(next);

  task_t *old = curr;
  if (old->state == TASK_RUNNING && !old->is_idle) {
    old->state = TASK_READY;
  }
  next->state = TASK_RUNNING;
  next->yield_requested = 0;
  __atomic_store_n((task_t *volatile *)&this_cpu(current_task), next, __ATOMIC_RELEASE);

  // on_cpu lo actualiza task_switch (asm) justo después de cargar el stack
  // de la nueva tarea (old->on_cpu = 0, new->on_cpu = 1). No tocarlo aquí.

  if (next->stack) {
    uint64_t kstack = (uint64_t)((uint8_t *)next->stack + TASK_STACK_SIZE);
    tss_set_rsp0(kstack);
    this_cpu(kernel_stack) = kstack;
  }

  task_switch(old, next, &sched_lock);

  // Restaurar los flags de la tarea que retoma el control.
  //
  // task_switch libera sched_lock por su cuenta (en asm, mov dword [rdx], 0),
  // pero NO toca RFLAGS. Cada tarea guardó sus flags en su stack cuando
  // llamó a sched_lock_irqsave, y los restaura aquí al volver de task_switch.
  //
  // SIN ESTO, IF queda en 0 tras el primer context switch y el sistema
  // deja de recibir interrupciones (timer, teclado, mouse). El sistema
  // parece funcionar 1-5 segundos y luego se cuelga.
  __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
}

void sched_yield(void) {
  // [FIX sched_yield] Marcar la petición explícita de ceder antes de
  // entrar al tick. sched_tick la leerá para saber que, aunque la
  // tarea actual siga en TASK_RUNNING, debe saltar al idle si no hay
  // otra READY.
  //
  // Sin esto, sched_yield no cede cuando todas las demás tareas han
  // migrado a otros CPUs (sched_kick_idle_cpu salta al BSP), y la
  // tarea actual se queda en un busy-loop del BSP a 100% CPU.
  task_t *cur = sched_current();
  if (cur)
    cur->yield_requested = 1;
  this_cpu(ticks_since_resched) = SCHED_INTERVAL;
  sched_tick();
}

task_t *sched_current(void) { return (task_t *)this_cpu(current_task); }

// ---------------------------------------------------------------------------
// [SMP 4.4] Marcar need_resched en la tarea actual.
// ---------------------------------------------------------------------------
void sched_mark_need_resched(void) {
  task_t *cur = sched_current();
  if (cur) {
    __atomic_store_n((int *)&cur->need_resched, 1, __ATOMIC_RELEASE);
  }
}

// [C4] Publicación atómica de (state=BLOCKED, wake_deadline) bajo
// sched_lock. Antes se hacía en wait.c bajo wq->lock, con el lector
// (sched_wake_expired) leyendo bajo sched_lock. En x86 TSO la race
// era benigna, pero hacerla explícita evita que un cambio futuro de
// orden de escrituras introduzca un bug sutil.
void sched_set_blocked_deadline(task_t *t, uint64_t deadline) {
  unsigned long flags = spin_lock_irqsave(&sched_lock);
  t->state = TASK_BLOCKED;
  t->wake_deadline = deadline;
  spin_unlock_irqrestore(&sched_lock, flags);
}

// ===========================================================================
// Lectura atómica del current_task de otra CPU.
//
// El campo se escribe desde la propia CPU en sched_tick/sched_start/
// sched_start_ap. En x86-64 una escritura de 8 bytes alineados es atómica,
// pero necesitamos la barrera de adquisición para que un lector en otra
// CPU observe el task_t completo antes de tocar sus campos (state,
// is_idle, need_resched).
// ===========================================================================
static inline task_t *sched_read_current(int cpu) {
  if (cpu < 0 || cpu >= MAX_CPUS)
    return NULL;
  return (task_t *)__atomic_load_n(
      (task_t *volatile *)&cpu_local_data[cpu].current_task, __ATOMIC_ACQUIRE);
}

// ===========================================================================
// [HARDENING] Validar que un task_t vive dentro del rango del SLAB.
//
// Las task_t se allocan con kmalloc(sizeof(task_t)) en sched_init y
// sched_create_task. sizeof(task_t) > 512 B (por fpu_raw), así que caen
// en la cache slab-1024. Cualquier puntero fuera de [SLAB_VMA, slab_top)
// indica lectura stale de current_task o corrupción.
//
// La validación NO arregla el race si existe, pero convierte una escritura
// silenciosa a memoria arbitraria en un LOG_WARN visible.
// ===========================================================================
static int sched_task_ptr_valid(const task_t *t) {
  if (!t)
    return 0;
  extern uint64_t slab_vma_start(void);
  extern uint64_t slab_vma_end(void);
  uint64_t lo = slab_vma_start();
  uint64_t hi = slab_vma_end();
  if (lo == 0 || hi <= lo)
    return 1; // SLAB no inicializado todavía, aceptar
  uintptr_t p = (uintptr_t)t;
  return p >= (uintptr_t)lo && p < (uintptr_t)hi;
}

// ---------------------------------------------------------------------------
// [SMP 4.4] Despierta (IPI) a una CPU idle para que ejecute 't'.
//
// [FIX] Todas las lecturas de per_cpu(current_task, cpu) usan
// sched_read_current() (carga atómica con barrera de adquisición) y
// validan el puntero antes de escribir. Además, se toma sched_lock
// durante TODO el bucle, no solo durante la parte final, para que
// task_switch no cambie current_task en medio.
// ---------------------------------------------------------------------------
static void sched_kick_idle_cpu(task_t *t) {
  int me = smp_processor_id();

  int target_cpu = -1;
  uint32_t dest_lapic = 0;
  int send_ipi = 0;

  unsigned long flags = spin_lock_irqsave(&sched_lock);

  // --- Caso 1: t tiene afinidad fija ---
  if (t->cpu_affinity >= 0) {
    int cpu = t->cpu_affinity;

    if (cpu == me) {
      task_t *cur = sched_read_current(me);
      if (cur && cur->is_idle && sched_task_ptr_valid(cur))
        __atomic_store_n((int *)&cur->need_resched, 1, __ATOMIC_RELEASE);
    } else if (cpu >= 0 && cpu < MAX_CPUS) {
      task_t *cur = sched_read_current(cpu);
      if (cur && cur->is_idle && sched_task_ptr_valid(cur)) {
        cur->need_resched = 1;
        target_cpu = cpu;
        dest_lapic = per_cpu(lapic_id, cpu);

        if (dest_lapic != per_cpu(lapic_id, me))
          send_ipi = 1;
      } else if (cur && !sched_task_ptr_valid(cur)) {
        LOG_WARN("[SCHED] kick_idle: current_task[%d]=%p fuera de SLAB", cpu,
                 (void *)cur);
      }
    }

    spin_unlock_irqrestore(&sched_lock, flags);

    if (send_ipi)
      ipi_send(dest_lapic, IPI_VECTOR_RESCHED);

    return;
  }

  // --- Caso 2: sin afinidad, buscar cualquier CPU idle ---
  static volatile int next_cpu = 0;
  int start = __sync_fetch_and_add(&next_cpu, 1) % MAX_CPUS;

  for (int i = 0; i < MAX_CPUS; i++) {
    int cpu = (start + i) % MAX_CPUS;
    if (cpu == me)
      continue;

    task_t *cur = sched_read_current(cpu);
    if (!cur || !cur->is_idle)
      continue;

    if (!sched_task_ptr_valid(cur)) {
      LOG_WARN("[SCHED] kick_idle: current_task[%d]=%p fuera de SLAB", cpu,
               (void *)cur);
      continue;
    }

    __atomic_store_n((int *)&cur->need_resched, 1, __ATOMIC_RELEASE);
    target_cpu = cpu;
    dest_lapic = per_cpu(lapic_id, cpu);

    if (dest_lapic != per_cpu(lapic_id, me))
      send_ipi = 1;
    break;
  }

  // Si el target resultó ser el propio CPU (o no encontramos idle), marcar
  // la idle local para que el siguiente tick haga el switch.
  if (!send_ipi) {
    task_t *me_task = sched_read_current(me);
    if (me_task && me_task->is_idle && sched_task_ptr_valid(me_task))
      __atomic_store_n((int *)&me_task->need_resched, 1, __ATOMIC_RELEASE);
  }

  spin_unlock_irqrestore(&sched_lock, flags);

  if (send_ipi)
    ipi_send(dest_lapic, IPI_VECTOR_RESCHED);
}

// ---------------------------------------------------------------------------
// [SMP 4.4] sched_make_ready: solo la transición BLOCKED -> READY (bajo
// sched_lock) y el aviso a una CPU idle si la tarea estaba dormida.
// No resucita tareas DEAD ni RUNNING.
// ---------------------------------------------------------------------------
void sched_make_ready(task_t *t) {
  if (!t)
    return;

  unsigned long flags = spin_lock_irqsave(&sched_lock);
  int was_blocked = (t->state == TASK_BLOCKED);
  if (t->state == TASK_BLOCKED || t->state == TASK_READY) {
    t->state = TASK_READY;
  }
  spin_unlock_irqrestore(&sched_lock, flags);

  if (was_blocked)
    sched_kick_idle_cpu(t);
}

// ---------------------------------------------------------------------------
// Tareas de usuario
// ---------------------------------------------------------------------------
task_t *sched_create_user_task_stopped(void (*fn)(void),
                                       uint64_t user_stack_top, uint64_t cr3) {
  task_t *task = (task_t *)kmalloc(sizeof(task_t));
  if (!task)
    return NULL;

  uint8_t *kstack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
  if (!kstack) {
    kfree(task);
    return NULL;
  }

  uint64_t *sp = (uint64_t *)(((uint64_t)(kstack + TASK_STACK_SIZE)) & ~0xFULL);

  *(--sp) = USER_DS_RING3;
  *(--sp) = user_stack_top;
  *(--sp) = 0x202;
  *(--sp) = USER_CS_RING3;
  *(--sp) = (uint64_t)fn;
  *(--sp) = (uint64_t)user_trampoline;

  *(--sp) = 0;
  *(--sp) = 0;
  *(--sp) = 0;
  *(--sp) = 0;
  *(--sp) = 0;
  *(--sp) = 0;

  task_init_common(task, sp, cr3);
  task->stack = (uint64_t *)kstack;
  // NO se inserta en la runqueue. El caller hará sched_make_ready.
  return task;
}

// ---------------------------------------------------------------------------
// Búsqueda
// ---------------------------------------------------------------------------
task_t *sched_find_task(uint32_t task_id) {
  unsigned long flags = spin_lock_irqsave(&sched_lock);
  if (!task_list_head) {
    spin_unlock_irqrestore(&sched_lock, flags);
    return NULL;
  }
  task_t *curr = task_list_head;
  do {
    if (curr->id == task_id && curr->state != TASK_DEAD) {
      spin_unlock_irqrestore(&sched_lock, flags);
      return curr;
    }
    curr = curr->next;
  } while (curr && curr != task_list_head);

  spin_unlock_irqrestore(&sched_lock, flags);
  return NULL;
}

/*
 * Busca una tarea y adquiere una referencia mientras sched_lock sigue
 * protegido. Esto evita que el reaper pueda liberar el task entre la
 * búsqueda y el primer acceso del llamante.
 *
 * El llamante debe hacer task_put() cuando termine de usar la referencia.
 */
task_t *sched_find_task_get(uint32_t task_id) {
  unsigned long flags = spin_lock_irqsave(&sched_lock);
  if (!task_list_head) {
    spin_unlock_irqrestore(&sched_lock, flags);
    return NULL;
  }

  task_t *curr = task_list_head;
  do {
    if (curr->id == task_id && curr->state != TASK_DEAD) {
      task_get(curr);
      spin_unlock_irqrestore(&sched_lock, flags);
      return curr;
    }
    curr = curr->next;
  } while (curr && curr != task_list_head);

  spin_unlock_irqrestore(&sched_lock, flags);
  return NULL;
}

// ---------------------------------------------------------------------------
// task_die_hlt
//
// Se llama desde el handler de #PF cuando matamos la tarea actual. La
// tarea está marcada TASK_DEAD pero sigue corriendo en esta CPU. Debemos
// ceder para que el scheduler (en cualquier CPU) pueda reapearla.
// ---------------------------------------------------------------------------
void task_die_hlt(void) {
  while (1) {
    sched_yield();
    __asm__ volatile("sti; hlt");
  }
}

// ---------------------------------------------------------------------------
// sched_start (BSP)
// ---------------------------------------------------------------------------
extern void task_jump_to(task_t *task);

__attribute__((noreturn)) void sched_start(task_t *task) {
  if (!task) {
    LOG_PANIC("[SCHED] sched_start: task == NULL");
  }
  if (task->state != TASK_READY) {
    LOG_PANIC("[SCHED] sched_start: task %u no está READY (state=%d)", task->id,
              task->state);
  }

  if (task->stack) {
    uint64_t kstack = (uint64_t)((uint8_t *)task->stack + TASK_STACK_SIZE);
    tss_set_rsp0(kstack);
    this_cpu(kernel_stack) = kstack;
  }

  task->state = TASK_RUNNING;
  task->on_cpu = 1;
__atomic_store_n((task_t *volatile *)&this_cpu(current_task), task, __ATOMIC_RELEASE);

  task_jump_to(task);
  __builtin_unreachable();
}

// ---------------------------------------------------------------------------
// sched_start_ap (AP)
// ---------------------------------------------------------------------------
__attribute__((noreturn)) void sched_start_ap(void) {
  int cpu = smp_processor_id();
  task_t *idle = idle_tasks[cpu];
  if (!idle) {
    LOG_PANIC("[SCHED] sched_start_ap: idle_task[%d] == NULL", cpu);
  }

  idle->state = TASK_RUNNING;
  idle->on_cpu = 1;
__atomic_store_n((task_t *volatile *)&this_cpu(current_task), idle, __ATOMIC_RELEASE);

  uint64_t kstack = (uint64_t)((uint8_t *)idle->stack + TASK_STACK_SIZE);
  tss_set_rsp0(kstack);
  this_cpu(kernel_stack) = kstack;

  LOG_INFO("[AP] CPU %d inició scheduler SMP (idle task ID=%u)", cpu, idle->id);

  task_jump_to(idle);
  __builtin_unreachable();
}

void task_entry_wrapper(void (*fn)(void)) {
  fn();
  __asm__ volatile("cli");
  task_t *cur = sched_current();
  if (cur) {
    cur->state = TASK_DEAD;
  }
  sched_yield();
  while (1) {
    __asm__ volatile("hlt");
  }
}

// ---------------------------------------------------------------------------
// [Fase A] Publica una tarea recién creada (sched_create_*_stopped) en la
// runqueue y avisa a una CPU idle. Ahora sí despierta a alguien.
// ---------------------------------------------------------------------------
void sched_publish_task(task_t *t) {
  if (!t)
    return;
  task_list_insert(t);
  sched_kick_idle_cpu(t);
}

extern volatile uint64_t tick_count;
uint64_t sched_get_ticks(void) { return tick_count; }

// ---------------------------------------------------------------------------
// [FIX] Recorre la lista de tareas y despierta las que estén BLOCKED y
// hayan pasado su wake_deadline. Se llama desde time_tick (a 1000 Hz)
// en el BSP.
//
// IMPORTANTE: no basta con cambiar state a READY. La tarea sigue en la
// wait queue, y el invariante "state == READY implica waiting_on == NULL"
// se viola, causando un panic en sched_check_invariants.
//
// La solución es llamar a wake_up_all sobre la wq, que quita la tarea
// de la cola Y la marca READY de forma atómica. Si varias tareas
// comparten la misma wq, sólo llamamos a wake_up_all una vez.
//
// El wake_up_all se hace FUERA de sched_lock para evitar deadlocks con
// el handler de IRQ del disco (que toma wq->lock y luego sched_lock).
// ---------------------------------------------------------------------------
void sched_wake_expired(void) {
  uint64_t now = sched_get_ticks();

  /*
   * No usamos un array fijo en la pila.
   *
   * Antes había 16 entradas y, si expiraban más de 16 wait queues distintas
   * en el mismo tick, las tareas restantes perdían su wake_deadline sin que
   * su cola llegara a despertarse. Eso podía dejarlas bloqueadas para siempre.
   *
   * Tampoco basta con cambiar 16 por 1200: 1200 punteros son ~9.6 KiB,
   * más que TASK_STACK_SIZE (8 KiB), y seguiría siendo un límite arbitrario.
   *
   * Construimos la lista de wait queues en el heap. La capacidad inicial es
   * el número de tareas existente bajo sched_lock, que es una cota superior
   * al número de wait queues distintas. Si se crean tareas concurrentemente
   * antes de la segunda pasada, detectamos overflow y reintentamos con una
   * capacidad mayor.
   *
   * Importante: recopilamos cada wait queue como máximo una vez por llamada.
   * Una tarea despertada puede volver a bloquearse antes de que termine esta
   * función; si se volviera a escanear sin recordar las colas ya procesadas,
   * podría despertarse repetidamente en el mismo tick por un deadline
   * absoluto que sigue vencido.
   */

  size_t capacity = 0;

  while (1) {
    unsigned long flags = spin_lock_irqsave(&sched_lock);

    size_t task_count = 0;
    if (task_list_head) {
      task_t *p = task_list_head;
      task_t *start_task = p;
      do {
        task_count++;
        p = p->next;
      } while (p && p != start_task);
    }

    spin_unlock_irqrestore(&sched_lock, flags);

    if (task_count == 0)
      return;

    if (capacity < task_count)
      capacity = task_count;

    wait_queue_t **wqs = (wait_queue_t **)kmalloc(capacity * sizeof(*wqs));
    if (!wqs) {
      /*
       * No podemos construir una lista completa. No consumimos ningún
       * deadline aquí; se reintentará en el siguiente tick. Es preferible
       * retrasar un timeout que perderlo definitivamente.
       */
      LOG_WARN("[SCHED] Sin memoria para procesar timeouts (%zu tareas)",
               task_count);
      return;
    }

    size_t n_wqs = 0;
    int overflow = 0;

    flags = spin_lock_irqsave(&sched_lock);

    if (task_list_head) {
      task_t *p = task_list_head;
      task_t *start_task = p;

      do {
        if (p->state == TASK_BLOCKED && p->wake_deadline > 0 &&
            now >= p->wake_deadline && p->waiting_on != NULL) {
          wait_queue_t *wq = p->waiting_on;

          int dup = 0;
          for (size_t i = 0; i < n_wqs; i++) {
            if (wqs[i] == wq) {
              dup = 1;
              break;
            }
          }

          if (!dup) {
            if (n_wqs >= capacity) {
              overflow = 1;
              break;
            }
            wqs[n_wqs++] = wq;
          }
        }

        p = p->next;
      } while (p && p != start_task);
    }

    if (!overflow) {
      /*
       * Ahora que sabemos que todas las wait queues caben en el array,
       * consumimos los deadlines bajo el mismo sched_lock. Así un overflow
       * nunca puede dejar una tarea sin deadline y sin wakeup.
       */
      if (task_list_head) {
        task_t *p = task_list_head;
        task_t *start_task = p;
        do {
          if (p->state == TASK_BLOCKED && p->wake_deadline > 0 &&
              now >= p->wake_deadline && p->waiting_on != NULL) {
            for (size_t i = 0; i < n_wqs; i++) {
              if (wqs[i] == p->waiting_on) {
                p->wake_deadline = 0;
                break;
              }
            }
          }
          p = p->next;
        } while (p && p != start_task);
      }
    }

    spin_unlock_irqrestore(&sched_lock, flags);

    if (overflow) {
      kfree(wqs);
      /*
       * Puede haber entrado una tarea nueva entre las dos pasadas.
       * Aumentamos la capacidad y repetimos sin modificar deadlines.
       */
      capacity *= 2;
      if (capacity < task_count + 1)
        capacity = task_count + 1;
      continue;
    }

    for (size_t i = 0; i < n_wqs; i++)
      wake_up_all(wqs[i]);

    kfree(wqs);
    return;
  }
}

void __sched_canary_arm(void) {
  task_t *me = sched_current();
  if (!me || !me->stack)
    return;
  uint8_t *base = (uint8_t *)me->stack;
  for (int i = 0; i < 128; i++)
    base[i] = 0xAA;
}

void __sched_canary_check(void) {
  task_t *me = sched_current();
  if (!me || !me->stack)
    return;
  uint8_t *base = (uint8_t *)me->stack;
  for (int i = 0; i < 128; i++) {
    if (base[i] != 0xAA) {
      LOG_PANIC("[CANARY] stack task %u roto en byte %d (0x%02x)", me->id, i,
                base[i]);
    }
  }
}