// kernel/cpu.h
#ifndef CPU_H
#define CPU_H

#include <stdint.h>

#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_CSTAR 0xC0000083
#define MSR_SFMASK 0xC0000084
#define MSR_FS_BASE 0xC0000100
#define MSR_GS_BASE 0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

// EFER (Extended Feature Enable Register)
#define MSR_EFER 0xC0000080
#define EFER_SCE 1 // System Call Extensions
#define EFER_NXE (1ULL << 11)

// ---------------------------------------------------------------------------
// MSR_SFMASK (0xC0000084): máscara de RFLAGS aplicada al entrar en SYSCALL.
//
// Cuando se ejecuta SYSCALL, el CPU hace:
//     RFLAGS &= ~SFMASK
// antes de saltar a LSTAR. Es decir: los bits puestos a 1 en SFMASK
// se limpian en el RFLAGS del kernel.
//
// 0x257C = 0000 0000 0010 0101 0111 1100
//
//   Bit  2  (0x00004)  PF   - Parity Flag
//   Bit  3  (0x00008)  (reservado, 1)
//   Bit  4  (0x00010)  AF   - Auxiliary Carry Flag
//   Bit  5  (0x00020)  (reservado, 1)
//   Bit  6  (0x00040)  ZF   - Zero Flag
//   Bit  7  (0x00080)  SF   - Sign Flag
//   Bit  8  (0x00100)  TF   - Trap Flag (evita single-step en kernel)
//   Bit 10  (0x00400)  DF   - Direction Flag (garantiza forward en REP)
//   Bit 12  (0x01000)  IOPL - bit 0
//   Bit 13  (0x02000)  IOPL - bit 1  → IOPL = 0 (sin port I/O)
//
// No se limpia IF (bit 9, 0x200): queremos que las interrupciones sigan
// llegando durante el handler de syscall.
// No se limpia CF (bit 0) ni OF (bit 11) porque Linux tampoco lo hace
// y algunos paths los usan. En la práctica no importa.
//
// Este valor es el mismo que usa Linux (arch/x86/entry/entry_64.S).
// ---------------------------------------------------------------------------
#define MSR_SFMASK_BITS 0x257CULL

// ---------------------------------------------------------------------------
// SMP: soporte per-CPU.
//
// Hoy solo hay una CPU. Estas macros permiten escribir código que ya es
// SMP-ready sin tener que refactorizarlo después. Cuando se implemente
// SMP de verdad:
//   - MAX_CPUS se sube a 8/16/64.
//   - smp_processor_id() lee el APIC ID real.
//   - cpu_local (variable global) se migra a cpu_local_data[MAX_CPUS]
//     accedido vía %gs:.
//
// Mientras tanto, todo es no-op.
// ---------------------------------------------------------------------------

// Número máximo de CPUs. Hoy 1.
#define MAX_CPUS 1

// ID de la CPU actual. Hoy siempre 0.
// Con SMP: lee el APIC ID del LAPIC (x2APIC MSR o MMIO).
static inline int smp_processor_id(void) { return 0; }

// Acceso a una variable per-CPU. Hoy no hay array; se documenta el
// patrón para cuando lo haya. De momento sirve como documentación.
//
// Uso futuro:
//   extern cpu_local_t cpu_local_data[MAX_CPUS];
//   #define this_cpu(field)  (cpu_local_data[smp_processor_id()].field)
//   #define per_cpu(field, cpu)  (cpu_local_data[cpu].field)
//
// Hoy, para variables globales únicas, no se usa.

// ---------------------------------------------------------------------------
// Estructura per-CPU. Hoy hay una sola instancia global (cpu_local).
// Cuando se implemente SMP, se migrará a un array cpu_local_data[MAX_CPUS]
// y cada CPU tendrá su copia accedida por %gs:.
// ---------------------------------------------------------------------------
typedef struct {
  uint64_t kernel_stack;
  uint64_t user_rsp;
} cpu_local_t;

extern cpu_local_t cpu_local;
extern int cpu_smap_enabled; // 1 si SMAP está activo

static inline void wrmsr(uint32_t msr, uint64_t val) {
  uint32_t low = (uint32_t)val;
  uint32_t high = (uint32_t)(val >> 32);
  __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
  uint32_t low, high;
  __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
  return ((uint64_t)high << 32) | low;
}

// ---------------------------------------------------------------------------
// CPU feature detection (CPUID)
// ---------------------------------------------------------------------------

static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax,
                         uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
  __asm__ volatile("cpuid"
                   : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                   : "a"(leaf), "c"(subleaf));
}

static inline int cpu_has_nx(void) {
  uint32_t eax, ebx, ecx, edx;
  cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx);
  return (edx >> 20) & 1;
}

static inline int cpu_has_smep(void) {
  uint32_t eax, ebx, ecx, edx;
  cpuid(7, 0, &eax, &ebx, &ecx, &edx);
  return (ebx >> 7) & 1;
}

static inline int cpu_has_smap(void) {
  uint32_t eax, ebx, ecx, edx;
  cpuid(7, 0, &eax, &ebx, &ecx, &edx);
  return (ebx >> 20) & 1;
}

// ---------------------------------------------------------------------------
// Control registers
// ---------------------------------------------------------------------------

static inline uint64_t read_cr0(void) {
  uint64_t v;
  __asm__ volatile("mov %%cr0, %0" : "=r"(v));
  return v;
}
static inline uint64_t read_cr2(void) {
  uint64_t v;
  __asm__ volatile("mov %%cr2, %0" : "=r"(v));
  return v;
}
static inline uint64_t read_cr3(void) {
  uint64_t v;
  __asm__ volatile("mov %%cr3, %0" : "=r"(v));
  return v;
}
static inline uint64_t read_cr4(void) {
  uint64_t v;
  __asm__ volatile("mov %%cr4, %0" : "=r"(v));
  return v;
}
static inline void write_cr3(uint64_t v) {
  __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}
static inline void write_cr4(uint64_t v) {
  __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}

// ---------------------------------------------------------------------------
// SMAP: stac / clac
// ---------------------------------------------------------------------------
static inline void stac(void) {
  if (cpu_smap_enabled) {
    __asm__ volatile("stac" ::: "memory");
  }
}

static inline void clac(void) {
  if (cpu_smap_enabled) {
    __asm__ volatile("clac" ::: "memory");
  }
}

#endif