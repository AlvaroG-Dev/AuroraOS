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

#endif