// kernel/sched.h
#pragma once
#include "idt.h"
#include "wait.h"
#include <stddef.h>
#include <stdint.h>

struct wait_queue;
struct process;
struct kthread; // [KTHREAD] forward-decl para el back-pointer de task_t

typedef enum {
  TASK_READY = 0,
  TASK_RUNNING = 1,
  TASK_BLOCKED = 2,
  TASK_STOPPED = 3,
  TASK_DEAD = 4,
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

typedef struct task {
  uint64_t rsp;
  uint64_t *stack;
  uint32_t id;
  task_state_t state;
  uint64_t fpu_state;
  uint64_t cr3;
  uint8_t fpu_raw[512 + 16];
  // Buzon de IPC: se crea la primera vez que la tarea envia o recibe (ipc.c)
  // y se libera con la tarea. Antes iba dentro (~1,3 KB en cada task_t).
  ipc_mailbox_t *mailbox;
  int is_idle;
  struct task *next;

  struct process *proc;

  struct wait_queue *waiting_on;
  int wake_reason;

  volatile int preempt_count;
  volatile int refcount;

  volatile int need_resched;
  int cpu_affinity;
  volatile int on_cpu;
  wait_queue_entry_t wait_entry;
  volatile uint64_t wait_seq;
  uint64_t wake_deadline;
  volatile int yield_requested;

  uint64_t fs_base;
  registers_t *syscall_regs;

  uint32_t *clear_child_tid;

  // rseq(2): area registrada por este hilo (0 = ninguna) y contador de
  // conmutaciones (el kernel actualiza cpu_id y aborta la seccion critica
  // en el primer retorno a userland tras una).
  uint64_t rseq_ptr;
  uint32_t rseq_len, rseq_sig;
  uint32_t rseq_sw, rseq_seen;

  // [KTHREAD] Back-pointer al handle cuando esta task fue creada vía
  // kthread_create/run. NULL para tareas nativas del kernel y para
  // procesos de usuario. kthread_stop lo pone a NULL antes de liberar
  // el handle, así que un task_t reapeado nunca deja un puntero vivo.
  struct kthread *kthread;

  // Aviso de wait_queue_wake_task() que llego cuando la tarea aun no estaba
  // en ninguna cola (entre evaluar la condicion y encolarse). wait_common lo
  // consume tras encolarse y no se duerme. Ver wait_queue_wake_task().
  volatile int wake_pending;

  // Senales por hilo (como Linux): la mascara es de cada hilo; las
  // pendientes del proceso estan en process_t y aqui solo las dirigidas a
  // este hilo (tkill/tgkill, SIGEV_THREAD_ID). tid 0 = hilo principal (su
  // tid es el pid del proceso); los demas hilos sacan el suyo del contador
  // de pids (task_tid()).
  uint32_t tid;
  uint64_t sig_blocked;
  uint64_t sig_pending;
  uint64_t sig_saved_mask; // mascara previa a sigsuspend
  uint64_t sig_waiting;    // las que espera en sigtimedwait (si no, 0)
  int sig_suspend_active;
} task_t;

void sched_init(void);
task_t *sched_create_task(void (*fn)(void));

// [KTHREAD] Igual que sched_create_task pero NO inserta la tarea en la
// runqueue. El llamante debe llamar a sched_publish_task(t) cuando el
// resto de la inicialización (incluido t->kthread) haya terminado.
task_t *sched_create_kernel_task_stopped(void (*fn)(void));

task_t *sched_create_user_task_stopped(void (*fn)(void),
                                       uint64_t user_stack_top, uint64_t cr3);

void sched_tick(void);
void sched_yield(void);
task_t *sched_current(void);
task_t *sched_find_task(uint32_t task_id);

// Hilo secundario (tid != 0) con ese tid, con referencia. NULL si no hay.
task_t *sched_find_tid_get(uint32_t tid);
// Hilo de `proc` que debe atender una senal de proceso `sigbit`: `prefer`
// (el principal) si vive y no la bloquea; si no, otro hilo vivo que no la
// bloquee. Con referencia; NULL si todos la bloquean.
task_t *sched_signal_target_get(struct process *proc, task_t *prefer,
                                uint64_t sigbit);
// Un hilo vivo de `proc` distinto de `except` que aun no tenga `sigbit` en
// sus pendientes propias, con referencia (para recorrerlos marcandolos).
task_t *sched_next_thread_get(struct process *proc, task_t *except,
                              uint64_t sigbit);

uint64_t sched_get_ticks(void);

void sched_make_ready(task_t *t);
void sched_mark_need_resched(void);
void sched_publish_task(task_t *t);

void sched_stop_task(task_t *t);
void sched_cont_task(task_t *t);

task_t *sched_create_forked_user_task(registers_t *parent_regs, uint64_t cr3);

void sched_set_blocked_deadline(task_t *t, uint64_t deadline);

task_t *sched_create_forked_user_task_ex(registers_t *regs, uint64_t cr3,
                                         uint64_t rsp_override);

void preempt_disable(void);
void preempt_enable(void);
int preempt_count(void);

void task_get(task_t *t);
void task_put(task_t *t);

struct spinlock;
extern void task_switch(task_t *old_task, task_t *new_task, void *lock);
void task_entry_wrapper(void (*fn)(void));
void task_die_hlt(void);

void sched_wake_expired(void);
// Tick del plazo de espera mas cercano (UINT64_MAX si no hay ninguno).
uint64_t sched_next_wake_deadline(void);

// [LinuxKPI RCU] Ticks de la CPU `cpu` que llegaron con preempt_count == 0
// (estados quiescentes). 0 si la CPU nunca ha tenido un tick.
uint64_t sched_rcu_qs_count(int cpu);

__attribute__((noreturn)) void sched_start(task_t *task);

void smp_init(void);
void smp_dump(void);
void sched_start_ap(void);
void smp_set_bsp_lapic_id(uint32_t lapic_id);

void __sched_canary_arm(void);
void __sched_canary_check(void);

_Static_assert(offsetof(task_t, rsp) == 0x00, "switch.asm: rsp @ 0x00");
_Static_assert(offsetof(task_t, fpu_state) == 0x18, "switch.asm");
_Static_assert(offsetof(task_t, cr3) == 0x20, "switch.asm");
_Static_assert(offsetof(task_t, on_cpu) == 0x274, "switch.asm TASK_OFF_ON_CPU");
_Static_assert(offsetof(spinlock_t, locked) == 0 &&
                   sizeof(((spinlock_t *)0)->locked) == 4,
               "switch.asm libera el lock con mov dword [rdx], 0");

_Static_assert(offsetof(task_t, fpu_raw) + sizeof(((task_t *)0)->fpu_raw) +
                       15 <=
                   sizeof(task_t),
               "fpu_raw demasiado cerca del final de task_t");
_Static_assert(offsetof(task_t, fs_base) == 0x2A0, "switch.asm: fs_base");
// sched_task_ptr_valid() exige que las task_t salgan del SLAB (<= 2048 bytes,
// SLAB_MAX_SIZE). Si crece por encima, kmalloc la sirve del heap grande y
// sched_kick_idle_cpu() deja de despertar CPUs ociosas.
_Static_assert(sizeof(task_t) <= 2048, "task_t debe caber en el SLAB");