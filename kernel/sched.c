// kernel/sched.c
#include "sched.h"
#include "gdt.h"
#include "heap.h"
#include "serial.h"
#include <stddef.h>

extern void task_trampoline(void);

#define TASK_STACK_SIZE (8 * 1024)
#define SCHED_INTERVAL 10

static task_t *current_task = NULL;
static task_t *task_list_head = NULL;
static uint32_t next_id = 0;
static uint32_t tick_counter = 0;

void task_entry_wrapper(void (*fn)(void)) {
  fn();
  current_task->state = TASK_DEAD;
  sched_yield(); // Ceder inmediatamente la CPU
  while (1) { __asm__ volatile("hlt"); }
}

void sched_init(void) {
  task_t *idle = (task_t *)kmalloc(sizeof(task_t));
  if (!idle) return;

  idle->id = next_id++;
  idle->state = TASK_RUNNING;
  idle->stack = NULL;
  idle->next = idle;
  idle->rsp = 0;

  // Calcular dirección alineada a 16 bytes dentro del buffer fpu_raw
  idle->fpu_state = ((uint64_t)idle->fpu_raw + 15) & ~0xFULL;
  
  // Guardar estado FPU/SSE limpio por defecto
  __asm__ volatile("fninit; fxsave64 (%0)" : : "r"(idle->fpu_state) : "memory");

  current_task = idle;
  task_list_head = idle;
  serial_puts("[SCHED] Scheduler + SSE/FPU inicializado.\n");
}

task_t *sched_create_task(void (*fn)(void)) {
  task_t *task = (task_t *)kmalloc(sizeof(task_t));
  if (!task) return NULL;

  uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
  if (!stack) {
    kfree(task);
    return NULL;
  }

  uint64_t *sp = (uint64_t *)(((uint64_t)(stack + TASK_STACK_SIZE)) & ~0xFULL);

  *(--sp) = (uint64_t)task_trampoline;
  *(--sp) = (uint64_t)0;               // rbx
  *(--sp) = (uint64_t)0;               // rbp
  *(--sp) = (uint64_t)fn;              // r12
  *(--sp) = (uint64_t)0;               // r13
  *(--sp) = (uint64_t)0;               // r14
  *(--sp) = (uint64_t)0;               // r15

  task->rsp = (uint64_t)sp;
  task->stack = (uint64_t *)stack;
  task->id = next_id++;
  task->state = TASK_READY;

  // Calcular dirección alineada a 16 bytes
  task->fpu_state = ((uint64_t)task->fpu_raw + 15) & ~0xFULL;

  // Inicializar estado FPU/SSE para la nueva tarea
  __asm__ volatile("fninit; fxsave64 (%0)" : : "r"(task->fpu_state) : "memory");

  // Insertar en lista circular
  task_t *tail = task_list_head;
  while (tail->next != task_list_head)
    tail = tail->next;
  tail->next = task;
  task->next = task_list_head;

  return task;
}

// Recolector de basura: Desvincula y libera tareas muertas
static void reap_dead_tasks(void) {
  if (!current_task) return;

  task_t *curr = current_task;
  // Recorrer la lista circular buscando tareas TASK_DEAD
  for (int i = 0; i < 32; i++) {
    task_t *next = curr->next;
    if (next == current_task) break;

    if (next->state == TASK_DEAD) {
      curr->next = next->next; // Desvincular de la lista
      if (next == task_list_head) task_list_head = curr->next;

      serial_puts("[SCHED] Limpiando tarea zombie ID=");
      serial_putn(next->id, 10, 0);
      serial_puts("\n");

      if (next->stack) kfree(next->stack);
      kfree(next);
    } else {
      curr = curr->next;
    }
  }
}

void sched_tick(void) {
  if (!current_task) return;
  tick_counter++;
  if (tick_counter < SCHED_INTERVAL) return;
  tick_counter = 0;

  reap_dead_tasks(); // Limpiar memoria de tareas finalizadas

  task_t *next = current_task->next;
  int max = 64;
  while ((next->state == TASK_DEAD || next->state == TASK_BLOCKED) &&
         next != current_task && max-- > 0) {
    next = next->next;
  }

  if (next == current_task || next->state != TASK_READY) return;

  task_t *old = current_task;
  current_task = next;

  if (next->stack) {
    tss_set_rsp0((uint64_t)((uint8_t *)next->stack + TASK_STACK_SIZE));
  }

  if (old->state == TASK_RUNNING) old->state = TASK_READY;
  next->state = TASK_RUNNING;

  task_switch(old, next);
}

void sched_yield(void) {
  uint64_t flags;
  __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags));
  tick_counter = SCHED_INTERVAL;
  sched_tick();
  __asm__ volatile ("push %0; popfq" : : "r"(flags));
}

void sched_unblock(task_t *task) {
  if (task && task->state == TASK_BLOCKED) {
    task->state = TASK_READY;
  }
}

task_t *sched_current(void) {
  return current_task;
}