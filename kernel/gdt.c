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

// Una GDT por CPU: las entradas TLS de 32 bits son de la tarea en curso.
static struct gdt_entry gdt_cpu[MAX_CPUS][GDT_ENTRIES] __attribute__((aligned(16)));
static struct gdt_ptr gdt_ptrs[MAX_CPUS];
#define gdt gdt_cpu[0]
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
  memset(gdt_cpu, 0, sizeof(gdt_cpu));
  gdt_set_gate(0, 0, 0, 0, 0);             // null
  gdt_set_gate(1, 0, 0, 0x9A, 0xA0);       // KERNEL_CS = 0x08
  gdt_set_gate(2, 0, 0, 0x92, 0x80);       // KERNEL_DS = 0x10
  // Usuario como Linux: límite 4 GiB (en modo compatibilidad de 32 bits los
  // límites de CS/DS/SS se comprueban).
  gdt_set_gate(4, 0, 0xFFFFF, 0xFA, 0xC0); // USER32_CS = 0x23 (D=1, L=0)
  gdt_set_gate(5, 0, 0xFFFFF, 0xF2, 0xC0); // USER_DS   = 0x2B
  gdt_set_gate(6, 0, 0xFFFFF, 0xFA, 0xA0); // USER_CS   = 0x33 (L=1)

  for (int i = 0; i < MAX_CPUS; i++) {
    memset(&tss_table[i], 0, sizeof(tss64_t));
    tss_table[i].iopb_offset = sizeof(tss64_t);
    tss_table[i].ist[0] =
        (uint64_t)(&ist1_stacks[i][0] + sizeof(ist1_stacks[i]));
    gdt_set_tss(GDT_TSS_FIRST + 2 * i, (uint64_t)&tss_table[i],
                sizeof(tss64_t) - 1);
  }
  // Copia para cada CPU antes del ltr (ltr marca el TSS como ocupado solo en
  // la GDT en la que se carga).
  for (int c = 0; c < MAX_CPUS; c++) {
    if (c)
      memcpy(gdt_cpu[c], gdt_cpu[0], sizeof(gdt_cpu[0]));
    gdt_ptrs[c].limit = sizeof(gdt_cpu[0]) - 1;
    gdt_ptrs[c].base = (uint64_t)&gdt_cpu[c][0];
  }
  struct gdt_ptr *gp = &gdt_ptrs[0];

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
                   : "r"(gp)
                   : "rax", "memory");

  // Cargar el selector GS puede sustituir la base per-CPU configurada
  // en el punto de entrada. Restaurarla antes de que sched_current() lea
  // cpu_id mediante %gs.
  wrmsr(MSR_GS_BASE, (uint64_t)&cpu_local_data[0]);
  wrmsr(MSR_KERNEL_GS_BASE, 0);

  uint16_t bsp_tss = TSS_SELECTOR(0);
  __asm__ volatile("ltr %%ax" : : "a"(bsp_tss));
}

