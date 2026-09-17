// kernel/panic.h
#ifndef PANIC_H
#define PANIC_H

#include "idt.h"
#include <stdint.h>

void backtrace(uint64_t rbp, uint64_t rip, int max_frames);

__attribute__((noreturn)) void panic(registers_t *regs, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

__attribute__((noreturn)) void panic_noctx(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

// NUEVO: consultar desde heap/slab/sched para no coger locks durante
// el panic. Retorna 1 si estamos dentro de panic(), 0 si no.
int panic_in_progress(void);

void dump_registers(registers_t *regs);
void dump_scheduler(void);
void dump_paging(void);
void dump_heap(void);
void dump_slab(void);

#endif