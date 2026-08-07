// kernel/sched.h
#pragma once
#include <stdint.h>

typedef enum {
  TASK_READY = 0,
  TASK_RUNNING = 1,
  TASK_BLOCKED = 2,
  TASK_DEAD = 3,
} task_state_t;

// Process Control Block
typedef struct task {
  uint64_t rsp;        // Offset 0x00 (0 bytes)
  uint64_t *stack;     // Offset 0x08 (8 bytes)
  uint32_t id;         // Offset 0x10 (16 bytes)
  task_state_t state;  // Offset 0x14 (20 bytes)
  uint64_t fpu_state;  // Offset 0x18 (24 bytes): Puntero al buffer FPU alineado
  
  uint8_t fpu_raw[512 + 16]; // Buffer crudo con 16 bytes extra para margen de alineación
  struct task *next;   // Lista circular
} task_t;

void sched_init(void);
task_t *sched_create_task(void (*fn)(void));
void sched_tick(void);
void sched_yield(void);
void sched_unblock(task_t *task);
task_t *sched_current(void);

extern void task_switch(task_t *old_task, task_t *new_task);
void task_entry_wrapper(void (*fn)(void));