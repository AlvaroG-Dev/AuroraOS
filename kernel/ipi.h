// kernel/ipi.h
#ifndef KERNEL_IPI_H
#define KERNEL_IPI_H

#include <stdint.h>

// Vectores de IPI. Deben estar libres en idt.c (no colisionar con
// excepciones 0-31, IRQs 32-47, LAPIC timer 48, espurio 0xFF).
#define IPI_VECTOR_RESCHED 0xFB
#define IPI_VECTOR_TLB 0xFC
#define IPI_VECTOR_CALL 0xFD
#define IPI_VECTOR_HALT 0xFE

// Inicializa el subsistema. Llamar tras apic_init().
void ipi_init(void);

// Envía una IPI a un CPU concreto (por APIC ID).
void ipi_send(uint32_t apic_id, uint8_t vector);

// Envía una IPI a todos los CPUs menos al actual.
void ipi_send_allbutself(uint8_t vector);

// Envía una IPI a todos los CPUs.
void ipi_send_all(uint8_t vector);

// Handlers invocados desde los stubs de isr_stubs.asm.
void ipi_handler_resched(void);
void ipi_handler_tlb(void);

// [H3] TLB shootdown cross-CPU.
//
// Invalida el TLB en todos los CPUs online (local + IPI a los demás,
// esperando sus acks):
//   - addr != 0: invalida la página concreta (invlpg) en cada CPU.
//   - addr == 0: flush completo del TLB (reload CR3) en cada CPU.
//
// Devuelve cuando todos los CPUs han hecho el ack. Sin APs activos
// degenera en una invalidación local.
//
// IMPORTANTE: debe llamarse con IRQs HABILITADOS. La espera del ack
// requiere atender las IPIs de otros CPUs que puedan estar haciendo
// su propio shootdown; con IRQs off hay riesgo de deadlock entre dos
// shootdowns concurrentes.
void ipi_tlb_shootdown(uint64_t addr);

#endif