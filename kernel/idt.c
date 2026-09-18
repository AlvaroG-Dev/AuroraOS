// kernel/idt.c
// IDT, ISR handlers y PIC

#include "idt.h"
#include "klog.h"
#include "paging.h"
#include "panic.h"
#include "serial.h"
#include "string.h"
#include <stdint.h>

struct idt_entry {
  uint16_t offset_low;
  uint16_t selector;
  uint8_t ist;
  uint8_t type_attr;
  uint16_t offset_mid;
  uint32_t offset_high;
  uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
  uint16_t limit;
  uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idt_ptr;
static void (*irq_handlers[16])(void) = {0};

// ---------------------------------------------------------------------------
// Stubs de ISR/IRQ (definidos en isr_stubs.asm).
// ---------------------------------------------------------------------------
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);

// LAPIC timer (vector 48).
extern void irq48(void);

// Vector espurio del LAPIC (0xFF).
extern void isr_spurious(void);

// Implementado en pf.c
extern int handle_page_fault(registers_t *regs);

// ---------------------------------------------------------------------------
// Tabla de stubs en el mismo orden en que se colocan en la IDT:
//   0..31  → isr0..isr31   (excepciones CPU)
//   32..47 → irq0..irq15   (IRQs del IOAPIC)
// ---------------------------------------------------------------------------
static const uint64_t isr_stub_table[48] = {
    (uint64_t)isr0,  (uint64_t)isr1,  (uint64_t)isr2,  (uint64_t)isr3,
    (uint64_t)isr4,  (uint64_t)isr5,  (uint64_t)isr6,  (uint64_t)isr7,
    (uint64_t)isr8,  (uint64_t)isr9,  (uint64_t)isr10, (uint64_t)isr11,
    (uint64_t)isr12, (uint64_t)isr13, (uint64_t)isr14, (uint64_t)isr15,
    (uint64_t)isr16, (uint64_t)isr17, (uint64_t)isr18, (uint64_t)isr19,
    (uint64_t)isr20, (uint64_t)isr21, (uint64_t)isr22, (uint64_t)isr23,
    (uint64_t)isr24, (uint64_t)isr25, (uint64_t)isr26, (uint64_t)isr27,
    (uint64_t)isr28, (uint64_t)isr29, (uint64_t)isr30, (uint64_t)isr31,
    (uint64_t)irq0,  (uint64_t)irq1,  (uint64_t)irq2,  (uint64_t)irq3,
    (uint64_t)irq4,  (uint64_t)irq5,  (uint64_t)irq6,  (uint64_t)irq7,
    (uint64_t)irq8,  (uint64_t)irq9,  (uint64_t)irq10, (uint64_t)irq11,
    (uint64_t)irq12, (uint64_t)irq13, (uint64_t)irq14, (uint64_t)irq15,
};

static const char *exception_names[] = {"Division by zero",
                                        "Debug",
                                        "Non-maskable interrupt",
                                        "Breakpoint",
                                        "Into detected overflow",
                                        "Out of bounds",
                                        "Invalid opcode",
                                        "No coprocessor",
                                        "Double fault",
                                        "Coprocessor segment overrun",
                                        "Bad TSS",
                                        "Segment not present",
                                        "Stack fault",
                                        "General protection fault",
                                        "Page fault",
                                        "Unknown interrupt",
                                        "Coprocessor fault",
                                        "Alignment check",
                                        "Machine check",
                                        "Reserved",
                                        "Reserved",
                                        "Reserved",
                                        "Reserved",
                                        "Reserved",
                                        "Reserved",
                                        "Reserved",
                                        "Reserved",
                                        "Reserved",
                                        "Reserved",
                                        "Reserved",
                                        "Reserved",
                                        "Reserved"};

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
  idt[num].offset_low = base & 0xFFFF;
  idt[num].offset_mid = (base >> 16) & 0xFFFF;
  idt[num].offset_high = (base >> 32);
  idt[num].selector = sel;
  idt[num].ist = 0;
  idt[num].type_attr = flags;
  idt[num].zero = 0;
}

