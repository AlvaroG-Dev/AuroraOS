// kernel/idt.h
#ifndef IDT_H
#define IDT_H

#include <stddef.h>
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
void msi_handler(registers_t *regs);

// Instala un handler que se ejecuta en dos fases:
//   ack:     se llama ANTES del EOI. Debe silenciar la fuente de IRQ
//            (limpiar PxIS, leer el registro de status, etc.).
//   process: se llama DESPUÉS del EOI. Hace el trabajo real.
//
// Si ack es NULL, se usa process como único handler (comportamiento
// clásico, EOI después).
void irq_install_handler_ex(uint8_t irq, void (*ack)(void),
                            void (*process)(void));

// Rango de vectores MSI (0x60-0x6F). 16 vectores disponibles.
#define MSI_VECTOR_BASE 0x60
#define MSI_VECTOR_COUNT 16
#define MSI_VECTOR_END (MSI_VECTOR_BASE + MSI_VECTOR_COUNT - 1)

// Instala un handler para un vector MSI.
// El handler recibe el vector y lo usa para indexar.
void msi_install_handler(uint8_t vector, void (*handler)(void));

// Wrapper que llama al handler MSI correspondiente.
// Lo usa el dispatcher de IRQ cuando el int_num está en el rango MSI.
void msi_dispatch(uint8_t vector);

// ---------------------------------------------------------------------------
// Asserts del layout de registers_t frente al orden de pushes de
// isr_stubs.asm.
//
// isr_common/irq_common hacen, en orden:
//   push rax, rcx, rdx, rbx, rbp, rsi, rdi,
//        r8, r9, r10, r11, r12, r13, r14, r15, r15(dummy)
// y luego pasan rdi = rsp + 8 (saltando el dummy).
//
// Los offsets que importan (por orden de aparición):
// ---------------------------------------------------------------------------
_Static_assert(offsetof(registers_t, r15) == 0x00, "isr_stubs: r15");
_Static_assert(offsetof(registers_t, r14) == 0x08, "isr_stubs: r14");
_Static_assert(offsetof(registers_t, r13) == 0x10, "isr_stubs: r13");
_Static_assert(offsetof(registers_t, r12) == 0x18, "isr_stubs: r12");
_Static_assert(offsetof(registers_t, r11) == 0x20, "isr_stubs: r11");
_Static_assert(offsetof(registers_t, r10) == 0x28, "isr_stubs: r10");
_Static_assert(offsetof(registers_t, r9) == 0x30, "isr_stubs: r9");
_Static_assert(offsetof(registers_t, r8) == 0x38, "isr_stubs: r8");
_Static_assert(offsetof(registers_t, rdi) == 0x40, "isr_stubs: rdi");
_Static_assert(offsetof(registers_t, rsi) == 0x48, "isr_stubs: rsi");
_Static_assert(offsetof(registers_t, rbp) == 0x50, "isr_stubs: rbp");
_Static_assert(offsetof(registers_t, rbx) == 0x58, "isr_stubs: rbx");
_Static_assert(offsetof(registers_t, rdx) == 0x60, "isr_stubs: rdx");
_Static_assert(offsetof(registers_t, rcx) == 0x68, "isr_stubs: rcx");
_Static_assert(offsetof(registers_t, rax) == 0x70, "isr_stubs: rax");
_Static_assert(offsetof(registers_t, int_num) == 0x78, "isr_stubs: int_num");
_Static_assert(offsetof(registers_t, error_code) == 0x80,
               "isr_stubs: error_code");
_Static_assert(offsetof(registers_t, rip) == 0x88, "hw: rip");
_Static_assert(offsetof(registers_t, cs) == 0x90, "hw: cs");
_Static_assert(offsetof(registers_t, rflags) == 0x98, "hw: rflags");
_Static_assert(offsetof(registers_t, rsp) == 0xA0, "hw: rsp");
_Static_assert(offsetof(registers_t, ss) == 0xA8, "hw: ss");

#endif
