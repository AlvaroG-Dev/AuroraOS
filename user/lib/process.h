#ifndef LIB_PROCESS_H
#define LIB_PROCESS_H

#include "../syscall.h"

// Spawn a new process from a TarFS path. Returns child PID or -1 on error.
static inline int spawn(const char *path) { return sys_spawn(path); }

// Wait for a specific child PID to exit. Returns pid or -1.
static inline int waitpid(int pid, int *status, int options) {
  return sys_waitpid(pid, status, options);
}

// Wait for any child process to exit.
static inline int wait(int *status) { return sys_waitpid(-1, status, 0); }

// Get current process PID.
static inline int getpid(void) { return sys_getpid(); }

static inline int spawn_args(const char *path, char *const argv[], int argc) {
  return sys_spawn_args(path, argv, argc);
}

#endif
