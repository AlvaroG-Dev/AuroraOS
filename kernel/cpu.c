// kernel/cpu.c
#include "cpu.h"
#include "klog.h"

cpu_features_t g_cpu_features = {0};

// ============================================================================
// CPUID + SIMD
// ============================================================================

static int cpuid_bit(uint32_t leaf, uint32_t sub, int reg, int bit) {
  uint32_t a, b, c, d;
  cpuid(leaf, sub, &a, &b, &c, &d);
  uint32_t v = (reg == 0) ? a : (reg == 1) ? b : (reg == 2) ? c : d;
  return (v >> bit) & 1;
}

static void enable_avx_xcr0(int avx512) {
  uint64_t cr4;
  __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
  cr4 |= (1ULL << 18);
  __asm__ volatile("movq %0, %%cr4" : : "r"(cr4));

  uint32_t lo, hi;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  lo |= 0x6;
  if (avx512)
    lo |= 0xE0;
  __asm__ volatile("xsetbv" : : "a"(lo), "d"(hi), "c"(0));
}

void cpu_simd_init(void) {
  g_cpu_features.sse2 = cpuid_bit(1, 0, 3, 26);
  g_cpu_features.sse3 = cpuid_bit(1, 0, 2, 0);
  g_cpu_features.ssse3 = cpuid_bit(1, 0, 2, 9);
  g_cpu_features.sse41 = cpuid_bit(1, 0, 2, 19);
  g_cpu_features.sse42 = cpuid_bit(1, 0, 2, 20);
  g_cpu_features.avx = cpuid_bit(1, 0, 2, 28);
  g_cpu_features.fma = cpuid_bit(1, 0, 2, 12);

  int has_xsave = cpuid_bit(1, 0, 2, 26);

  g_cpu_features.avx2 = cpuid_bit(7, 0, 1, 5);
  g_cpu_features.avx512f = cpuid_bit(7, 0, 1, 16);
  g_cpu_features.avx512dq = cpuid_bit(7, 0, 1, 17);
  g_cpu_features.avx512bw = cpuid_bit(7, 0, 1, 30);
  g_cpu_features.avx512vl = cpuid_bit(7, 0, 1, 31);

  LOG_INFO("[CPU] SSE2=%d SSE3=%d SSSE3=%d SSE4.1=%d SSE4.2=%d",
           g_cpu_features.sse2, g_cpu_features.sse3, g_cpu_features.ssse3,
           g_cpu_features.sse41, g_cpu_features.sse42);
  LOG_INFO("[CPU] AVX=%d AVX2=%d FMA=%d XSAVE=%d", g_cpu_features.avx,
           g_cpu_features.avx2, g_cpu_features.fma, has_xsave);
  LOG_INFO("[CPU] AVX512F=%d BW=%d VL=%d DQ=%d", g_cpu_features.avx512f,
           g_cpu_features.avx512bw, g_cpu_features.avx512vl,
           g_cpu_features.avx512dq);

  if (!g_cpu_features.sse2) {
    LOG_PANIC("[CPU] SSE2 no detectado. x86_64 inválido.");
    return;
  }

  g_cpu_features.level = CPU_SIMD_SSE2;
  if (g_cpu_features.sse41)
    g_cpu_features.level = CPU_SIMD_SSE41;

  int can_avx = g_cpu_features.avx || g_cpu_features.avx2;

  if (can_avx && has_xsave) {
    int want_avx512 = g_cpu_features.avx512f && g_cpu_features.avx512bw &&
                      g_cpu_features.avx512vl && g_cpu_features.avx512dq;

    enable_avx_xcr0(want_avx512);

    int now_osxsave = cpuid_bit(1, 0, 2, 27);
    g_cpu_features.osxsave = now_osxsave;
    g_cpu_features.xsave_enabled = now_osxsave;
    g_cpu_features.avx512_enabled = want_avx512 && now_osxsave;

    if (!now_osxsave) {
      LOG_ERR("[CPU] CR4.OSXSAVE no se aplicó. Dejando AVX deshabilitado.");
      g_cpu_features.avx = g_cpu_features.avx2 = 0;
      g_cpu_features.avx512f = g_cpu_features.avx512bw = 0;
      g_cpu_features.avx512vl = g_cpu_features.avx512dq = 0;
    } else {
      LOG_INFO("[CPU] XCR0 configurado: %s habilitado",
               want_avx512 ? "AVX-512 (SSE+AVX+AVX512)" : "SSE+AVX");
      if (g_cpu_features.avx2) {
        g_cpu_features.level = CPU_SIMD_AVX2;
        if (g_cpu_features.avx512_enabled)
          g_cpu_features.level = CPU_SIMD_AVX512;
      }
    }
  } else if (can_avx && !has_xsave) {
    LOG_WARN("[CPU] AVX en CPUID pero XSAVE=0. No se puede usar AVX.");
    g_cpu_features.avx = g_cpu_features.avx2 = 0;
    g_cpu_features.avx512f = g_cpu_features.avx512bw = 0;
    g_cpu_features.avx512vl = g_cpu_features.avx512dq = 0;
  }

  static const char *names[] = {"scalar", "sse2", "sse4.1", "avx2", "avx512"};
  LOG_INFO("[CPU] SIMD dispatch level: %s", names[g_cpu_features.level]);
}

