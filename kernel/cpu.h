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
#define EFER_SCE 1
#define EFER_NXE (1ULL << 11)

// MSR_SFMASK (0xC0000084): máscara de RFLAGS aplicada al entrar en SYSCALL.
#define MSR_SFMASK_BITS 0x277CULL

// ---------------------------------------------------------------------------
// SMP: soporte per-CPU.
//
// MAX_CPUS debe cubrir los hilos lógicos reales. Un Alder Lake / Raptor
// Lake tiene hasta 24 núcleos (8 P + 16 E) → 32 hilos con HT. 64 cubre
// cualquier desktop/laptop actual y no consume un .bss exagerado
// (cpu_local_t son 40 bytes → 2.5 KB con 64).
// ---------------------------------------------------------------------------
#define MAX_CPUS 64

#define CPU_LOCAL_CPU_ID_OFFSET 16

static inline int smp_processor_id(void) {
  int id;
  __asm__ volatile("movl %%gs:%c1, %0"
                   : "=r"(id)
                   : "i"(CPU_LOCAL_CPU_ID_OFFSET));
  return id;
}

typedef struct {
  uint64_t kernel_stack;
  uint64_t user_rsp;
  int cpu_id;
  int lapic_id;
  void *current_task;
  uint64_t ticks_since_resched;
} cpu_local_t;

extern cpu_local_t cpu_local_data[MAX_CPUS];

#define this_cpu(field) (cpu_local_data[smp_processor_id()].field)

// Tarea actual de esta CPU con UNA instruccion relativa a %gs. this_cpu()
// hace dos lecturas (cpu_id y luego cpu_local_data[cpu]): si la tarea se
// expulsa y migra entre ambas, obtiene la tarea actual de su CPU anterior.
// Una sola instruccion no se puede partir, asi que el resultado es siempre
// la tarea que la ejecuta (como `current` en Linux).
#define CPU_LOCAL_CURRENT_TASK_OFFSET 24
static inline void *this_cpu_current_task(void) {
  void *t;
  __asm__ volatile("movq %%gs:%c1, %0"
                   : "=r"(t)
                   : "i"(CPU_LOCAL_CURRENT_TASK_OFFSET));
  return t;
}
#define per_cpu(field, cpu) (cpu_local_data[cpu].field)

extern int cpu_smap_enabled;

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

static inline void stac(void) {
  if (cpu_smap_enabled)
    __asm__ volatile("stac" ::: "memory");
}
static inline void clac(void) {
  if (cpu_smap_enabled)
    __asm__ volatile("clac" ::: "memory");
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
  int xsave_enabled;
  int avx512_enabled;
  int level;
  // -----------------------------------------------------------------------
  // [Power Management] Solo detección; la aplicación va aparte.
  // -----------------------------------------------------------------------
  int has_hwp;           // CPUID.6:EAX[7]
  int has_hwp_feedback;  // CPUID.6:EAX[13]
  int hwp_epp_supported; // CPUID.6:EAX[22]
  int has_aperf_mperf;   // CPUID.6:ECX[0]  ← NUEVO
  int has_eist;          // IA32_MISC_ENABLE[16], leído tras GDT/IDT
  uint32_t pm_base_mhz;  // CPUID.16
  uint32_t pm_max_mhz;
  uint32_t pm_bus_mhz;
  // Rellenados por cpu_apply_pm_policy():
  int hwp_active;    // 1 si HWP quedó habilitado en este boot
  int pstate_forced; // 1 si forzamos IA32_PERF_CTL
  int hwp_lowest;
  int hwp_guaranteed;
  int hwp_highest;
  int hwp_efficient;
} cpu_features_t;

extern cpu_features_t g_cpu_features;

#define CPU_SIMD_SCALAR 0
#define CPU_SIMD_SSE2 1
#define CPU_SIMD_SSE41 2
#define CPU_SIMD_AVX2 3
#define CPU_SIMD_AVX512 4

void cpu_simd_init(void);

// ---------------------------------------------------------------------------
// [Power Management] Split detección / aplicación:
//
//   cpu_detect_pm()       — solo CPUID + lectura de MSR sin efectos. Se llama
//                           en kmain, antes de pmm_init/paging_init. Nunca
//                           escribe nada, así que no puede fallar.
//
//   cpu_apply_pm_policy() — decide qué mecanismo usar (HWP / IA32_PERF_CTL
//                           / ACPI _PSS / ninguno) y lo aplica. Se llama
//                           en kmain después de ACPI y en kmain_task una
//                           vez que el kernel está maduro. Puede escribir
//                           MSRs y por tanto puede fallar; requiere que la
//                           IDT esté ya cargada.
//
//   cpu_hwp_init_ap()     — versión reducida para APs. Solo activa HWP
//                           o fuerza el ratio si la política ya fue
//                           elegida por el BSP. Se llama desde ap_entry
//                           DESPUÉS de idt_load().
//
//   cpu_dump_cpuid_diag() — dump completo de CPUID/MSR para diagnóstico.
//                           Solo lectura. Se llama desde cpu_detect_pm().
// ---------------------------------------------------------------------------
void cpu_detect_pm(void);
void cpu_apply_pm_policy(void);
void cpu_hwp_init_ap(void);
void cpu_dump_cpuid_diag(void);

static inline int cpu_has_sse2(void) { return g_cpu_features.sse2; }
static inline int cpu_has_sse41(void) { return g_cpu_features.sse41; }
static inline int cpu_has_avx2(void) {
  return g_cpu_features.level >= CPU_SIMD_AVX2;
}
static inline int cpu_has_avx512(void) {
  return g_cpu_features.level >= CPU_SIMD_AVX512;
}

_Static_assert(offsetof(cpu_local_t, kernel_stack) == 0,
               "syscall_entry.asm lee [gs:0] como kernel_stack");
_Static_assert(offsetof(cpu_local_t, user_rsp) == 8,
               "syscall_entry.asm lee [gs:8] como user_rsp");
_Static_assert(offsetof(cpu_local_t, cpu_id) == CPU_LOCAL_CPU_ID_OFFSET,
               "smp_processor_id() lee %gs:cpu_id");
_Static_assert(offsetof(cpu_local_t, current_task) ==
                   CPU_LOCAL_CURRENT_TASK_OFFSET,
               "this_cpu_current_task() lee %gs:current_task");

#endif