// kernel/apic.h
#ifndef KERNEL_APIC_H
#define KERNEL_APIC_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// LAPIC (Local APIC)
//
// Cada CPU tiene su propio LAPIC. Se accede por MMIO a partir de la
// dirección base detectada en el MADT (normalmente 0xFEE00000).
// Todos los accesos son de 32 bits y alineados a 16 bytes.
// ---------------------------------------------------------------------------

#define LAPIC_REG_ID 0x020
#define LAPIC_REG_VERSION 0x030
#define LAPIC_REG_TPR 0x080
#define LAPIC_REG_EOI 0x0B0
#define LAPIC_REG_LDR 0x0D0
#define LAPIC_REG_DFR 0x0E0
#define LAPIC_REG_SVR 0x0F0
#define LAPIC_REG_ESR 0x280
#define LAPIC_REG_ICR_LOW 0x300
#define LAPIC_REG_ICR_HIGH 0x310
#define LAPIC_REG_LVT_TIMER 0x320
#define LAPIC_REG_LVT_THERMAL 0x330
#define LAPIC_REG_LVT_PERF 0x340
#define LAPIC_REG_LVT_LINT0 0x350
#define LAPIC_REG_LVT_LINT1 0x360
#define LAPIC_REG_LVT_ERROR 0x370
#define LAPIC_REG_TIMER_INIT 0x380
#define LAPIC_REG_TIMER_CURRENT 0x390
#define LAPIC_REG_TIMER_DIV 0x3E0

// Bits del SVR (Spurious Vector Register)
#define LAPIC_SVR_ENABLE (1 << 8)
#define LAPIC_SVR_FOCUS_DISABLE (1 << 9) // focus CPU checking off

// Bits de las LVT entries (Timer, LINT, Error, etc.)
#define LAPIC_LVT_MASKED (1 << 16)
#define LAPIC_LVT_TRIGGER_LEVEL (1 << 15)
#define LAPIC_LVT_REMOTE_IRR (1 << 14)
#define LAPIC_LVT_POLARITY_LOW (1 << 13)
#define LAPIC_LVT_DELIVERY_STATUS (1 << 12)

// Modos del LAPIC timer (bits 17-18 del LVT Timer)
#define LAPIC_TIMER_MODE_ONESHOT 0
#define LAPIC_TIMER_MODE_PERIODIC (1 << 17)
#define LAPIC_TIMER_MODE_TSC_DEADLINE (2 << 17)

// Divisores del LAPIC timer (bits 0-3 del TIMER_DIV)
#define LAPIC_TIMER_DIV_1 0x0B
#define LAPIC_TIMER_DIV_2 0x00
#define LAPIC_TIMER_DIV_4 0x01
#define LAPIC_TIMER_DIV_8 0x02
#define LAPIC_TIMER_DIV_16 0x03
#define LAPIC_TIMER_DIV_32 0x08
#define LAPIC_TIMER_DIV_64 0x09
#define LAPIC_TIMER_DIV_128 0x0A

// Modos de entrega del LVT (bits 8-10)
#define LAPIC_LVT_DELIVERY_FIXED (0 << 8)
#define LAPIC_LVT_DELIVERY_NMI (4 << 8)
#define LAPIC_LVT_DELIVERY_EXTINT (7 << 8)

// ---------------------------------------------------------------------------
// IOAPIC
// ---------------------------------------------------------------------------
#define IOAPIC_REG_SELECT 0x00
#define IOAPIC_REG_DATA 0x10

// Bits de una entrada de redirección del IOAPIC (64 bits)
#define IOAPIC_REDIR_MASKED (1ULL << 16)
#define IOAPIC_REDIR_TRIGGER_LEVEL (1ULL << 15)
#define IOAPIC_REDIR_POLARITY_LOW (1ULL << 13)
#define IOAPIC_REDIR_DEST_PHYSICAL (0ULL << 11)
#define IOAPIC_REDIR_DEST_LOGICAL (1ULL << 11)
#define IOAPIC_REDIR_DELIVERY_FIXED (0ULL << 8)
#define IOAPIC_REDIR_DELIVERY_LOWEST (1ULL << 8)
#define IOAPIC_REDIR_ACTIVE_LOW (1ULL << 13)
#define IOAPIC_REDIR_EDGE (0ULL << 15)

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------

// Inicializa el subsistema APIC: activa el LAPIC del BSP, mapea el LAPIC
// y los IOAPICs, y prepara las redirecciones. Debe llamarse tras
// acpi_init() y tras paging_init() (para poder mapear MMIO).
void apic_init(void);

// Devuelve el APIC ID del LAPIC del CPU actual (leído del registro).
uint32_t lapic_get_id(void);

// Devuelve el APIC ID del BSP (el que se detectó al inicializar).
uint32_t lapic_get_bsp_id(void);

// Lee/escribe un registro del LAPIC del CPU actual.
uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t value);

// Envía End Of Interrupt al LAPIC del CPU actual.
void lapic_eoi(void);

// Redirige una IRQ legacy (0-15) a un vector en el CPU destino,
// pasando por el IOAPIC. Aplica los ISOs del MADT.
// `vector`: número de vector de IDT (32-255).
// `dest_apic_id`: APIC ID del CPU destino (o 0xFF para broadcast).
// `masked`: si es 1, la entrada queda enmascarada.
void ioapic_redirect_irq(uint8_t irq, uint8_t vector, uint32_t dest_apic_id,
                         int masked);

// Enmascara/desenmascara una IRQ en el IOAPIC.
void ioapic_mask_irq(uint8_t irq, int masked);

// Debug.
void apic_dump(void);

#endif