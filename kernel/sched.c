// kernel/sched.c
#include "sched.h"
#include "cpu.h"
#include "gdt.h"
#include "heap.h"
#include "klog.h"
#include "paging.h"
#include "panic.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"
#include "wait.h"
#include <stddef.h>

extern void task_trampoline(void);
extern void user_trampoline(void);

// ---------------------------------------------------------------------------
// Estado per-CPU.
//
// Fase 0 de SMP: cpu_local sigue siendo la instancia única usada por todo
// el código. cpu_local_data[] es el array que se usará con SMP real.
// smp_init() inicializa cpu_local_data[0] con los mismos valores que
// cpu_local, para que this_cpu() funcione cuando se use.
//
// TODO SMP (Fase 4): eliminar cpu_local y usar solo cpu_local_data[].
// ---------------------------------------------------------------------------
cpu_local_t cpu_local = {0, 0, 0, 0, NULL, 0};
cpu_local_t cpu_local_data[MAX_CPUS] = {{0}};

#define TASK_STACK_SIZE (8 * 1024)
#define SCHED_INTERVAL 10

static task_t *current_task = NULL;
static task_t *task_list_head = NULL;
static uint32_t next_id = 0;
static uint32_t tick_counter = 0;
static uint64_t kernel_cr3 = 0;

// ---------------------------------------------------------------------------
// Idle task
// ---------------------------------------------------------------------------
static void idle_loop(void) {
  while (1) {
    __asm__ volatile("sti; hlt");
  }
}

// ---------------------------------------------------------------------------
// SMP: Fase 0
// ---------------------------------------------------------------------------
void smp_init(void) {
  // Inicializar la entrada del CPU 0 en cpu_local_data, copiando los
  // valores de cpu_local (que es la instancia "viva" hoy).
  cpu_local_data[0] = cpu_local;
  cpu_local_data[0].cpu_id = 0;
  cpu_local_data[0].lapic_id = 0;
  cpu_local_data[0].current_task = NULL;
  cpu_local_data[0].tick_counter = 0;

  // Las demás entradas quedan a 0.
  for (int i = 1; i < MAX_CPUS; i++) {
    cpu_local_data[i] = (cpu_local_t){0};
    cpu_local_data[i].cpu_id = i;
  }

  LOG_INFO("[SMP] Fase 0: cpu_local_data[%d] inicializado (cpu_id=0)",
           MAX_CPUS);
}

void smp_dump(void) {
  LOG_INFO("[SMP] Estado per-CPU:");
  for (int i = 0; i < MAX_CPUS; i++) {
    cpu_local_t *c = &cpu_local_data[i];
    LOG_INFO("  cpu[%d]: cpu_id=%d lapic_id=%d kernel_stack=%p "
             "user_rsp=%p current_task=%p tick_counter=%lu",
             i, c->cpu_id, c->lapic_id, (void *)c->kernel_stack,
             (void *)c->user_rsp, c->current_task,
             (unsigned long)c->tick_counter);
  }
}

// ---------------------------------------------------------------------------
// Refcount
// ---------------------------------------------------------------------------
void task_get(task_t *t) {
  if (!t)
    return;
  __sync_fetch_and_add(&t->refcount, 1);
}

static void task_free(task_t *t) {
  if (!t)
    return;
  if (t->cr3 != kernel_cr3 && t->cr3 != 0) {
    LOG_DEBUG("[SCHED] Liberando PML4 de usuario en %p", (void *)t->cr3);
    paging_free_user_space(t->cr3);
  }
  if (t->stack)
    kfree(t->stack);
  kfree(t);
}

void task_put(task_t *t) {
  if (!t)
    return;
  int old = __sync_fetch_and_sub(&t->refcount, 1);
  if (old <= 0) {
    LOG_PANIC("sched: task_put underflow en task %u (refcount=%d)", t->id,
              old - 1);
  }
  if (old == 1) {
    task_free(t);
  }
}

// ---------------------------------------------------------------------------
// Preemption
// ---------------------------------------------------------------------------
void preempt_disable(void) {
  task_t *cur = current_task;
  if (cur) {
    cur->preempt_count++;
  }
  __asm__ volatile("" ::: "memory");
}

void preempt_enable(void) {
  task_t *cur = current_task;
  if (cur && cur->preempt_count > 0) {
    cur->preempt_count--;
  }
  __asm__ volatile("" ::: "memory");
}

int preempt_count(void) {
  task_t *cur = current_task;
  return cur ? cur->preempt_count : 0;
}