// ============================================================================
// [Power Management]
// ============================================================================

#define MSR_PM_ENABLE 0x770
#define MSR_HWP_CAPABILITIES 0x771
#define MSR_HWP_REQUEST 0x774
#define MSR_IA32_PERF_STATUS 0x198
#define MSR_IA32_PERF_CTL 0x199
#define MSR_IA32_MISC_ENABLE 0x1A0

// ---------------------------------------------------------------------------
// cpu_dump_cpuid_diag — Dump de CPUID y MSRs de solo lectura.
//
// Solo se llama desde el BSP, una vez, para tener los valores crudos en
// el log. Los APs comparten el mismo silicio, así que su CPUID sería el
// mismo salvo APIC ID. No spamear.
// ---------------------------------------------------------------------------
void cpu_dump_cpuid_diag(void) {
  uint32_t eax, ebx, ecx, edx;

  cpuid(0, 0, &eax, &ebx, &ecx, &edx);
  uint32_t max_basic = eax;
  LOG_INFO("[CPUID] max basic leaf = 0x%x", max_basic);

  cpuid(1, 0, &eax, &ebx, &ecx, &edx);
  uint32_t apic_id = (ebx >> 24) & 0xFF;
  LOG_INFO("[CPUID] 1:EAX=0x%08x EBX=0x%08x ECX=0x%08x EDX=0x%08x", eax, ebx,
           ecx, edx);
  LOG_INFO("[CPUID]   APIC ID = %u", apic_id);

  if (max_basic >= 6) {
    cpuid(6, 0, &eax, &ebx, &ecx, &edx);
    LOG_INFO("[CPUID] 6:EAX=0x%08x EBX=0x%08x ECX=0x%08x EDX=0x%08x", eax, ebx,
             ecx, edx);
    LOG_INFO("[CPUID]   bit7  (HWP)          = %u", (eax >> 7) & 1);
    LOG_INFO("[CPUID]   bit13 (HW_FEEDBACK)  = %u", (eax >> 13) & 1);
    LOG_INFO("[CPUID]   bit22 (HWP_EPP)      = %u", (eax >> 22) & 1);
    LOG_INFO("[CPUID]   ECX[0] (APERF/MPERF) = %u", (ecx >> 0) & 1);
  } else {
    LOG_WARN("[CPUID] leaf 6 no soportado (max=%u)", max_basic);
  }

  if (max_basic >= 0x16) {
    cpuid(0x16, 0, &eax, &ebx, &ecx, &edx);
    LOG_INFO("[CPUID] 16:EAX=0x%08x EBX=0x%08x ECX=0x%08x EDX=0x%08x", eax, ebx,
             ecx, edx);
    LOG_INFO("[CPUID]   base=%u MHz max=%u MHz bus=%u MHz", eax & 0xFFFF,
             ebx & 0xFFFF, ecx & 0xFFFF);
  }

  uint64_t misc = rdmsr(MSR_IA32_MISC_ENABLE);
  LOG_INFO("[MSR] IA32_MISC_ENABLE=0x%lx  EIST=%d  TURBO_DIS=%d",
           (unsigned long)misc, (int)((misc >> 16) & 1),
           (int)((misc >> 38) & 1));

  uint64_t status = rdmsr(MSR_IA32_PERF_STATUS);
  LOG_INFO("[MSR] IA32_PERF_STATUS=0x%lx  ratio=%u", (unsigned long)status,
           (unsigned)((status >> 8) & 0xFF));
  LOG_INFO("[MSR] IA32_PERF_CTL=0x%lx",
           (unsigned long)rdmsr(MSR_IA32_PERF_CTL));
}