void idt_load(void) {
  idt_ptr.limit = sizeof(idt) - 1;
  idt_ptr.base = (uint64_t)&idt;
  __asm__ volatile("lidt (%0)" : : "r"(&idt_ptr));
}

void pic_init(void) {
  __asm__ volatile("outb %0, $0x20" : : "a"((uint8_t)0x11));
  __asm__ volatile("outb %0, $0xA0" : : "a"((uint8_t)0x11));
  __asm__ volatile("outb %0, $0x21" : : "a"((uint8_t)0x20));
  __asm__ volatile("outb %0, $0xA1" : : "a"((uint8_t)0x28));
  __asm__ volatile("outb %0, $0x21" : : "a"((uint8_t)0x04));
  __asm__ volatile("outb %0, $0xA1" : : "a"((uint8_t)0x02));
  __asm__ volatile("outb %0, $0x21" : : "a"((uint8_t)0x01));
  __asm__ volatile("outb %0, $0xA1" : : "a"((uint8_t)0x01));
  __asm__ volatile("outb %0, $0x21" : : "a"((uint8_t)0xFF));
  __asm__ volatile("outb %0, $0xA1" : : "a"((uint8_t)0xFF));
}

void pic_eoi(uint8_t irq) {
  if (irq >= 8)
    __asm__ volatile("outb %0, $0xA0" : : "a"((uint8_t)0x20));
  __asm__ volatile("outb %0, $0x20" : : "a"((uint8_t)0x20));
}

void irq_install_handler(uint8_t irq, void (*handler)(void)) {
  irq_handlers[irq] = handler;

  // Con IOAPIC, desenmascaramos la IRQ en el IOAPIC.
  // El PIC ya está enmascarado (apic_init lo hizo).
  extern void ioapic_mask_irq(uint8_t irq, int masked);
  ioapic_mask_irq(irq, 0);
}

void irq_uninstall_handler(uint8_t irq) {
  irq_handlers[irq] = 0;
  extern void ioapic_mask_irq(uint8_t irq, int masked);
  ioapic_mask_irq(irq, 1);
}

void idt_init(void) {
  memset(&idt, 0, sizeof(idt));

  for (uint8_t i = 0; i < 48; i++) {
    idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E);
  }

  // Vector 48: LAPIC timer.
  idt_set_gate(48, (uint64_t)irq48, 0x08, 0x8E);

  // Vector 0xFF: espurio del LAPIC.
  idt_set_gate(0xFF, (uint64_t)isr_spurious, 0x08, 0x8E);

  pic_init();
  idt_load();
  LOG_INFO("[IDT] IDT y PIC inicializados (256 entradas)");
}

// ---------------------------------------------------------------------------
// ISR handler principal
// ---------------------------------------------------------------------------
void isr_handler(registers_t *regs) {
  if (regs->int_num == 14) {
    if (handle_page_fault(regs)) {
      return;
    }
  }

  const char *name = (regs->int_num < 32) ? exception_names[regs->int_num]
                                          : "unknown exception";
  panic(regs, "unhandled exception #%lu (%s)", (unsigned long)regs->int_num,
        name);
}

void irq_handler(registers_t *regs) {
  // Enviar EOI al LAPIC ANTES de llamar al handler. Si el handler cambia
  // de tarea (por ejemplo, el timer llama a sched_tick), no queremos que
  // el EOI se pierda en el cambio de contexto.
  extern void lapic_eoi(void);
  lapic_eoi();

  // LAPIC timer (vector 48).
  if (regs->int_num == 48) {
    extern void lapic_timer_handler(void);
    lapic_timer_handler();
    return;
  }

  // IRQs del IOAPIC (vectores 32-47).
  uint8_t irq = (uint8_t)(regs->int_num - 32);
  if (irq < 16 && irq_handlers[irq]) {
    irq_handlers[irq]();
  }
}