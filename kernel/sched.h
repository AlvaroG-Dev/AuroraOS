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

  volatile int preempt_count;

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

void preempt_disable(void);
void preempt_enable(void);
int preempt_count(void);

void task_get(task_t *t);
void task_put(task_t *t);

struct spinlock;
extern void task_switch(task_t *old_task, task_t *new_task, void *lock);
void task_entry_wrapper(void (*fn)(void));
void task_die_hlt(void);

__attribute__((noreturn)) void sched_start(task_t *task);

// ---------------------------------------------------------------------------
// SMP: Fase 0/3.1
// ---------------------------------------------------------------------------
void smp_init(void);
void smp_dump(void);
void sched_start_ap(void);

// Publica el APIC ID del BSP en cpu_local_data[0]. Llamar tras apic_init.
void smp_set_bsp_lapic_id(uint32_t lapic_id);