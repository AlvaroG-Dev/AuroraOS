// kernel/idt.c
// IDT, ISR handlers y PIC

#include "idt.h"
#include "ipi.h"
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
static void (*irq_ack_handlers[16])(void) = {NULL};
static void (*irq_process_handlers[16])(void) = {NULL};
static void (*irq_handlers[16])(void) = {NULL}; // legacy

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

extern void ipi_stub_resched(void);
extern void ipi_stub_tlb(void);

extern void ioapic_mask_irq(uint8_t irq, int masked);

extern void msi_stub_0x60(void);

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
  idt[num].ist = (num == 8) ? 1 : 0; // #DF usa IST1; el resto usa RSP normal.
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
  ioapic_mask_irq(irq, 0);
}

void irq_uninstall_handler(uint8_t irq) {
  irq_handlers[irq] = 0;
  ioapic_mask_irq(irq, 1);
}

void idt_init(void) {
  memset(&idt, 0, sizeof(idt));

  for (uint8_t i = 0; i < 48; i++) {
    idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E);
  }

  // Vector 48: LAPIC timer.
  idt_set_gate(48, (uint64_t)irq48, 0x08, 0x8E);

  // [SMP 4.4] IPIs. Vectores 0xFB (resched) y 0xFC (TLB).
  idt_set_gate(IPI_VECTOR_RESCHED, (uint64_t)ipi_stub_resched, 0x08, 0x8E);
  idt_set_gate(IPI_VECTOR_TLB, (uint64_t)ipi_stub_tlb, 0x08, 0x8E);

  // Vector 0xFF: espurio del LAPIC.
  idt_set_gate(0xFF, (uint64_t)isr_spurious, 0x08, 0x8E);

  // Vector MSI para el AHCI (0x60).
  idt_set_gate(0x60, (uint64_t)msi_stub_0x60, 0x08, 0x8E);

  pic_init();
  idt_load();
  LOG_INFO("[IDT] IDT y PIC inicializados (256 entradas)");
}

// ---------------------------------------------------------------------------
// ISR handler principal
// ---------------------------------------------------------------------------
void isr_handler(registers_t *regs) {
  // [DEBUG] Escribir al puerto 0xE9 sin locks ni buffers.
  {
    uint8_t _c = 'I';
    __asm__ volatile("outb %0, $0xE9" : : "a"(_c));
  }

  // Guardar el frame en un buffer estático (no en el stack).
  static registers_t saved_regs;
  saved_regs = *regs;

  // Log con LOG_DEBUG normal (puede colgar pero es lo que tenemos).
  LOG_DEBUG("[ISR] int_num=%lu error=0x%lx rip=%p rsp=%p cs=0x%lx ss=0x%lx",
            (unsigned long)saved_regs.int_num,
            (unsigned long)saved_regs.error_code, (void *)saved_regs.rip,
            (void *)saved_regs.rsp, (unsigned long)saved_regs.cs,
            (unsigned long)saved_regs.ss);

  if (saved_regs.int_num == 14) {
    if (handle_page_fault(regs)) {
      return;
    }
  }

  // Comparar.
  LOG_ERR("[ISR] ahora: int_num=%lu rip=%p rsp=%p ss=0x%lx",
          (unsigned long)regs->int_num, (void *)regs->rip, (void *)regs->rsp,
          (unsigned long)regs->ss);

  panic(&saved_regs, "unhandled exception #%lu (%s)",
        (unsigned long)saved_regs.int_num,
        (saved_regs.int_num < 32) ? exception_names[saved_regs.int_num] : "?");
}

void irq_install_handler_ex(uint8_t irq, void (*ack)(void),
                            void (*process)(void)) {
  if (irq >= 16)
    return;
  irq_ack_handlers[irq] = ack;
  irq_process_handlers[irq] = process;
  // No tocar irq_handlers[irq] (legacy), o ponerlo a NULL.
  irq_handlers[irq] = NULL;
  ioapic_mask_irq(irq, 0); // desenmascarar
}

void irq_handler(registers_t *regs) {
  // [FIX] LAPIC timer (vector 48): EOI ANTES del handler.
  //
  // lapic_timer_handler() llama a sched_tick(), que puede hacer un
  // task_switch. Si el switch ocurre, la instrucción lapic_eoi() NO se
  // ejecuta hasta que la tarea original vuelva a este punto de la pila.
  // Mientras tanto, el bit del vector 48 sigue puesto en el ISR del
  // LAPIC, y esa CPU NO recibe más ticks del timer.
  //
  // El caso patológico: la tarea original se marca TASK_DEAD y su pila
  // se libera. El lapic_eoi() pendiente se pierde con la pila. Esa CPU
  // deja de recibir ticks para siempre: sin preemption, sin timeouts,
  // sin sched_wake_expired. Si le pasa a las 4 CPUs, el sistema se
  // congela sin panic, sin PF, sin logs.
  //
  // El LAPIC timer es edge-triggered: la línea ya está desasertada
  // cuando llega el handler. El EOI solo limpia el ISR. Enviarlo antes
  // es correcto y elimina la ventana.
  if (regs->int_num == 48) {
    extern void lapic_timer_handler(void);
    extern void lapic_eoi(void);
    lapic_eoi();           // [FIX] PRIMERO
    lapic_timer_handler(); // luego el handler (que puede cambiar de tarea)
    return;
  }

  // IRQs del IOAPIC (32..47). Aquí el patrón ack → EOI → process es
  // correcto para IRQs level-triggered: el ack silencia la fuente
  // antes de enviar el EOI, así el IOAPIC no re-dispara.
  uint8_t irq = (uint8_t)(regs->int_num - 32);

  if (irq < 16) {
    if (irq_ack_handlers[irq]) {
      irq_ack_handlers[irq]();
    } else if (irq_handlers[irq]) {
      irq_handlers[irq]();
    }
  }

  extern void lapic_eoi(void);
  lapic_eoi();

  if (irq < 16 && irq_process_handlers[irq]) {
    irq_process_handlers[irq]();
  }
}

// ===========================================================================
// MSI handlers.
// ===========================================================================
static void (*msi_handlers[MSI_VECTOR_COUNT])(void) = {NULL};

void msi_install_handler(uint8_t vector, void (*handler)(void)) {
  if (vector < MSI_VECTOR_BASE || vector > MSI_VECTOR_END)
    return;
  msi_handlers[vector - MSI_VECTOR_BASE] = handler;
}

void msi_dispatch(uint8_t vector) {
  if (vector < MSI_VECTOR_BASE || vector > MSI_VECTOR_END)
    return;
  if (msi_handlers[vector - MSI_VECTOR_BASE])
    msi_handlers[vector - MSI_VECTOR_BASE]();
}

// ===========================================================================
// msi_handler: entrada del stub MSI (msi_stub_0x60).
//
// El stub empuja int_num=0x60 en el stack y salta a msi_common, que
// salva los registros y llama aquí con el registers_t en RDI.
//
// Este handler despacha el vector MSI al handler registrado y manda
// el EOI al LAPIC.
// ===========================================================================
void msi_handler(registers_t *regs) {
  uint8_t vector = (uint8_t)(regs->int_num & 0xFF);
  msi_dispatch(vector);
  extern void lapic_eoi(void);
  lapic_eoi();
}