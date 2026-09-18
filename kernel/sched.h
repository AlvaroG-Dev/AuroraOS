// kernel/sched.h
#pragma once
#include "wait.h"
#include <stddef.h>
#include <stdint.h>

struct wait_queue;

typedef enum {
  TASK_READY = 0,
  TASK_RUNNING = 1,
  TASK_BLOCKED = 2,
  TASK_DEAD = 3,
} task_state_t;

#define IPC_MAX_PAYLOAD 64
#define IPC_MAILBOX_SIZE 16

typedef struct ipc_msg {
  uint32_t sender;
  uint32_t receiver;
  uint32_t type;
  uint32_t size;
  uint8_t data[IPC_MAX_PAYLOAD];
} ipc_msg_t;

typedef struct ipc_mailbox {
  ipc_msg_t messages[IPC_MAILBOX_SIZE];
  uint8_t head;
  uint8_t tail;
  uint8_t count;
  struct wait_queue wq;
} ipc_mailbox_t;

// Process Control Block
typedef struct task {
  uint64_t rsp;
  uint64_t *stack;
  uint32_t id;
  task_state_t state;
  uint64_t fpu_state;
  uint64_t cr3;
  uint8_t fpu_raw[512 + 16];
  ipc_mailbox_t mailbox;
  int is_idle;
  struct task *next;

  struct wait_queue *waiting_on;
  int wake_reason;

  // Contador de preemption. Si > 0, sched_tick() NO desaloja esta tarea
  // (desalojo involuntario). sched_yield() siempre cede, ignorando el
  // contador, porque es un acto voluntario.
  volatile int preempt_count;

  // Contador de referencias atómico. La tarea se libera cuando llega a 0.
  // Cada sitio que guarda un puntero a la tarea (wq, process_t, etc.)
  // debe incrementar el contador con task_get() y decrementarlo con
  // task_put() cuando lo suelta.
  //
  // La referencia inicial (refcount = 1) la tiene el scheduler al
  // crear la tarea. Se libera en reap_dead_tasks().
  volatile int refcount;
} task_t;

void sched_init(void);
task_t *sched_create_task(void (*fn)(void));
task_t *sched_create_user_task(void (*fn)(void), uint64_t user_stack_top,
                               uint64_t cr3);
void sched_tick(void);
void sched_yield(void);
task_t *sched_current(void);
task_t *sched_find_task(uint32_t task_id);

void sched_make_ready(task_t *t);

// Preemption disable/enable. Se aplican a la tarea ACTUAL.
// Cada disable DEBE tener su enable correspondiente.
void preempt_disable(void);
void preempt_enable(void);
int preempt_count(void);

// Refcount de tareas. Atómicos. task_put libera la tarea cuando
// el contador llega a 0.
void task_get(task_t *t);
void task_put(task_t *t);

extern void task_switch(task_t *old_task, task_t *new_task);
void task_entry_wrapper(void (*fn)(void));
void task_die_hlt(void);

// Cambia a `task` SIN guardar el contexto actual. Se usa una sola vez
// en el arranque para saltar del stack del bootloader a la primera
// tarea real (kmain_task). No retorna.
__attribute__((noreturn)) void sched_start(task_t *task);