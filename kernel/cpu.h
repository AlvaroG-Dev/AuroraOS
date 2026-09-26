// kernel/cpu.h
#ifndef CPU_H
#define CPU_H

#include <stddef.h>
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
// ---------------------------------------------------------------------------
#define MSR_SFMASK_BITS 0x277CULL // + IF (0x200)

// ---------------------------------------------------------------------------
// SMP: soporte per-CPU.
//
// Fase 0 de SMP: MAX_CPUS = 8, pero solo hay 1 CPU corriendo.
// Fase 3.1: smp_processor_id() lee %gs:cpu_id.
// Fase 4: scheduler SMP completo.
// ---------------------------------------------------------------------------

// Número máximo de CPUs soportadas.
#define MAX_CPUS 8

// Offset de cpu_id dentro de cpu_local_t.
// cpu_local_t: kernel_stack (8) + user_rsp (8) + cpu_id (4) + ...
#define CPU_LOCAL_CPU_ID_OFFSET 16

// ID de la CPU actual. Lee %gs:cpu_id. El BSP tiene %gs apuntando a
// cpu_local_data[0] (tras smp_init). Cada AP configura %gs a su propia
// entrada en ap_entry. Requiere que %gs esté configurado; si no, lee
// basura. En el boot temprano (antes de smp_init) no llames a esto.
static inline int smp_processor_id(void) {
  int id;
  __asm__ volatile("movl %%gs:%c1, %0"
                   : "=r"(id)
                   : "i"(CPU_LOCAL_CPU_ID_OFFSET));
  return id;
}

// ---------------------------------------------------------------------------
// Estructura per-CPU.
// ---------------------------------------------------------------------------
typedef struct {
  uint64_t kernel_stack;        // stack de kernel para syscalls/interrupciones
  uint64_t user_rsp;            // RSP del usuario guardado al entrar en syscall
  int cpu_id;                   // índice 0..MAX_CPUS-1
  int lapic_id;                 // APIC ID del LAPIC
  void *current_task;           // task_t* actual (Fase 4, per-CPU)
  uint64_t ticks_since_resched; // contador de ticks desde el último resched
} cpu_local_t;

// Array de estructuras per-CPU.
extern cpu_local_t cpu_local_data[MAX_CPUS];

// cpu_local eliminada en Fase 4; todo migrado a cpu_local_data[].

// Acceso a un campo del CPU actual. Ej: this_cpu(kernel_stack).
#define this_cpu(field) (cpu_local_data[smp_processor_id()].field)

// Acceso a un campo de un CPU concreto. Ej: per_cpu(kernel_stack, 2).
#define per_cpu(field, cpu) (cpu_local_data[cpu].field)

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

// ============================================================================
// CPU feature detection + SIMD dispatch
// ============================================================================

typedef struct {
  // SSE family
  int sse2, sse3, ssse3, sse41, sse42;
  // AVX family
  int avx, avx2, fma;
  // AVX-512
  int avx512f, avx512bw, avx512vl, avx512dq;
  // OS state
  int osxsave;
  int xsave_enabled;  // XCR0 configurado (AVX usable)
  int avx512_enabled; // XCR0 bits 5..7 (ZMM usable)
  // Nivel de dispatch elegido (0..4)
  int level;
} cpu_features_t;

extern cpu_features_t g_cpu_features;

#define CPU_SIMD_SCALAR 0
#define CPU_SIMD_SSE2 1
#define CPU_SIMD_SSE41 2
#define CPU_SIMD_AVX2 3
#define CPU_SIMD_AVX512 4

void cpu_simd_init(void);

static inline int cpu_has_sse2(void) { return g_cpu_features.sse2; }
static inline int cpu_has_sse41(void) { return g_cpu_features.sse41; }
static inline int cpu_has_avx2(void) {
  return g_cpu_features.level >= CPU_SIMD_AVX2;
}
static inline int cpu_has_avx512(void) {
  return g_cpu_features.level >= CPU_SIMD_AVX512;
}

// ---------------------------------------------------------------------------
// Asserts de offsets asm↔C.
//
// syscall_entry.asm usa:
//   [gs:0]  → cpu_local_data[cpu].kernel_stack
//   [gs:8]  → cpu_local_data[cpu].user_rsp
//
// switch.asm usa:
//   task_t.rsp        (offset 0x00)
//   task_t.fpu_state  (offset 0x18)
//   task_t.cr3        (offset 0x20)
//
// Si cambias los structs, estos asserts te avisan en compilación.
// ---------------------------------------------------------------------------
_Static_assert(offsetof(cpu_local_t, kernel_stack) == 0,
               "syscall_entry.asm lee [gs:0] como kernel_stack");
_Static_assert(offsetof(cpu_local_t, user_rsp) == 8,
               "syscall_entry.asm lee [gs:8] como user_rsp");
_Static_assert(offsetof(cpu_local_t, cpu_id) == CPU_LOCAL_CPU_ID_OFFSET,
               "smp_processor_id() lee %gs:cpu_id");

#endif