// ---------------------------------------------------------------------------
// cpu_detect_pm — Detección temprana. Solo CPUID + lectura de MSR.
//
// Sin escrituras. Se puede llamar antes de tener heap, IDT, etc.
// ---------------------------------------------------------------------------
void cpu_detect_pm(void) {
  uint32_t eax, ebx, ecx, edx;
  uint32_t max_basic;

  cpuid(0, 0, &eax, &ebx, &ecx, &edx);
  max_basic = eax;

  // CPUID.6
  if (max_basic >= 6) {
    cpuid(6, 0, &eax, &ebx, &ecx, &edx);
    g_cpu_features.has_hwp = (eax >> 7) & 1;
    g_cpu_features.has_hwp_feedback = (eax >> 13) & 1;
    g_cpu_features.hwp_epp_supported = (eax >> 22) & 1;
    g_cpu_features.has_aperf_mperf = (ecx >> 0) & 1; // ← CLAVE
  } else {
    g_cpu_features.has_hwp = 0;
    g_cpu_features.has_hwp_feedback = 0;
    g_cpu_features.hwp_epp_supported = 0;
    g_cpu_features.has_aperf_mperf = 0;
  }

  // CPUID.16 — frecuencias nominales.
  if (max_basic >= 0x16) {
    cpuid(0x16, 0, &eax, &ebx, &ecx, &edx);
    g_cpu_features.pm_base_mhz = eax & 0xFFFF;
    g_cpu_features.pm_max_mhz = ebx & 0xFFFF;
    g_cpu_features.pm_bus_mhz = ecx & 0xFFFF;
  } else {
    g_cpu_features.pm_base_mhz = 0;
    g_cpu_features.pm_max_mhz = 0;
    g_cpu_features.pm_bus_mhz = 0;
  }

  // IA32_MISC_ENABLE: EIST. Este MSR sí existe en todo x86_64 moderno,
  // se puede leer sin comprobar soporte.
  uint64_t misc = rdmsr(MSR_IA32_MISC_ENABLE);
  g_cpu_features.has_eist = (misc >> 16) & 1;

  LOG_INFO("[PM] HWP=%d HWP_EPP=%d EIST=%d APERF=%d  base=%u max=%u bus=%u MHz",
           g_cpu_features.has_hwp, g_cpu_features.hwp_epp_supported,
           g_cpu_features.has_eist, g_cpu_features.has_aperf_mperf,
           g_cpu_features.pm_base_mhz, g_cpu_features.pm_max_mhz,
           g_cpu_features.pm_bus_mhz);
}

