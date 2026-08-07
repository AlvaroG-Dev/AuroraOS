// kernel/gdt.h
// Global Descriptor Table para modo largo x86_64

#ifndef GDT_H
#define GDT_H

#include <stdint.h>

#define GDT_ENTRIES 7   // 5 normales + 2 para el TSS de 64 bits (16 bytes)

// Selectores
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_CS   0x1B  // 0x18 | RPL3
#define USER_DS   0x23  // 0x20 | RPL3
#define TSS_SEG   0x28

void gdt_init(void);
void tss_set_rsp0(uint64_t rsp0);

#endif
