// kernel/gdt.c
// GDT y TSS para x86_64

#include "gdt.h"
#include "cpu.h"
#include "string.h"
#include <stdint.h>

struct gdt_entry {
  uint16_t limit_low;
  uint16_t base_low;
  uint8_t base_mid;
  uint8_t access;
  uint8_t granularity;
  uint8_t base_high;
} __attribute__((packed));

struct tss_descriptor {
  uint16_t limit_low;
  uint16_t base_low;
  uint8_t base_mid;
  uint8_t access;
  uint8_t limit_high;
  uint8_t base_high;
  uint32_t base_upper;
  uint32_t reserved;
} __attribute__((packed));

struct gdt_ptr {
  uint16_t limit;
  uint64_t base;
} __attribute__((packed));

typedef struct __attribute__((packed)) {
  uint32_t reserved0;
  uint64_t rsp0;
  uint64_t rsp1;
  uint64_t rsp2;
  uint64_t reserved1;
  uint64_t ist[7];
  uint64_t reserved2;
  uint16_t reserved3;
  uint16_t iopb_offset;
} tss64_t;

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr gdt_ptr;
static tss64_t tss_table[MAX_CPUS];

static uint8_t ist1_stacks[MAX_CPUS][8192] __attribute__((aligned(16)));

static void gdt_set_gate(int num, uint64_t base, uint32_t limit, uint8_t access,
                         uint8_t gran) {
  gdt[num].limit_low = (uint16_t)(limit & 0xFFFF);
  gdt[num].base_low = (uint16_t)(base & 0xFFFF);
  gdt[num].base_mid = (uint8_t)((base >> 16) & 0xFF);
  gdt[num].access = access;
  gdt[num].granularity = (gran & 0xF0) | ((limit >> 16) & 0x0F);
  gdt[num].base_high = (uint8_t)((base >> 24) & 0xFF);
}

static void gdt_set_tss(int num, uint64_t base, uint32_t limit) {
  struct tss_descriptor *desc = (struct tss_descriptor *)&gdt[num];
  desc->limit_low = (uint16_t)(limit & 0xFFFF);
  desc->base_low = (uint16_t)(base & 0xFFFF);
  desc->base_mid = (uint8_t)((base >> 16) & 0xFF);
  desc->access = 0x89;
  desc->limit_high = (uint8_t)((limit >> 16) & 0x0F);
  desc->base_high = (uint8_t)((base >> 24) & 0xFF);
  desc->base_upper = (uint32_t)(base >> 32);
  desc->reserved = 0;
}

void gdt_init(void) {
  gdt_set_gate(0, 0, 0, 0, 0);
  gdt_set_gate(1, 0, 0, 0x9A, 0xA0);
  gdt_set_gate(2, 0, 0, 0x92, 0x80);
  gdt_set_gate(3, 0, 0, 0xF2, 0x80);
  gdt_set_gate(4, 0, 0, 0xFA, 0xA0);

  for (int i = 0; i < MAX_CPUS; i++) {
    memset(&tss_table[i], 0, sizeof(tss64_t));
    tss_table[i].iopb_offset = sizeof(tss64_t);
    tss_table[i].ist[0] = (uint64_t)(&ist1_stacks[i][0] + sizeof(ist1_stacks[i]));
    gdt_set_tss(5 + 2 * i, (uint64_t)&tss_table[i], sizeof(tss64_t) - 1);
  }

  gdt_ptr.limit = sizeof(gdt) - 1;
  gdt_ptr.base = (uint64_t)&gdt;

  __asm__ volatile("lgdt (%0)\n"
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
                   : "rax", "memory");

  uint16_t bsp_tss = TSS_SELECTOR(0);
  __asm__ volatile("ltr %%ax" : : "a"(bsp_tss));
}

void gdt_ap_init(int cpu_id) {
  __asm__ volatile("lgdt (%0)\n"
                   "pushq $0x08\n"
                   "leaq 1f(%%rip), %%rax\n"
                   "pushq %%rax\n"
                   "lretq\n"
                   "1:\n"
                   "movw $0x10, %%ax\n"
                   "movw %%ax, %%ds\n"
                   "movw %%ax, %%es\n"
                   "movw %%ax, %%fs\n"
                   "movw %%ax, %%ss\n"
                   :
                   : "r"(&gdt_ptr)
                   : "rax", "memory");

  if (cpu_id >= 0 && cpu_id < MAX_CPUS) {
    uint16_t ap_tss = TSS_SELECTOR(cpu_id);
    __asm__ volatile("ltr %%ax" : : "a"(ap_tss));
  }
}

void tss_set_rsp0(uint64_t rsp0) {
  int cpu = smp_processor_id();
  if (cpu >= 0 && cpu < MAX_CPUS) {
    tss_table[cpu].rsp0 = rsp0;
  } else {
    tss_table[0].rsp0 = rsp0;
  }
}