// ---------------------------------------------------------------------------
// cpu_apply_pm_policy — Aplica la política de rendimiento.
//
// Aquí SÍ escribimos MSRs. Requiere que la IDT esté cargada (para que un
// #GP en el wrmsr sea capturable) y que el heap esté inicializado (por si
// el log necesita más de lo que cabe en el ring).
//
// Prioridad:
//   1) HWP si está soportado. Es el método moderno, deja la CPU decidir.
//   2) Si no, forzar el ratio máximo con IA32_PERF_CTL (requiere EIST).
//   3) Si tampoco, dejar constancia y no tocar nada.
// ---------------------------------------------------------------------------
// Reemplaza la función cpu_apply_pm_policy completa en cpu.c
void cpu_apply_pm_policy(void) {
  if (g_cpu_features.has_hwp) {
    wrmsr(MSR_PM_ENABLE, 1);

    uint64_t pm_enable = rdmsr(MSR_PM_ENABLE);
    if ((pm_enable & 1) == 0) {
      LOG_ERR("[PM] IA32_PM_ENABLE no se aplicó (leído 0x%lx)",
              (unsigned long)pm_enable);
      g_cpu_features.has_hwp = 0;
    } else {
      uint64_t caps = rdmsr(MSR_HWP_CAPABILITIES);
      uint8_t highest = (caps >> 0) & 0xFF;
      uint8_t lowest = (caps >> 24) & 0xFF;

      g_cpu_features.hwp_highest = highest;
      g_cpu_features.hwp_lowest = lowest;

      LOG_INFO("[PM] HWP caps: highest=%u lowest=%u", highest, lowest);

      uint64_t req = ((uint64_t)lowest) | ((uint64_t)highest << 8) |
                     ((uint64_t)highest << 16);
      wrmsr(MSR_HWP_REQUEST, req);

      g_cpu_features.hwp_active = 1;
      LOG_INFO("[PM] HWP activado (min=%u max=%u)", lowest, highest);
      return;
    }
  }

  // Fallback: IA32_PERF_CTL
  if (g_cpu_features.pm_max_mhz > 0 && g_cpu_features.pm_bus_mhz > 0 &&
      g_cpu_features.has_eist) {
    uint32_t ratio = g_cpu_features.pm_max_mhz / g_cpu_features.pm_bus_mhz;
    if (ratio > 0 && ratio < 255) {
      uint64_t ctl = rdmsr(MSR_IA32_PERF_CTL);
      ctl = (ctl & ~0xFF00ULL) | ((uint64_t)ratio << 8);
      wrmsr(MSR_IA32_PERF_CTL, ctl);
      g_cpu_features.pstate_forced = 1;
      LOG_INFO("[PM] IA32_PERF_CTL ratio=%u (~%u MHz) forzado", ratio,
               ratio * g_cpu_features.pm_bus_mhz);
    }
  } else if (!g_cpu_features.has_eist) {
    LOG_WARN("[PM] EIST deshabilitado en la BIOS.");
  } else {
    // [FIX] No es un error, muchas laptops no exponen CPUID.16
    LOG_INFO("[PM] CPUID.16 no soportado. Delegando control a ACPI.");
  }
}

// ---------------------------------------------------------------------------
// cpu_hwp_init_ap — Versión reducida para APs.
//
// Se llama desde ap_entry DESPUÉS de idt_load(), para que cualquier #GP
// en un wrmsr sea capturable. Repite la política ya elegida por el BSP:
//   - Si g_cpu_features.hwp_active, activa HWP en este core.
//   - Si g_cpu_features.pstate_forced, aplica el mismo ratio.
//   - Si no hay política, no toca nada.
//
// No hace diagnóstico APERF/MPERF (ya lo hizo el BSP).
// ---------------------------------------------------------------------------
void cpu_hwp_init_ap(void) {
  if (g_cpu_features.hwp_active) {
    wrmsr(MSR_PM_ENABLE, 1);

    uint64_t caps = rdmsr(MSR_HWP_CAPABILITIES);

    // Layout correcto (ver cpu_apply_pm_policy).
    uint8_t highest = (caps >> 0) & 0xFF;
    uint8_t lowest = (caps >> 24) & 0xFF;

    uint64_t req = ((uint64_t)lowest) | ((uint64_t)highest << 8) |
                   ((uint64_t)highest << 16);
    wrmsr(MSR_HWP_REQUEST, req);
    return;
  }

  if (g_cpu_features.pstate_forced && g_cpu_features.pm_bus_mhz > 0 &&
      g_cpu_features.pm_max_mhz > 0) {
    uint32_t ratio = g_cpu_features.pm_max_mhz / g_cpu_features.pm_bus_mhz;
    if (ratio > 0 && ratio < 255) {
      uint64_t ctl = rdmsr(MSR_IA32_PERF_CTL);
      ctl = (ctl & ~0xFF00ULL) | ((uint64_t)ratio << 8);
      wrmsr(MSR_IA32_PERF_CTL, ctl);
    }
  }
}