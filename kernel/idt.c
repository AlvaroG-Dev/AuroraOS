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
static volatile int in_panic = 0;

// ---------------------------------------------------------------------------
// Stubs de ISR/IRQ (definidos en isr_stubs.asm).
// Los declaramos todos aquí para poder meterlos en una tabla.
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

// Implementado en pf.c
extern int handle_page_fault(registers_t *regs);

// ---------------------------------------------------------------------------
// Tabla de stubs en el mismo orden en que se colocan en la IDT:
//   0..31  → isr0..isr31   (excepciones CPU)
//   32..47 → irq0..irq15   (IRQs del PIC)
//
// Usamos uint64_t en vez de punteros a función porque en C convertir
// function-pointer a void* es UB técnico. La IDT almacena direcciones
// (enteros), así que este es el tipo correcto.
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
  uint16_t port = (irq < 8) ? 0x21 : 0xA1;
  uint8_t mask;

  __asm__ volatile("inb %1, %0" : "=a"(mask) : "dN"(port));
  mask &= ~(1 << (irq & 7));
  __asm__ volatile("outb %0, %1" : : "a"(mask), "dN"(port));

  if (irq >= 8) {
    uint8_t master_mask;
    __asm__ volatile("inb $0x21, %0" : "=a"(master_mask));
    master_mask &= ~(1 << 2);
    __asm__ volatile("outb %0, $0x21" : : "a"(master_mask));
  }
}

void irq_uninstall_handler(uint8_t irq) {
  irq_handlers[irq] = 0;
  uint16_t port = (irq < 8) ? 0x21 : 0xA1;
  uint8_t mask;
  __asm__ volatile("inb %1, %0" : "=a"(mask) : "dN"(port));
  mask |= (1 << (irq & 7));
  __asm__ volatile("outb %0, %1" : : "a"(mask), "dN"(port));
}

void idt_init(void) {
  memset(&idt, 0, sizeof(idt));

  // Excepciones (0-31) + IRQs (32-47): 48 entradas contiguas.
  for (uint8_t i = 0; i < 48; i++) {
    idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E);
  }

  pic_init();
  idt_load();
  LOG_INFO("[IDT] IDT y PIC inicializados (256 entradas)");
}

void print_hex64(uint64_t val) { serial_hex(val); }

static void print_page_fault_details(uint64_t err_code, uint64_t cr2) {
  serial_puts("\n--- Page Fault Details ---");
  serial_puts("\n Faulting VADDR: ");
  print_hex64(cr2);

  serial_puts("\n Cause: ");
  serial_puts((err_code & 0x01) ? "Page-protection violation"
                                : "Page not present");

  serial_puts("\n Operation: ");
  serial_puts((err_code & 0x02) ? "Write" : "Read");

  serial_puts("\n Privilege: ");
  serial_puts((err_code & 0x04) ? "User-mode (Ring 3)"
                                : "Supervisor-mode (Ring 0)");

  if (err_code & 0x08)
    serial_puts("\n Reserved bit overwritten!");
  if (err_code & 0x10) {
    serial_puts("\n Instruction fetch");
    if (!(err_code & 0x01)) {
      serial_puts(" (NX violation: pagina no ejecutable)");
    } else {
      serial_puts(
          " (SMEP violation: kernel intento ejecutar codigo de usuario)");
    }
  }
  if ((err_code & 0x01) && !(err_code & 0x04) && !(err_code & 0x10)) {
    serial_puts("\n  -> Posible violacion SMAP: kernel accedio a memoria de "
                "usuario sin stac/clac");
  }
  if (err_code & 0x20)
    serial_puts("\n PKRU violation");
  if (err_code & 0x40)
    serial_puts("\n Shadow stack access fault");
}

static void panic_dump(registers_t *regs) {
  uint64_t cr0, cr2, cr3, cr4;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

  serial_puts("\n=======================================================");
  serial_puts("\n               !!! KERNEL PANIC !!!                   ");
  serial_puts("\n=======================================================");

  serial_puts("\nException: #");
  if (regs->int_num < 32) {
    serial_puts(exception_names[regs->int_num]);
  } else {
    serial_puts("Unknown Exception");
  }
  serial_puts(" (Vector ");
  serial_hex(regs->int_num);
  serial_puts(")");

  serial_puts("\nError Code: ");
  print_hex64(regs->error_code);

  serial_puts("\n\n--- CPU Instruction Pointer & Stack ---");
  serial_puts("\nRIP: ");
  print_hex64(regs->rip);
  serial_puts("  CS: ");
  print_hex64(regs->cs);
  serial_puts("  RFLAGS: ");
  print_hex64(regs->rflags);
  serial_puts("\nRSP: ");
  print_hex64(regs->rsp);
  serial_puts("  SS: ");
  print_hex64(regs->ss);

  serial_puts("\n\n--- General Purpose Registers ---");
  serial_puts("\nRAX: ");
  print_hex64(regs->rax);
  serial_puts("  RBX: ");
  print_hex64(regs->rbx);
  serial_puts("\nRCX: ");
  print_hex64(regs->rcx);
  serial_puts("  RDX: ");
  print_hex64(regs->rdx);
  serial_puts("\nRSI: ");
  print_hex64(regs->rsi);
  serial_puts("  RDI: ");
  print_hex64(regs->rdi);
  serial_puts("\nRBP: ");
  print_hex64(regs->rbp);
  serial_puts("  R8 : ");
  print_hex64(regs->r8);
  serial_puts("\nR9 : ");
  print_hex64(regs->r9);
  serial_puts("  R10: ");
  print_hex64(regs->r10);
  serial_puts("\nR11: ");
  print_hex64(regs->r11);
  serial_puts("  R12: ");
  print_hex64(regs->r12);
  serial_puts("\nR13: ");
  print_hex64(regs->r13);
  serial_puts("  R14: ");
  print_hex64(regs->r14);
  serial_puts("\nR15: ");
  print_hex64(regs->r15);

  serial_puts("\n\n--- Control Registers ---");
  serial_puts("\nCR0: ");
  print_hex64(cr0);
  serial_puts("  CR2: ");
  print_hex64(cr2);
  serial_puts("\nCR3: ");
  print_hex64(cr3);
  serial_puts("  CR4: ");
  print_hex64(cr4);

  if (regs->int_num == 14) {
    print_page_fault_details(regs->error_code, cr2);
  }

  serial_puts("\n\n--- Stack Dump (RSP) ---");
  uint64_t *stack = (uint64_t *)regs->rsp;
  for (int i = 0; i < 8; i++) {
    serial_puts("\n[RSP+");
    serial_hex(i * 8);
    serial_puts("]: ");
    print_hex64(stack[i]);
  }

  serial_puts("\n\n--- Backtrace ---");
  backtrace(regs->rbp, regs->rip, 16);

  serial_puts("\n=======================================================");
  serial_puts("\nSystem Halted.\n");
}

void isr_handler(registers_t *regs) {
  if (in_panic) {
    __asm__ volatile("cli; hlt");
  }

  if (regs->int_num == 14) {
    if (handle_page_fault(regs)) {
      return;
    }
  }

  in_panic = 1;
  panic_dump(regs);

  __asm__ volatile("cli; hlt");
}

void irq_handler(registers_t *regs) {
  uint8_t irq = (uint8_t)(regs->int_num - 32);
  pic_eoi(irq);
  if (irq < 16 && irq_handlers[irq]) {
    irq_handlers[irq]();
  }
}