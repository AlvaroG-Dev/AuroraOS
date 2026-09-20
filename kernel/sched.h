// kernel/sched.h
#pragma once
#include "wait.h"
#include <stddef.h>
#include <stdint.h>

struct wait_queue;
struct process;

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

  // [Fase A] Enlace directo a la estructura de proceso (NULL para
  // tareas de kernel e idle). Evita recorrer process_list en cada
  // syscall y elimina el use-after-free que había.
  struct process *proc;

  struct wait_queue *waiting_on;
  int wake_reason;

  volatile int preempt_count;
  volatile int refcount;

  volatile int need_resched;
  int cpu_affinity;
  volatile int on_cpu;
  wait_queue_entry_t wait_entry;
  // [FIX timeout] Tick absoluto en el que la tarea debe despertar aunque
  // nadie la despierte explícitamente. 0 = sin timeout (espera indefinida).
  // Lo usa wait_event_interruptible_timeout para que el scheduler la
  // despierte al expirar, en vez de quedarse dormida para siempre.
  uint64_t wake_deadline;
} task_t;

void sched_init(void);
task_t *sched_create_task(void (*fn)(void));
task_t *sched_create_user_task(void (*fn)(void), uint64_t user_stack_top,
                               uint64_t cr3);

// [Fase A] Crea una tarea de usuario pero NO la inserta en la runqueue.
// El llamante debe inicializar task->proc y llamar a sched_make_ready(task)
// cuando esté todo listo. Esto evita que un AP ejecute la tarea antes de
// que process_spawn haya terminado de montarla.
task_t *sched_create_user_task_stopped(void (*fn)(void),
                                       uint64_t user_stack_top, uint64_t cr3);

void sched_tick(void);
void sched_yield(void);
task_t *sched_current(void);
task_t *sched_find_task(uint32_t task_id);

// [FIX] Contador monótono de ticks desde el arranque. Lo incrementa el
// handler del LAPIC timer a 1000 Hz. Usado por wait_event_*_timeout.
uint64_t sched_get_ticks(void);

void sched_make_ready(task_t *t);
void sched_mark_need_resched(void);
void sched_publish_task(task_t *t);

void preempt_disable(void);
void preempt_enable(void);
int preempt_count(void);

void task_get(task_t *t);
void task_put(task_t *t);

struct spinlock;
extern void task_switch(task_t *old_task, task_t *new_task, void *lock);
void task_entry_wrapper(void (*fn)(void));
void task_die_hlt(void);

// [FIX timeout] Despierta tareas BLOCKED cuyo wake_deadline haya expirado.
// Lo llama time_tick a 1000 Hz en el BSP.
void sched_wake_expired(void);

__attribute__((noreturn)) void sched_start(task_t *task);

void smp_init(void);
void smp_dump(void);
void sched_start_ap(void);
void smp_set_bsp_lapic_id(uint32_t lapic_id);

// ---------------------------------------------------------------------------
// Asserts de offsets asm↔C.
//
// switch.asm usa:
//   0x00 → rsp
//   0x18 → fpu_state
//   0x20 → cr3
//
// Si cambias el struct task_t, estos asserts te avisan.
// ---------------------------------------------------------------------------
_Static_assert(offsetof(task_t, rsp) == 0x00, "switch.asm: rsp @ 0x00");
_Static_assert(offsetof(task_t, fpu_state) == 0x18, "switch.asm");
_Static_assert(offsetof(task_t, cr3) == 0x20, "switch.asm");
_Static_assert(offsetof(task_t, on_cpu) == 0x78C, "switch.asm TASK_OFF_ON_CPU");
_Static_assert(offsetof(spinlock_t, locked) == 0 &&
                   sizeof(((spinlock_t *)0)->locked) == 4,
               "switch.asm libera el lock con mov dword [rdx], 0");

// FXSAVE64/FXRSTOR64 requieren que la dirección del buffer esté
// alineada a 16 bytes. task->fpu_state se calcula como
//   (uint64_t)task->fpu_raw redondeado hacia arriba a 16.
// Para que esto funcione, fpu_raw debe estar a una distancia tal que
// el redondeo no desborde el buffer.
//
// Verificamos que fpu_raw + 15 (el peor caso del redondeo) siga
// cabiendo dentro de task_t.
_Static_assert(offsetof(task_t, fpu_raw) + sizeof(((task_t *)0)->fpu_raw) +
                       15 <=
                   sizeof(task_t),
               "fpu_raw demasiado cerca del final de task_t");