// ---------------------------------------------------------------------------
// Entry wrapper
// ---------------------------------------------------------------------------
void task_entry_wrapper(void (*fn)(void)) {
  fn();
  __asm__ volatile("cli");
  current_task->state = TASK_DEAD;
  sched_yield();
  while (1)
    __asm__ volatile("hlt");
}

// ---------------------------------------------------------------------------
// Helper interno
// ---------------------------------------------------------------------------
static void task_init_common(task_t *task, uint64_t *stack_ptr, uint64_t cr3) {
  task->rsp = (uint64_t)stack_ptr;
  task->stack = (uint64_t *)stack_ptr;
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
// Insertar una tarea en la lista circular
// ---------------------------------------------------------------------------
static void task_list_insert(task_t *task) {
  uint64_t flags;
  __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags));

  if (!task_list_head) {
    task_list_head = task;
  } else {
    task_t *tail = task_list_head;
    while (tail->next != task_list_head)
      tail = tail->next;
    task->next = task_list_head;
    tail->next = task;
  }

  __asm__ volatile("push %0; popfq" : : "r"(flags));
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void sched_init(void) {
  __asm__ volatile("mov %%cr3, %0" : "=r"(kernel_cr3));

  uint64_t cpu_local_ptr = (uint64_t)&cpu_local;
  wrmsr(MSR_GS_BASE, cpu_local_ptr);
  wrmsr(MSR_KERNEL_GS_BASE, 0);

  task_t *idle = sched_create_task(idle_loop);
  if (!idle) {
    LOG_ERR("[SCHED] Fallo al crear idle task");
    return;
  }
  idle->is_idle = 1;
  idle->state = TASK_READY;
  current_task = idle;

  LOG_INFO("[SCHED] Scheduler + SSE/FPU inicializado (idle task ID=%u)",
           idle->id);
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
// Reap de tareas muertas
// ---------------------------------------------------------------------------
static void reap_dead_tasks(void) {
  if (!current_task)
    return;

  task_t *curr = current_task;
  for (int i = 0; i < 32; i++) {
    task_t *next = curr->next;
    if (next == current_task)
      break;

    if (next->state == TASK_DEAD && !next->is_idle) {
      curr->next = next->next;
      if (next == task_list_head)
        task_list_head = curr->next;

      LOG_INFO("[SCHED] Limpiando tarea zombie ID=%u (refcount=%d)", next->id,
               next->refcount);

      task_put(next);
    } else {
      curr = curr->next;
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
  if (!current_task)
    return;

  tick_counter++;
  if (tick_counter < SCHED_INTERVAL)
    return;
  tick_counter = 0;

  if (current_task->preempt_count > 0)
    return;

  reap_dead_tasks();

  task_t *next = current_task->next;
  int max = 64;
  while ((next->state == TASK_DEAD || next->state == TASK_BLOCKED) &&
         next != current_task && max-- > 0) {
    next = next->next;
  }

  if (next == current_task || next->state != TASK_READY)
    return;

  sched_check_invariants(next);

  task_t *old = current_task;
  current_task = next;

  if (next->stack) {
    uint64_t kstack = (uint64_t)((uint8_t *)next->stack + TASK_STACK_SIZE);
    tss_set_rsp0(kstack);
    cpu_local.kernel_stack = kstack;
  }

  if (old->state == TASK_RUNNING)
    old->state = TASK_READY;
  next->state = TASK_RUNNING;
  LOG_DEBUG("[SCHED] tick: current=%u", current_task->id);
  task_switch(old, next);
}

void sched_yield(void) {
  uint64_t flags;
  __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags));
  tick_counter = SCHED_INTERVAL;
  sched_tick();
  __asm__ volatile("push %0; popfq" : : "r"(flags));
}

task_t *sched_current(void) { return current_task; }

void sched_make_ready(task_t *t) {
  if (!t)
    return;
  if (t->state == TASK_BLOCKED || t->state == TASK_READY) {
    t->state = TASK_READY;
  }
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
  if (!task_list_head)
    return NULL;
  task_t *curr = task_list_head;
  do {
    if (curr->id == task_id && curr->state != TASK_DEAD) {
      return curr;
    }
    curr = curr->next;
  } while (curr && curr != task_list_head);
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
// sched_start
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

  if (current_task && current_task != task) {
    if (current_task->state == TASK_RUNNING) {
      current_task->state = TASK_READY;
    }
  }

  if (task->stack) {
    uint64_t kstack = (uint64_t)((uint8_t *)task->stack + TASK_STACK_SIZE);
    tss_set_rsp0(kstack);
    cpu_local.kernel_stack = kstack;
  }

  task->state = TASK_RUNNING;
  current_task = task;

  task_jump_to(task);
  __builtin_unreachable();
}