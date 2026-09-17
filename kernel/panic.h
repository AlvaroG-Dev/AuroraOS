// kernel/panic.h
#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>

// Imprime la pila de llamadas siguiendo la cadena de RBP.
// 'max_frames' limita la profundidad.
void backtrace(uint64_t rbp, uint64_t rip, int max_frames);

#endif