void gdt_ap_init(int cpu_id) {
  struct gdt_ptr *gp =
      &gdt_ptrs[(cpu_id >= 0 && cpu_id < MAX_CPUS) ? cpu_id : 0];
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
                   : "r"(gp)
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

// ---------------------------------------------------------------------------
// TLS de 32 bits (set_thread_area): Wine WoW64 pone en %fs el TEB de 32 bits.
// ---------------------------------------------------------------------------
#include "sched.h"
#include "uaccess.h"

struct user_desc_l {
  uint32_t entry_number;
  uint32_t base_addr;
  uint32_t limit;
  uint32_t flags; // seg_32bit:1 contents:2 read_exec_only:1 limit_in_pages:1
                  // seg_not_present:1 useable:1 lm:1
};

static uint64_t desc_pack(const struct user_desc_l *u) {
  uint32_t f = u->flags;
  int seg32 = f & 1, contents = (f >> 1) & 3, roe = (f >> 3) & 1,
      pages = (f >> 4) & 1, notp = (f >> 5) & 1, useable = (f >> 6) & 1;
  // Descriptor vacío (como LDT_empty de Linux): se borra la entrada.
  if (u->base_addr == 0 && u->limit == 0 && contents == 0 && roe == 1 &&
      seg32 == 0 && pages == 0 && notp == 1 && useable == 0)
    return 0;
  if (u->base_addr == 0 && u->limit == 0 && f == 0)
    return 0;
  uint64_t lo = (u->limit & 0xFFFF) | ((uint64_t)(u->base_addr & 0xFFFF) << 16);
  uint64_t hi = ((u->base_addr >> 16) & 0xFF) | (u->base_addr & 0xFF000000) |
                (u->limit & 0xF0000) | ((uint32_t)(roe ^ 1) << 9) |
                ((uint32_t)contents << 10) | ((uint32_t)(notp ^ 1) << 15) |
                ((uint32_t)seg32 << 22) | ((uint32_t)pages << 23) |
                ((uint32_t)useable << 20) | 0x7000; // S=1, DPL=3
  return lo | (hi << 32);
}

static void desc_unpack(uint64_t d, struct user_desc_l *u) {
  uint32_t lo = (uint32_t)d, hi = (uint32_t)(d >> 32);
  u->base_addr = (lo >> 16) | ((hi & 0xFF) << 16) | (hi & 0xFF000000);
  u->limit = (lo & 0xFFFF) | (hi & 0xF0000);
  uint32_t f = 0;
  if (!d) {
    f = (1 << 3) | (1 << 5); // read_exec_only + not_present (vacío)
  } else {
    f |= (hi >> 22) & 1;                  // seg_32bit
    f |= ((hi >> 10) & 3) << 1;           // contents
    f |= (((hi >> 9) & 1) ^ 1) << 3;      // read_exec_only
    f |= ((hi >> 23) & 1) << 4;           // limit_in_pages
    f |= (((hi >> 15) & 1) ^ 1) << 5;     // seg_not_present
    f |= ((hi >> 20) & 1) << 6;           // useable
  }
  u->flags = f;
}

void gdt_load_tls(struct task *t) {
  int cpu = smp_processor_id();
  if (cpu < 0 || cpu >= MAX_CPUS)
    cpu = 0;
  uint64_t *g = (uint64_t *)&gdt_cpu[cpu][GDT_TLS_MIN];
  for (int i = 0; i < GDT_TLS_ENTRIES; i++)
    g[i] = t ? t->tls_desc[i] : 0;
  if (t && t->fs_sel) {
    int idx = t->fs_sel >> 3;
    // Solo se recargan selectores de usuario de la GDT que sigan siendo
    // válidos (una entrada TLS borrada daría #GP en el kernel).
    if ((t->fs_sel & 7) != 3 ||
        (idx >= GDT_TLS_MIN && idx < GDT_TLS_MIN + GDT_TLS_ENTRIES &&
         !(t->tls_desc[idx - GDT_TLS_MIN] & (1ULL << 47))) ||
        (idx != 4 && idx != 5 && idx != 6 &&
         !(idx >= GDT_TLS_MIN && idx < GDT_TLS_MIN + GDT_TLS_ENTRIES)))
      t->fs_sel = 0;
  }
}

int64_t gdt_set_thread_area(struct task *t, void *uptr) {
  struct user_desc_l u;
  if (!t || copy_from_user(&u, uptr, sizeof(u)) < 0)
    return -EFAULT;
  int idx = (int)u.entry_number;
  if (idx == -1) {
    for (int i = 0; i < GDT_TLS_ENTRIES; i++)
      if (!t->tls_desc[i]) {
        idx = GDT_TLS_MIN + i;
        break;
      }
    if (idx == -1)
      return -ESRCH;
    u.entry_number = (uint32_t)idx;
    if (copy_to_user(uptr, &u, sizeof(u)) < 0)
      return -EFAULT;
  }
  if (idx < GDT_TLS_MIN || idx >= GDT_TLS_MIN + GDT_TLS_ENTRIES)
    return -EINVAL;
  if ((u.flags >> 7) & 1) // lm: no en la GDT
    return -EINVAL;
  uint64_t d = desc_pack(&u);
  // Un segmento de código conforme o de sistema no se admite.
  if (d && ((u.flags >> 1) & 3) == 3 && !((u.flags >> 5) & 1))
    return -EINVAL;
  t->tls_desc[idx - GDT_TLS_MIN] = d;
  if (t == sched_current()) {
    uint64_t fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl)::"memory");
    gdt_load_tls(t);
    __asm__ volatile("push %0; popfq" ::"r"(fl) : "memory", "cc");
  }
  return 0;
}

int64_t gdt_get_thread_area(struct task *t, void *uptr) {
  struct user_desc_l u;
  if (!t || copy_from_user(&u, uptr, sizeof(u)) < 0)
    return -EFAULT;
  int idx = (int)u.entry_number;
  if (idx < GDT_TLS_MIN || idx >= GDT_TLS_MIN + GDT_TLS_ENTRIES)
    return -EINVAL;
  desc_unpack(t->tls_desc[idx - GDT_TLS_MIN], &u);
  if (copy_to_user(uptr, &u, sizeof(u)) < 0)
    return -EFAULT;
  return 0;
}
