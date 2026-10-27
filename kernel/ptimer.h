// kernel/ptimer.h
//
// Temporizadores de proceso: alarm/setitimer/getitimer y los temporizadores
// POSIX (timer_create y compañía). Ver ptimer.c.

#ifndef PTIMER_H
#define PTIMER_H

#include <stdbool.h>
#include <stdint.h>

struct task;
struct process;

// Datos de siginfo de una señal que generó un temporizador POSIX.
typedef struct {
  int timerid;
  int overrun;
  uint64_t value; // sigev_value
} ptimer_siginfo_t;

// Syscalls.
int64_t k_alarm(uint64_t sec, uint64_t a2, uint64_t a3, uint64_t a4,
                uint64_t a5);
int64_t k_setitimer(uint64_t which, uint64_t new_ptr, uint64_t old_ptr,
                    uint64_t a4, uint64_t a5);
int64_t k_getitimer(uint64_t which, uint64_t cur_ptr, uint64_t a3, uint64_t a4,
                    uint64_t a5);
int64_t k_timer_create(uint64_t clockid, uint64_t sevp, uint64_t idp,
                       uint64_t a4, uint64_t a5);
int64_t k_timer_settime(uint64_t id, uint64_t flags, uint64_t new_ptr,
                        uint64_t old_ptr, uint64_t a5);
int64_t k_timer_gettime(uint64_t id, uint64_t cur_ptr, uint64_t a3,
                        uint64_t a4, uint64_t a5);
int64_t k_timer_getoverrun(uint64_t id, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5);
int64_t k_timer_delete(uint64_t id, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5);

// Ganchos del ciclo de vida del proceso.
void ptimer_exec(uint32_t pid); // execve: borra los POSIX, conserva itimers
void ptimer_exit(uint32_t pid); // exit: borra todos

// ITIMER_VIRTUAL/ITIMER_PROF: lo llama el tick por cada tick de CPU de `t`.
void ptimer_account_tick(struct task *t);

// Si la señal `sig` pendiente de `p` la generó un temporizador POSIX, rellena
// `out`, la marca entregada y devuelve true. Lo usan la entrega a un handler,
// sigtimedwait y signalfd para poner si_code = SI_TIMER.
bool ptimer_take_siginfo(struct process *p, int sig, ptimer_siginfo_t *out);

#endif
