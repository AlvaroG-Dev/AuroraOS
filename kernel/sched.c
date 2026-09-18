// kernel/sched.c
// Scheduler preemptivo Round-Robin con soporte SMP

#include "sched.h"
#include "cpu.h"
#include "gdt.h"
#include "heap.h"
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

#define TASK_STACK_SIZE (8 * 1024)
#define SCHED_INTERVAL 10

static spinlock_t sched_lock;
static task_t *task_list_head = NULL;
static task_t *idle_tasks[MAX_CPUS] = {NULL};
static uint32_t next_id = 0;
static uint64_t kernel_cr3 = 0;

static void idle_loop(void) {
  while (1) {
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
      cpu_local_data[i].tick_counter = 0;
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
              "user_rsp=%p current_task=%p tick_counter=%lu",
              i, c->cpu_id, c->lapic_id, (void *)c->kernel_stack,
              (void *)c->user_rsp, c->current_task,
              (unsigned long)c->tick_counter);
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
// ---------------------------------------------------------------------------
static void task_init_common(task_t *task, uint64_t *sp, uint64_t cr3) {
  task->rsp = (uint64_t)sp;
  task->id = next_id++;
  task->state = TASK_READY;
  task->cr3 = cr3;
  task->is_idle = 0;
  task->waiting_on = NULL;
  task->wake_reason = 0;
  task->preempt_count = 0;
  task->next = task;
  task->refcount = 1;

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
    uint64_t *sp = (uint64_t *)(((uint64_t)(stack + TASK_STACK_SIZE)) & ~0xFULL);
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
    idle_tasks[i] = idle;
  }

  this_cpu(current_task) = idle_tasks[0];

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
// ---------------------------------------------------------------------------
static void reap_dead_tasks(void) {
  if (!task_list_head)
    return;

  task_t *cur = sched_current();

  // Si solo hay un nodo en la lista circular
  if (task_list_head->next == task_list_head) {
    if (task_list_head->state == TASK_DEAD && !task_list_head->is_idle && task_list_head != cur) {
      task_t *dead = task_list_head;
      task_list_head = NULL;
      LOG_INFO("[SCHED] Limpiando última tarea zombie ID=%u (refcount=%d)", dead->id, dead->refcount);
      task_put(dead);
    }
    return;
  }

  // Lista con 2 o más nodos
  task_t *prev = task_list_head;
  for (int i = 0; i < 32 && task_list_head; i++) {
    task_t *target = prev->next;
    if (!target)
      break;

    if (target->state == TASK_DEAD && !target->is_idle && target != cur) {
      if (target->next == target) {
        task_list_head = NULL;
        task_put(target);
        break;
      }
      prev->next = target->next;
      if (target == task_list_head) {
        task_list_head = prev->next;
      }
      LOG_INFO("[SCHED] Limpiando tarea zombie ID=%u (refcount=%d)", target->id, target->refcount);
      task_put(target);
    } else {
      prev = prev->next;
      if (prev == task_list_head)
        break;
    }
  }
}

// ---------------------------------------------------------------------------
// Invariantes
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
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------
void sched_tick(void) {
  task_t *curr = (task_t *)this_cpu(current_task);
  if (!curr)
    return;

  this_cpu(tick_counter)++;
  if (this_cpu(tick_counter) < SCHED_INTERVAL)
    return;
  this_cpu(tick_counter) = 0;

  if (curr->preempt_count > 0)
    return;

  uint64_t flags;
  __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags));

  while (__sync_lock_test_and_set(&sched_lock.locked, 1)) {
    while (sched_lock.locked) {
      __asm__ volatile("pause");
    }
  }

  reap_dead_tasks();

  int cpu = smp_processor_id();

  // Buscar una tarea en TASK_READY en la lista compartida
  task_t *next = NULL;
  if (task_list_head) {
    task_t *p = task_list_head;
    task_t *start = p;
    do {
      if (p->state == TASK_READY && !p->is_idle) {
        next = p;
        // Rotar task_list_head para round-robin equitativo entre CPUs
        task_list_head = p->next;
        break;
      }
      p = p->next;
    } while (p && p != start);
  }

  // Si no hay tareas listas:
  if (!next) {
    if (curr->is_idle) {
      // Ya estamos en idle de este CPU
      __sync_lock_release(&sched_lock.locked);
      __asm__ volatile("push %0; popfq" : : "r"(flags));
      return;
    }
    // Si la tarea actual murió o se bloqueó, cambiar a idle
    if (curr->state == TASK_BLOCKED || curr->state == TASK_DEAD) {
      next = idle_tasks[cpu];
    } else {
      // La tarea actual sigue RUNNING y no hay competencia
      __sync_lock_release(&sched_lock.locked);
      __asm__ volatile("push %0; popfq" : : "r"(flags));
      return;
    }
  }

  if (next == curr) {
    __sync_lock_release(&sched_lock.locked);
    __asm__ volatile("push %0; popfq" : : "r"(flags));
    return;
  }

  sched_check_invariants(next);

  task_t *old = curr;
  if (old->state == TASK_RUNNING && !old->is_idle) {
    old->state = TASK_READY;
  }
  next->state = TASK_RUNNING;
  this_cpu(current_task) = next;

  if (next->stack) {
    uint64_t kstack = (uint64_t)((uint8_t *)next->stack + TASK_STACK_SIZE);
    tss_set_rsp0(kstack);
    this_cpu(kernel_stack) = kstack;
  }

  // task_switch liberará sched_lock tan pronto como old esté completamente guardado en RAM
  task_switch(old, next, &sched_lock);

  __asm__ volatile("push %0; popfq" : : "r"(flags));
}

void sched_yield(void) {
  this_cpu(tick_counter) = SCHED_INTERVAL;
  sched_tick();
}

task_t *sched_current(void) {
  return (task_t *)this_cpu(current_task);
}

void sched_make_ready(task_t *t) {
  if (!t)
    return;
  unsigned long flags = spin_lock_irqsave(&sched_lock);
  if (t->state == TASK_BLOCKED || t->state == TASK_READY) {
    t->state = TASK_READY;
  }
  spin_unlock_irqrestore(&sched_lock, flags);
}

// ---------------------------------------------------------------------------
// Tareas de usuario
// ---------------------------------------------------------------------------
task_t *sched_create_user_task(void (*fn)(void), uint64_t user_stack_top,
                               uint64_t cr3) {
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
  task_list_insert(task);

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

// ---------------------------------------------------------------------------
// task_die_hlt
// ---------------------------------------------------------------------------
void task_die_hlt(void) {
  while (1) {
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
  this_cpu(current_task) = task;

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
  this_cpu(current_task) = idle;

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
