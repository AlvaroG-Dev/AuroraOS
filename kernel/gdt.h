// kernel/gdt.h
// Global Descriptor Table para modo largo x86_64

#ifndef GDT_H
#define GDT_H

#include <stdint.h>

#define GDT_ENTRIES 7   // 5 normales + 2 para el TSS de 64 bits (16 bytes)

// Selectores
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_DS   0x18
#define USER_CS   0x20
#define USER_DS_RING3 (USER_DS | 3)
#define USER_CS_RING3 (USER_CS | 3)
#define TSS_SEG   0x28

void gdt_init(void);
void tss_set_rsp0(uint64_t rsp0);

#endif
