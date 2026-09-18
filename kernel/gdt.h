// kernel/gdt.h
// Global Descriptor Table para modo largo x86_64

#ifndef GDT_H
#define GDT_H

#include "cpu.h"
#include <stdint.h>

// 5 entradas fijas + 2 slots por CPU para su descriptor TSS (16 bytes)
#define GDT_ENTRIES (5 + 2 * MAX_CPUS)

// Selectores
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_DS   0x18
#define USER_CS   0x20
#define USER_DS_RING3 (USER_DS | 3)
#define USER_CS_RING3 (USER_CS | 3)

// Selector TSS para un CPU dado
#define TSS_SELECTOR(cpu) ((uint16_t)((5 + ((cpu) * 2)) * 8))
#define TSS_SEG TSS_SELECTOR(0)

void gdt_init(void);
void gdt_ap_init(int cpu_id);
void tss_set_rsp0(uint64_t rsp0);

#endif
