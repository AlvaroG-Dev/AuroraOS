// kernel/apic.h
#ifndef KERNEL_APIC_H
#define KERNEL_APIC_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// LAPIC (Local APIC)
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

// Bits de las LVT entries
#define LAPIC_LVT_MASKED (1 << 16)

// Modos de entrega del LVT (bits 8-10)
#define LAPIC_LVT_DELIVERY_FIXED (0 << 8)
#define LAPIC_LVT_DELIVERY_NMI (4 << 8)
#define LAPIC_LVT_DELIVERY_EXTINT (7 << 8)

// Modos del LAPIC timer (bits 17-18 del LVT Timer)
#define LAPIC_TIMER_MODE_ONESHOT (0 << 17)
#define LAPIC_TIMER_MODE_PERIODIC (1 << 17)

// Divisores del LAPIC timer (bits 0-3 del TIMER_DIV)
#define LAPIC_TIMER_DIV_1 0x0B
#define LAPIC_TIMER_DIV_2 0x00
#define LAPIC_TIMER_DIV_4 0x01
#define LAPIC_TIMER_DIV_8 0x02
#define LAPIC_TIMER_DIV_16 0x03
#define LAPIC_TIMER_DIV_32 0x08
#define LAPIC_TIMER_DIV_64 0x09
#define LAPIC_TIMER_DIV_128 0x0A

// ---------------------------------------------------------------------------
// IOAPIC
// ---------------------------------------------------------------------------
#define IOAPIC_REG_SELECT 0x00
#define IOAPIC_REG_DATA 0x10

// Bits de una entrada de redirección del IOAPIC (64 bits)
#define IOAPIC_REDIR_DEST_LOGICAL (1ULL << 11)
#define IOAPIC_REDIR_ACTIVE_LOW (1ULL << 13)
#define IOAPIC_REDIR_TRIGGER_LEVEL (1ULL << 15)
#define IOAPIC_REDIR_MASKED (1ULL << 16)

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------

void apic_init(void);
void apic_init_ap(void);
uint32_t lapic_get_id(void);
uint32_t lapic_get_bsp_id(void);
uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t value);
void lapic_eoi(void);

void ioapic_redirect_irq(uint8_t irq, uint8_t vector, uint32_t dest_apic_id,
                         int masked);
void ioapic_mask_irq(uint8_t irq, int masked);

void apic_dump(void);
void *lapic_get_base(void);

#endif