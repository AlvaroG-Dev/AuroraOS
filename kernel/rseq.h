// kernel/rseq.h
//
// rseq(2): "restartable sequences". Cada hilo registra un area compartida con
// el kernel; el kernel mantiene cpu_id/cpu_id_start y, si el hilo es sacado de
// la CPU o recibe una senal dentro de una seccion critica (rseq_cs), salta a su
// abort_ip.

#ifndef KERNEL_RSEQ_H
#define KERNEL_RSEQ_H

#include "idt.h"
#include "sched.h"
#include <stdint.h>

int64_t k_rseq(uint64_t area, uint64_t len, uint64_t flags, uint64_t sig, uint64_t a5);

// Retorno a userland (irq o syscall): actualiza cpu_id y, si el hilo se
// conmuto desde la ultima vez, aborta la seccion critica en curso.
void rseq_exit_to_user(task_t *t, registers_t *regs);
// Entrega de senal: aborta siempre la seccion critica (Linux hace lo mismo).
void rseq_signal(task_t *t, registers_t *regs);

#endif
