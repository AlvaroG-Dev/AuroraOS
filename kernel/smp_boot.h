// kernel/smp_boot.h
#ifndef KERNEL_SMP_BOOT_H
#define KERNEL_SMP_BOOT_H

#include "acpi.h"
#include "cpu.h"
#include <stdint.h>

#define SMP_TRAMPOLINE_PHYS 0x7000ULL
#define SMP_TRAMPOLINE_VECTOR ((uint8_t)(SMP_TRAMPOLINE_PHYS >> 12))
#define SMP_BOOT_PARAMS_PHYS 0x8000ULL

// ---------------------------------------------------------------------------
// Header fijo del trampoline en SMP_TRAMPOLINE_PHYS (0x7000).
//
// IMPORTANTE: smp_trampoline.asm lee estos campos con direcciones
// absolutas (0x7008, 0x7010, 0x7018, 0x7020). Si añades o cambias
// campos, actualiza también el asm.
//
//   Offset 0x00-0x07: jmp short + align (no tocar, es código)
//   Offset 0x08:      pml4_phys   (uint64_t)
//   Offset 0x10:      entry_virt  (uint64_t)
//   Offset 0x18:      stack_top   (uint64_t)
//   Offset 0x20:      ap_index    (uint64_t)  <- índice ACPI del AP
//
// Después del header (offset 0x28) empieza trampoline_code.
// ---------------------------------------------------------------------------
typedef struct {
  uint8_t jmp_opcode[8]; // jmp short + align (0x00..0x07)
  uint64_t pml4_phys;    // offset 0x08
  uint64_t entry_virt;   // offset 0x10
  uint64_t stack_top;    // offset 0x18
  uint64_t ap_index;     // offset 0x20 (índice ACPI del AP)
} __attribute__((packed)) smp_trampoline_header_t;

typedef struct {
  uint64_t ap_stack_top[MAX_CPUS]; // indexado por índice ACPI, NO por APIC ID
  volatile int aps_started;
  volatile int aps_ready;
  volatile uint32_t bsp_apic_id;
  volatile uint32_t next_ap_apic_id; // solo para debug/logs
} smp_boot_params_t;

extern smp_boot_params_t *smp_boot_params;
void smp_boot_init(void);
void smp_boot_aps(void);
void ap_entry(void);
int smp_aps_ready(void);
extern volatile int smp_sched_active;

#endif