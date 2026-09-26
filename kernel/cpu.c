// kernel/cpu.c
#include "cpu.h"
#include "klog.h"

// ============================================================================
// CPUID + SIMD feature detection
//
// IMPORTANTE sobre OSXSAVE:
//   CPUID.1:ECX[27] (OSXSAVE) es de SOLO LECTURA: refleja el estado actual
//   de CR4.OSXSAVE. La CPU no "tiene" OSXSAVE — nosotros lo ponemos.
//   El feature de hardware que hay que detectar es XSAVE (ECX[26]).
//   Orden correcto:
//     1. cpuid.1:ecx[26] (XSAVE)       -> ¿la CPU soporta xsave/xgetbv?
//     2. si sí: set CR4.OSXSAVE         -> habilita xsave
//     3. xsetbv(XCR0)                    -> habilita SSE/AVX/AVX-512
//     4. cpuid.1:ecx[27] (OSXSAVE) ahora devuelve 1
// ============================================================================

cpu_features_t g_cpu_features = {0};

static int cpuid_bit(uint32_t leaf, uint32_t sub, int reg, int bit) {
  uint32_t a, b, c, d;
  cpuid(leaf, sub, &a, &b, &c, &d);
  uint32_t v = (reg == 0) ? a : (reg == 1) ? b : (reg == 2) ? c : d;
  return (v >> bit) & 1;
}

static void enable_avx_xcr0(int avx512) {
  /* CR4.OSXSAVE (bit 18) */
  uint64_t cr4;
  __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
  cr4 |= (1ULL << 18);
  __asm__ volatile("movq %0, %%cr4" : : "r"(cr4));

  /* XCR0: bit 1 = SSE, bit 2 = AVX, bits 5-7 = AVX-512 state */
  uint32_t lo, hi;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  lo |= 0x6; /* SSE + AVX */
  if (avx512)
    lo |= 0xE0; /* opmask + ZMM_Hi256 + Hi16_ZMM */
  __asm__ volatile("xsetbv" : : "a"(lo), "d"(hi), "c"(0));
}

void cpu_simd_init(void) {
  /* --- Detección de features básicos --- */
  g_cpu_features.sse2 = cpuid_bit(1, 0, 3, 26);
  g_cpu_features.sse3 = cpuid_bit(1, 0, 2, 0);
  g_cpu_features.ssse3 = cpuid_bit(1, 0, 2, 9);
  g_cpu_features.sse41 = cpuid_bit(1, 0, 2, 19);
  g_cpu_features.sse42 = cpuid_bit(1, 0, 2, 20);
  g_cpu_features.avx = cpuid_bit(1, 0, 2, 28);
  g_cpu_features.fma = cpuid_bit(1, 0, 2, 12);

  /* XSAVE: feature de hardware. ESTE es el que hay que comprobar. */
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

  /* Nivel base: SSE2, sube a SSE4.1 si está disponible. */
  g_cpu_features.level = CPU_SIMD_SSE2;
  if (g_cpu_features.sse41)
    g_cpu_features.level = CPU_SIMD_SSE41;

  /* --- Habilitar AVX si HW lo soporta --- */
  int can_avx = g_cpu_features.avx || g_cpu_features.avx2;

  if (can_avx && has_xsave) {
    /* AVX-512 solo si el HW lo soporta Y queremos los bits 5-7. */
    int want_avx512 = g_cpu_features.avx512f && g_cpu_features.avx512bw &&
                      g_cpu_features.avx512vl && g_cpu_features.avx512dq;

    enable_avx_xcr0(want_avx512);

    /* Ahora sí, OSXSAVE debería leerse como 1. Lo comprobamos como
     * sanity check: si aún así dice 0, algo ha fallado al escribir CR4. */
    int now_osxsave = cpuid_bit(1, 0, 2, 27);
    g_cpu_features.osxsave = now_osxsave;
    g_cpu_features.xsave_enabled = now_osxsave;
    g_cpu_features.avx512_enabled = want_avx512 && now_osxsave;

    if (!now_osxsave) {
      LOG_ERR("[CPU] CR4.OSXSAVE no se aplicó (¿se lee 0 tras escribirlo?). "
              "Dejando AVX deshabilitado por seguridad.");
      g_cpu_features.avx = g_cpu_features.avx2 = 0;
      g_cpu_features.avx512f = g_cpu_features.avx512bw = 0;
      g_cpu_features.avx512vl = g_cpu_features.avx512dq = 0;
      /* level se queda en SSE4.1 o SSE2 */
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
    LOG_WARN("[CPU] AVX en CPUID pero XSAVE=0 (raro). No se puede usar AVX.");
    g_cpu_features.avx = g_cpu_features.avx2 = 0;
    g_cpu_features.avx512f = g_cpu_features.avx512bw = 0;
    g_cpu_features.avx512vl = g_cpu_features.avx512dq = 0;
  }

  static const char *names[] = {"scalar", "sse2", "sse4.1", "avx2", "avx512"};
  LOG_INFO("[CPU] SIMD dispatch level: %s", names[g_cpu_features.level]);
}