// kernel/gdt.c
// GDT y TSS para x86_64

#include "gdt.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// Estructuras GDT
// ---------------------------------------------------------------------------
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

// El descriptor TSS en 64-bit ocupa 16 bytes (dos slots de 8 en la GDT)
struct tss_descriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;     // 0x89 = Present, Type=TSS Available
    uint8_t  limit_high; // bits [19:16] del limit + flags
    uint8_t  base_high;
    uint32_t base_upper; // bits [63:32] de la base
    uint32_t reserved;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// ---------------------------------------------------------------------------
// TSS de 64-bit (104 bytes, especificacion Intel Vol.3 §7.7)
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;      // Stack de kernel para CPL=0 (interrupciones desde usuario)
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];    // Interrupt Stack Table
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss64_t;

// ---------------------------------------------------------------------------
// Datos globales
// ---------------------------------------------------------------------------
static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gdt_ptr;
static tss64_t          tss;

// Stack dedicado para interrupciones (IST1), 8KB
static uint8_t ist1_stack[8192] __attribute__((aligned(16)));

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void gdt_set_gate(int num, uint64_t base, uint32_t limit,
                         uint8_t access, uint8_t gran) {
    gdt[num].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt[num].base_low    = (uint16_t)(base  & 0xFFFF);
    gdt[num].base_mid    = (uint8_t)((base >> 16) & 0xFF);
    gdt[num].access      = access;
    gdt[num].granularity = (gran & 0xF0) | ((limit >> 16) & 0x0F);
    gdt[num].base_high   = (uint8_t)((base >> 24) & 0xFF);
}

static void gdt_set_tss(int num, uint64_t base, uint32_t limit) {
    struct tss_descriptor *desc = (struct tss_descriptor *)&gdt[num];
    desc->limit_low   = (uint16_t)(limit & 0xFFFF);
    desc->base_low    = (uint16_t)(base  & 0xFFFF);
    desc->base_mid    = (uint8_t)((base >> 16) & 0xFF);
    desc->access      = 0x89;  // Present | TSS Available (64-bit)
    desc->limit_high  = (uint8_t)((limit >> 16) & 0x0F);
    desc->base_high   = (uint8_t)((base >> 24) & 0xFF);
    desc->base_upper  = (uint32_t)(base >> 32);
    desc->reserved    = 0;
}

// ---------------------------------------------------------------------------
// API publica
// ---------------------------------------------------------------------------
void gdt_init(void) {
    gdt_set_gate(0, 0, 0,      0,    0);        // Null
    gdt_set_gate(1, 0, 0,      0x9A, 0xA0);     // Kernel Code (64-bit)
    gdt_set_gate(2, 0, 0,      0x92, 0x80);     // Kernel Data
    gdt_set_gate(3, 0, 0,      0xFA, 0xA0);     // User Code (64-bit)
    gdt_set_gate(4, 0, 0,      0xF2, 0x80);     // User Data

    // TSS — descriptor de 16 bytes en slots 5 y 6
    __builtin_memset(&tss, 0, sizeof(tss));
    tss.iopb_offset = sizeof(tss64_t);  // Sin I/O bitmap
    // IST1: stack para excepciones criticas (Double Fault, NMI, etc.)
    tss.ist[0] = (uint64_t)(ist1_stack + sizeof(ist1_stack));

    gdt_set_tss(5, (uint64_t)&tss, sizeof(tss64_t) - 1);

    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (uint64_t)&gdt;

    __asm__ volatile (
        "lgdt (%0)\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %%ax, %%ss\n"
        :
        : "r"(&gdt_ptr)
        : "rax", "memory"
    );

    // Cargar TSS (selector = 0x28, TSS_SEG)
    __asm__ volatile ("ltr %%ax" : : "a"((uint16_t)TSS_SEG));
}

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}
