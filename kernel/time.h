// kernel/time.h
#ifndef KERNEL_TIME_H
#define KERNEL_TIME_H

#include <stdint.h>

// Número de ticks por segundo del sistema (PIT o LAPIC timer).
#define KERNEL_HZ 1000

// Contador global de ticks desde el arranque. Lo incrementa el timer
// handler (PIT o LAPIC timer). Es la base del scheduler preemptivo
// y de los timestamps de klog.
extern volatile uint64_t tick_count;

// Inicializa el PIT a KERNEL_HZ.
void pit_init(void);

// Handler genérico de tick. Es lo que llaman tanto el PIT (IRQ 0) como
// el LAPIC timer (vector 48). Incrementa tick_count y notifica a los
// subsistemas (scheduler, compositor, etc.).
void time_tick(void);

// Inicializa el LAPIC timer. Calibra contra el TSC y configura el
// LAPIC timer en modo periódico a KERNEL_HZ. Requiere que el LAPIC
// esté mapeado y activo (apic_init) y que el TSC esté calibrado.
//
// Tras esta llamada, el LAPIC timer sustituye al PIT como fuente de
// ticks. La IRQ 0 del IOAPIC queda enmascarada.
void lapic_timer_init(void);
void lapic_timer_init_ap(void);

// Handler del LAPIC timer. Llamado desde irq_handler cuando llega el
// vector 48.
void lapic_timer_handler(void);

#endif