// kernel/idt.c
// IDT, ISR handlers y PIC

#include "idt.h"
#include "serial.h"
#include <stdint.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
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

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

static const char *exception_names[] = {
    "Division by zero", "Debug", "Non-maskable interrupt", "Breakpoint",
    "Into detected overflow", "Out of bounds", "Invalid opcode", "No coprocessor",
    "Double fault", "Coprocessor segment overrun", "Bad TSS", "Segment not present",
    "Stack fault", "General protection fault", "Page fault", "Unknown interrupt",
    "Coprocessor fault", "Alignment check", "Machine check", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved"
};

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt[num].offset_low  = base & 0xFFFF;
    idt[num].offset_mid  = (base >> 16) & 0xFFFF;
    idt[num].offset_high = (base >> 32);
    idt[num].selector    = sel;
    idt[num].ist         = 0;
    idt[num].type_attr   = flags;
    idt[num].zero        = 0;
}

void idt_load(void) {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint64_t)&idt;
    __asm__ volatile ("lidt (%0)" : : "r"(&idt_ptr));
}

void pic_init(void) {
    __asm__ volatile ("outb %0, $0x20" : : "a"((uint8_t)0x11));
    __asm__ volatile ("outb %0, $0xA0" : : "a"((uint8_t)0x11));
    __asm__ volatile ("outb %0, $0x21" : : "a"((uint8_t)0x20));
    __asm__ volatile ("outb %0, $0xA1" : : "a"((uint8_t)0x28));
    __asm__ volatile ("outb %0, $0x21" : : "a"((uint8_t)0x04));
    __asm__ volatile ("outb %0, $0xA1" : : "a"((uint8_t)0x02));
    __asm__ volatile ("outb %0, $0x21" : : "a"((uint8_t)0x01));
    __asm__ volatile ("outb %0, $0xA1" : : "a"((uint8_t)0x01));
    __asm__ volatile ("outb %0, $0x21" : : "a"((uint8_t)0xFF));
    __asm__ volatile ("outb %0, $0xA1" : : "a"((uint8_t)0xFF));
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8) __asm__ volatile ("outb %0, $0xA0" : : "a"((uint8_t)0x20));
    __asm__ volatile ("outb %0, $0x20" : : "a"((uint8_t)0x20));
}

void irq_install_handler(uint8_t irq, void (*handler)(void)) {
    irq_handlers[irq] = handler;
    uint16_t port = (irq < 8) ? 0x21 : 0xA1;
    uint8_t mask;
    
    __asm__ volatile ("inb %1, %0" : "=a"(mask) : "dN"(port));
    mask &= ~(1 << (irq & 7));
    __asm__ volatile ("outb %0, %1" : : "a"(mask), "dN"(port));

    // SI LA IRQ ES DEL PIC SECUNDARIO (8-15), DESENMASCARAR IRQ 2 EN EL PIC PRIMARIO
    if (irq >= 8) {
        uint8_t master_mask;
        __asm__ volatile ("inb $0x21, %0" : "=a"(master_mask));
        master_mask &= ~(1 << 2); // Bit 2 = IRQ2 (Cascada)
        __asm__ volatile ("outb %0, $0x21" : : "a"(master_mask));
    }
}

void irq_uninstall_handler(uint8_t irq) {
    irq_handlers[irq] = 0;
    uint16_t port = (irq < 8) ? 0x21 : 0xA1;
    uint8_t mask;
    __asm__ volatile ("inb %1, %0" : "=a"(mask) : "dN"(port));
    mask |= (1 << (irq & 7));
    __asm__ volatile ("outb %0, %1" : : "a"(mask), "dN"(port));
}

void idt_init(void) {
    __builtin_memset(&idt, 0, sizeof(idt));

    idt_set_gate(0,  (uint64_t)isr0,  0x08, 0x8E);
    idt_set_gate(1,  (uint64_t)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (uint64_t)isr2,  0x08, 0x8E);
    idt_set_gate(3,  (uint64_t)isr3,  0x08, 0x8E);
    idt_set_gate(4,  (uint64_t)isr4,  0x08, 0x8E);
    idt_set_gate(5,  (uint64_t)isr5,  0x08, 0x8E);
    idt_set_gate(6,  (uint64_t)isr6,  0x08, 0x8E);
    idt_set_gate(7,  (uint64_t)isr7,  0x08, 0x8E);
    idt_set_gate(8,  (uint64_t)isr8,  0x08, 0x8E);
    idt_set_gate(9,  (uint64_t)isr9,  0x08, 0x8E);
    idt_set_gate(10, (uint64_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint64_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint64_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint64_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint64_t)isr14, 0x08, 0x8E);
    idt_set_gate(15, (uint64_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint64_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint64_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint64_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint64_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint64_t)isr20, 0x08, 0x8E);
    idt_set_gate(21, (uint64_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint64_t)isr22, 0x08, 0x8E);
    idt_set_gate(23, (uint64_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint64_t)isr24, 0x08, 0x8E);
    idt_set_gate(25, (uint64_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint64_t)isr26, 0x08, 0x8E);
    idt_set_gate(27, (uint64_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint64_t)isr28, 0x08, 0x8E);
    idt_set_gate(29, (uint64_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint64_t)isr30, 0x08, 0x8E);
    idt_set_gate(31, (uint64_t)isr31, 0x08, 0x8E);

    idt_set_gate(32, (uint64_t)irq0,  0x08, 0x8E);
    idt_set_gate(33, (uint64_t)irq1,  0x08, 0x8E);
    idt_set_gate(34, (uint64_t)irq2,  0x08, 0x8E);
    idt_set_gate(35, (uint64_t)irq3,  0x08, 0x8E);
    idt_set_gate(36, (uint64_t)irq4,  0x08, 0x8E);
    idt_set_gate(37, (uint64_t)irq5,  0x08, 0x8E);
    idt_set_gate(38, (uint64_t)irq6,  0x08, 0x8E);
    idt_set_gate(39, (uint64_t)irq7,  0x08, 0x8E);
    idt_set_gate(40, (uint64_t)irq8,  0x08, 0x8E);
    idt_set_gate(41, (uint64_t)irq9,  0x08, 0x8E);
    idt_set_gate(42, (uint64_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint64_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint64_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint64_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint64_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint64_t)irq15, 0x08, 0x8E);

    pic_init();
    idt_load();
    serial_puts("[IDT] IDT y PIC inicializados (256 entradas)\n");
}

void isr_handler(uint64_t isr_num, uint64_t error_code) {
    serial_puts("\n[!] EXCEPCION #");
    if (isr_num >= 100) serial_putc('0' + (isr_num / 100));
    if (isr_num >= 10)  serial_putc('0' + ((isr_num / 10) % 10));
    serial_putc('0' + (isr_num % 10));
    serial_puts(": ");
    if (isr_num < 32) {
        serial_puts(exception_names[isr_num]);
    } else {
        serial_puts("Desconocida");
    }
    serial_puts(" | Error code: ");
    serial_hex(error_code);
    serial_puts("\n[!] Sistema detenido.\n");
    __asm__ volatile ("cli; hlt");
}

void irq_handler(uint64_t irq_num) {
    uint8_t irq = (uint8_t)irq_num;
    // Enviamos EOI ANTES del handler para que el PIC pueda recibir
    // el siguiente tick mientras las tareas corren (necesario para preemption).
    pic_eoi(irq);
    if (irq < 16 && irq_handlers[irq]) {
        irq_handlers[irq]();
    }
}
