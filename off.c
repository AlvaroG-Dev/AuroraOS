#include <stddef.h>
#include <stdint.h>
#include <stdio.h>


typedef struct wait_queue {
  uint32_t lock;
  void *head;
  int nr_waiting;
} wait_queue_t;

typedef struct ipc_msg {
  uint32_t sender, receiver, type, size;
  uint8_t data[64];
} ipc_msg_t;

typedef struct ipc_mailbox {
  ipc_msg_t messages[16];
  uint8_t head, tail, count;
  wait_queue_t wq;
} ipc_mailbox_t;

struct process;

typedef struct task {
  uint64_t rsp;
  uint64_t *stack;
  uint32_t id;
  int state;
  uint64_t fpu_state;
  uint64_t cr3;
  uint8_t fpu_raw[512 + 16];
  ipc_mailbox_t mailbox;
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
} task_t;

int main(void) {
  printf("sizeof(task_t) = %zu\n", sizeof(task_t));
  printf("offsetof(rsp)         = 0x%zx\n", offsetof(task_t, rsp));
  printf("offsetof(stack)       = 0x%zx\n", offsetof(task_t, stack));
  printf("offsetof(id)          = 0x%zx\n", offsetof(task_t, id));
  printf("offsetof(state)       = 0x%zx\n", offsetof(task_t, state));
  printf("offsetof(fpu_state)   = 0x%zx\n", offsetof(task_t, fpu_state));
  printf("offsetof(cr3)         = 0x%zx\n", offsetof(task_t, cr3));
  printf("offsetof(fpu_raw)     = 0x%zx\n", offsetof(task_t, fpu_raw));
  printf("offsetof(mailbox)     = 0x%zx\n", offsetof(task_t, mailbox));
  printf("offsetof(is_idle)     = 0x%zx\n", offsetof(task_t, is_idle));
  printf("offsetof(next)        = 0x%zx\n", offsetof(task_t, next));
  printf("offsetof(proc)        = 0x%zx\n", offsetof(task_t, proc));
  printf("offsetof(waiting_on)  = 0x%zx\n", offsetof(task_t, waiting_on));
  printf("offsetof(wake_reason) = 0x%zx\n", offsetof(task_t, wake_reason));
  printf("offsetof(preempt_count)= 0x%zx\n", offsetof(task_t, preempt_count));
  printf("offsetof(refcount)    = 0x%zx\n", offsetof(task_t, refcount));
  printf("offsetof(need_resched)= 0x%zx\n", offsetof(task_t, need_resched));
  printf("offsetof(cpu_affinity)= 0x%zx\n", offsetof(task_t, cpu_affinity));
  printf("offsetof(on_cpu)      = 0x%zx\n", offsetof(task_t, on_cpu));
  return 0;
}