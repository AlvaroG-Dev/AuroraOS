// kernel/gdt.h
// Global Descriptor Table para modo largo x86_64

#ifndef GDT_H
#define GDT_H

#include "cpu.h"
#include <stdint.h>

// Disposición de los selectores de usuario igual que Linux x86_64 (Wine en
// modo WoW64 salta a código de 32 bits con %cs = 0x23 y vuelve a 0x33; los
// programas leen estos valores en sigcontext):
//   0x08 kernel CS   0x10 kernel DS
//   0x23 user CS 32 bits   0x2B user DS   0x33 user CS 64 bits
//   0x63/0x6B/0x73 TLS de 32 bits (set_thread_area), una copia por tarea
//   0x80.. TSS de cada CPU (16 bytes)
// Cada CPU tiene su propia GDT (las entradas TLS cambian con la tarea).
#define GDT_TLS_MIN 12
#define GDT_TLS_ENTRIES 3
#define GDT_TSS_FIRST 16
#define GDT_ENTRIES (GDT_TSS_FIRST + 2 * MAX_CPUS)

// Selectores
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER32_CS 0x20
#define USER_DS 0x28
#define USER_CS 0x30
// STAR[63:48]: SYSRET carga CS = base+16 y SS = base+8 (como Linux, 0x23).
#define USER_STAR_BASE USER32_CS
#define USER32_CS_RING3 (USER32_CS | 3) // 0x23
#define USER_DS_RING3 (USER_DS | 3)     // 0x2B
#define USER_CS_RING3 (USER_CS | 3)     // 0x33

// Selector TSS para un CPU dado
#define TSS_SELECTOR(cpu) ((uint16_t)((GDT_TSS_FIRST + ((cpu) * 2)) * 8))
#define TSS_SEG TSS_SELECTOR(0)

void gdt_init(void);
void gdt_ap_init(int cpu_id);
void tss_set_rsp0(uint64_t rsp0);

struct task;
// Copia las entradas TLS de `t` a la GDT de esta CPU (antes de cambiar a la
// tarea). Si t->fs_sel apunta a una entrada TLS vacía, lo pone a 0.
void gdt_load_tls(struct task *t);
// set_thread_area / get_thread_area (struct user_desc de Linux).
int64_t gdt_set_thread_area(struct task *t, void *user_desc_uptr);
int64_t gdt_get_thread_area(struct task *t, void *user_desc_uptr);

#endif
