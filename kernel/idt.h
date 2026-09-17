// kernel/idt.h
#ifndef IDT_H
#define IDT_H

#include <stdint.h>

#define IDT_ENTRIES 256

typedef struct {
  uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
  uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;

  uint64_t int_num;
  uint64_t error_code;

  uint64_t rip;
  uint64_t cs;
  uint64_t rflags;
  uint64_t rsp;
  uint64_t ss;
} __attribute__((packed)) registers_t;

void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);
void pic_init(void);
void pic_eoi(uint8_t irq);
void irq_install_handler(uint8_t irq, void (*handler)(void));
void irq_uninstall_handler(uint8_t irq);
void idt_load(void);

void isr_handler(registers_t *regs);
void irq_handler(registers_t *regs);

#endif