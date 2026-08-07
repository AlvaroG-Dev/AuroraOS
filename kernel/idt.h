// kernel/idt.h
// Interrupt Descriptor Table para x86_64

#ifndef IDT_H
#define IDT_H

#include <stdint.h>

#define IDT_ENTRIES 256

// IRQ base despues de remap del PIC
#define IRQ0  32
#define IRQ1  33
#define IRQ2  34
#define IRQ3  35
#define IRQ4  36
#define IRQ5  37
#define IRQ6  38
#define IRQ7  39
#define IRQ8  40
#define IRQ9  41
#define IRQ10 42
#define IRQ11 43
#define IRQ12 44
#define IRQ13 45
#define IRQ14 46
#define IRQ15 47

void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);
void pic_init(void);
void pic_eoi(uint8_t irq);
void irq_install_handler(uint8_t irq, void (*handler)(void));
void irq_uninstall_handler(uint8_t irq);
void idt_load(void);

#endif
