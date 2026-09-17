// kernel/process.h
#ifndef PROCESS_H
#define PROCESS_H

#include "sched.h"
#include "vfs.h"
#include "wait.h"
#include <stddef.h>
#include <stdint.h>

#define WNOHANG 1

typedef struct process {
  uint32_t pid;
  uint32_t ppid;
  int exit_code;
  int is_zombie;
  char name[64];
  task_t *task;
  uint64_t pml4_phys;
  uint64_t load_base;
  uint64_t heap_start;
  uint64_t heap_end;
  uint64_t heap_max;
  // VMA y stack info:
  struct vma *vma_list;
  uint64_t stack_base;
  uint64_t stack_low;
  uint64_t stack_top;
  uint64_t stack_guard;
  file_descriptor_t *fds[MAX_PROCESS_FDS];
  struct process *next;

  // Wait queue: aquí duermen los procesos que esperan a que
  // alguno de SUS hijos muera. wake_up_all lo llama process_exit
  // cuando un hijo termina.
  wait_queue_t child_wq;
} process_t;

process_t *process_spawn(const char *name, const void *elf_data,
                         size_t elf_size);
process_t *process_load(const char *path);
process_t *process_spawn_child(process_t *parent, const char *path);

int process_waitpid(process_t *parent, int32_t pid, int *status_out,
                    int options);

void process_exit(process_t *proc, int exit_code);

static inline process_t *process_exec(const char *path) {
  return process_load(path);
}

process_t *process_current(void);
process_t *process_get_by_pid(uint32_t pid);
void *process_sbrk(process_t *proc, int64_t increment);
void process_terminate(process_t *proc);

#endif // PROCESS_H