// kernel/simd.c
//
// Implementaciones SIMD de los kernels calientes + dispatch automático.
// Usa __attribute__((target(...))) para compilar todas las variantes en
// un mismo .o sin necesidad de -mavx2 global (que rompería el boot).
//
#include "simd.h"
#include "cpu.h"
#include "gfx/gfx.h"
#include "klog.h"
#include <string.h>

#if defined(__x86_64__) || defined(__amd64__)
#include <immintrin.h>
#define HAVE_X86_SIMD 1
#else
#define HAVE_X86_SIMD 0
#endif

// ============================================================================
// Function-pointer dispatch
// ============================================================================
typedef void (*row_blend_fn_t)(uint32_t *, const uint32_t *, int);
typedef void (*row_copy_fn_t)(uint32_t *, const uint32_t *, size_t);
typedef void (*row_fill_fn_t)(uint32_t *, uint32_t, size_t);

static row_blend_fn_t s_blend = NULL;
static row_copy_fn_t s_copy_vram = NULL;
static row_copy_fn_t s_copy_norm = NULL;
static row_fill_fn_t s_fill = NULL;

// ============================================================================
// SCALAR implementations (baseline, siempre disponibles)
// ============================================================================
static void blend_scalar(uint32_t *dst, const uint32_t *src, int n) {
  for (int i = 0; i < n; i++)
    dst[i] = blend_pixel_fast(src[i], dst[i]);
}

static void copy_scalar(uint32_t *dst, const uint32_t *src, size_t n) {
  for (size_t i = 0; i < n; i++)
    dst[i] = src[i];
}

static void fill_scalar(uint32_t *dst, uint32_t color, size_t n) {
  for (size_t i = 0; i < n; i++)
    dst[i] = color;
}

#if HAVE_X86_SIMD

// ============================================================================
// Helper: división por 255 (aproximada) en 16-bit lanes.
//   (x + 128 + ((x + 128) >> 8)) >> 8
// Resultado idéntico a (x + 127) / 255 para x en 0..65535.
// ============================================================================
#define DIV255_16(x, c128)                                                     \
  _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16((x), (c128)),                     \
                               _mm_srli_epi16(_mm_add_epi16((x), (c128)), 8)), \
                 8)

// ============================================================================
// SSE2 blend (4 px por iteración)
// ============================================================================
__attribute__((target("sse2"))) static void
blend_sse2(uint32_t *dst, const uint32_t *src, int n) {
  const __m128i zero = _mm_setzero_si128();
  const __m128i c255 = _mm_set1_epi32(255);
  const __m128i c128 = _mm_set1_epi16(128);
  const __m128i cff = _mm_set1_epi8((char)0xFF);
  const __m128i alpha_mask = _mm_set1_epi32((int)0xFF000000);

  int i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128i s = _mm_loadu_si128((const __m128i *)&src[i]);
    __m128i d = _mm_loadu_si128((const __m128i *)&dst[i]);
    __m128i sa = _mm_srli_epi32(s, 24);

    if (_mm_movemask_epi8(_mm_cmpeq_epi32(sa, zero)) == 0xFFFF)
      continue;
    if (_mm_movemask_epi8(_mm_cmpeq_epi32(sa, c255)) == 0xFFFF) {
      _mm_storeu_si128((__m128i *)&dst[i], s);
      continue;
    }

    // Broadcast alpha to all 4 bytes per dword
    __m128i sa2 = _mm_or_si128(sa, _mm_slli_epi32(sa, 8));
    __m128i sa4 = _mm_or_si128(sa2, _mm_slli_epi32(sa2, 16));
    __m128i ia4 = _mm_sub_epi8(cff, sa4);

    __m128i s_lo = _mm_unpacklo_epi8(s, zero);
    __m128i s_hi = _mm_unpackhi_epi8(s, zero);
    __m128i d_lo = _mm_unpacklo_epi8(d, zero);
    __m128i d_hi = _mm_unpackhi_epi8(d, zero);
    __m128i a_lo = _mm_unpacklo_epi8(sa4, zero);
    __m128i a_hi = _mm_unpackhi_epi8(sa4, zero);
    __m128i ia_lo = _mm_unpacklo_epi8(ia4, zero);
    __m128i ia_hi = _mm_unpackhi_epi8(ia4, zero);

    __m128i sa_lo = DIV255_16(_mm_mullo_epi16(s_lo, a_lo), c128);
    __m128i sa_hi = DIV255_16(_mm_mullo_epi16(s_hi, a_hi), c128);
    __m128i dia_lo = DIV255_16(_mm_mullo_epi16(d_lo, ia_lo), c128);
    __m128i dia_hi = DIV255_16(_mm_mullo_epi16(d_hi, ia_hi), c128);

    __m128i r_lo = _mm_add_epi16(sa_lo, dia_lo);
    __m128i r_hi = _mm_add_epi16(sa_hi, dia_hi);

    __m128i res = _mm_packus_epi16(r_lo, r_hi);
    res = _mm_or_si128(res, alpha_mask);

    _mm_storeu_si128((__m128i *)&dst[i], res);
  }
  for (; i < n; i++)
    dst[i] = blend_pixel_fast(src[i], dst[i]);
}

// ============================================================================
// SSE4.1 blend (4 px). Usa _mm_blendv_epi8 y _mm_mullo_epi32
// ============================================================================
__attribute__((target("sse4.1"))) static void
blend_sse41(uint32_t *dst, const uint32_t *src, int n) {
  blend_sse2(dst, src, n); // por ahora reutiliza SSE2
}

// ============================================================================
// AVX2 blend (8 px por iteración)
// ============================================================================
__attribute__((target("avx2"))) static void
blend_avx2(uint32_t *dst, const uint32_t *src, int n) {
  const __m256i zero = _mm256_setzero_si256();
  const __m256i c255 = _mm256_set1_epi32(255);
  const __m256i c128 = _mm256_set1_epi16(128);
  const __m256i cff = _mm256_set1_epi8((char)0xFF);
  const __m256i alpha_mask = _mm256_set1_epi32((int)0xFF000000);

  int i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256i s = _mm256_loadu_si256((const __m256i *)&src[i]);
    __m256i d = _mm256_loadu_si256((const __m256i *)&dst[i]);
    __m256i sa = _mm256_srli_epi32(s, 24);

    if (_mm256_movemask_epi8(_mm256_cmpeq_epi32(sa, zero)) == -1)
      continue;
    if (_mm256_movemask_epi8(_mm256_cmpeq_epi32(sa, c255)) == -1) {
      _mm256_storeu_si256((__m256i *)&dst[i], s);
      continue;
    }

    __m256i sa2 = _mm256_or_si256(sa, _mm256_slli_epi32(sa, 8));
    __m256i sa4 = _mm256_or_si256(sa2, _mm256_slli_epi32(sa2, 16));
    __m256i ia4 = _mm256_sub_epi8(cff, sa4);

    // Unpack 8 x 32-bit to two 8 x 16-bit halves
    __m256i s_lo = _mm256_unpacklo_epi8(s, zero);
    __m256i s_hi = _mm256_unpackhi_epi8(s, zero);
    __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
    __m256i d_hi = _mm256_unpackhi_epi8(d, zero);
    __m256i a_lo = _mm256_unpacklo_epi8(sa4, zero);
    __m256i a_hi = _mm256_unpackhi_epi8(sa4, zero);
    __m256i ia_lo = _mm256_unpacklo_epi8(ia4, zero);
    __m256i ia_hi = _mm256_unpackhi_epi8(ia4, zero);

// Divide by 255
#define DIV255_256(x)                                                          \
  _mm256_srli_epi16(                                                           \
      _mm256_add_epi16(_mm256_add_epi16((x), c128),                            \
                       _mm256_srli_epi16(_mm256_add_epi16((x), c128), 8)),     \
      8)

    __m256i sa_lo = DIV255_256(_mm256_mullo_epi16(s_lo, a_lo));
    __m256i sa_hi = DIV255_256(_mm256_mullo_epi16(s_hi, a_hi));
    __m256i dia_lo = DIV255_256(_mm256_mullo_epi16(d_lo, ia_lo));
    __m256i dia_hi = DIV255_256(_mm256_mullo_epi16(d_hi, ia_hi));

    __m256i r_lo = _mm256_add_epi16(sa_lo, dia_lo);
    __m256i r_hi = _mm256_add_epi16(sa_hi, dia_hi);

    // packus_epi16 opera por-lane de 128 bits: para cada lane, empaqueta
    // (r_lo.lane, r_hi.lane). Como r_lo = unpacklo(s,0) contiene los
    // pixeles {0,1|4,5} de ese lane y r_hi = unpackhi(s,0) contiene {2,3|6,7},
    // packus(r_lo,r_hi) reconstruye {0,1,2,3|4,5,6,7} = EL ORDEN ORIGINAL,
    // sin necesitar ningun shuffle/permute adicional.
    //
    // El permute4x64_epi64(res, 0xD8) que habia aqui intercambiaba los
    // qwords 1 y 2 del resultado (bytes 8-15 con 16-23), es decir, los
    // pixeles 2-3 con los pixeles 4-5 de cada bloque de 8. Eso rompia el
    // orden correcto y producia el efecto de "doble imagen"/bandas cada
    // 8 pixeles en TODO lo que pasara por blend con alpha parcial: iconos,
    // el surface completo de la ventana (blit en window_render_frame) y,
    // sobre todo, la sombra Aurora (100% translucida => siempre toma esta
    // ruta), que se veia como rayas en vez de un degradado liso.
    __m256i res = _mm256_packus_epi16(r_lo, r_hi);
    res = _mm256_or_si256(res, alpha_mask);

    _mm256_storeu_si256((__m256i *)&dst[i], res);
  }
  for (; i < n; i++)
    dst[i] = blend_pixel_fast(src[i], dst[i]);
}

// ============================================================================
// SSE2/AVX2/AVX-512 copies
// ============================================================================
__attribute__((target("sse2"))) static void
copy_sse2(uint32_t *dst, const uint32_t *src, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128i v = _mm_loadu_si128((const __m128i *)&src[i]);
    _mm_storeu_si128((__m128i *)&dst[i], v);
  }
  for (; i < n; i++)
    dst[i] = src[i];
}

__attribute__((target("avx2"))) static void
copy_avx2(uint32_t *dst, const uint32_t *src, size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256i v = _mm256_loadu_si256((const __m256i *)&src[i]);
    _mm256_storeu_si256((__m256i *)&dst[i], v);
  }
  for (; i < n; i++)
    dst[i] = src[i];
}

__attribute__((target("avx512f"))) static void
copy_avx512(uint32_t *dst, const uint32_t *src, size_t n) {
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512i v = _mm512_loadu_si512((const void *)&src[i]);
    _mm512_storeu_si512((void *)&dst[i], v);
  }
  for (; i < n; i++)
    dst[i] = src[i];
}

// Non-temporal (streaming) copies for VRAM
__attribute__((target("sse2"))) static void
copy_nt_sse2(uint32_t *dst, const uint32_t *src, size_t n) {
  size_t i = 0;
  // Align dst to 16 bytes
  while (i < n && (((uintptr_t)&dst[i]) & 15) != 0) {
    dst[i] = src[i];
    i++;
  }
  for (; i + 4 <= n; i += 4) {
    __m128i v = _mm_loadu_si128((const __m128i *)&src[i]);
    _mm_stream_si128((__m128i *)&dst[i], v);
  }
  _mm_sfence();
  for (; i < n; i++)
    dst[i] = src[i];
}

__attribute__((target("avx2"))) static void
copy_nt_avx2(uint32_t *dst, const uint32_t *src, size_t n) {
  size_t i = 0;
  while (i < n && (((uintptr_t)&dst[i]) & 31) != 0) {
    dst[i] = src[i];
    i++;
  }
  for (; i + 8 <= n; i += 8) {
    __m256i v = _mm256_loadu_si256((const __m256i *)&src[i]);
    _mm256_stream_si256((__m256i *)&dst[i], v);
  }
  _mm_sfence();
  for (; i < n; i++)
    dst[i] = src[i];
}

__attribute__((target("avx512f"))) static void
copy_nt_avx512(uint32_t *dst, const uint32_t *src, size_t n) {
  size_t i = 0;
  while (i < n && (((uintptr_t)&dst[i]) & 63) != 0) {
    dst[i] = src[i];
    i++;
  }
  for (; i + 16 <= n; i += 16) {
    __m512i v = _mm512_loadu_si512((const void *)&src[i]);
    _mm512_stream_si512((void *)&dst[i], v);
  }
  _mm_sfence();
  for (; i < n; i++)
    dst[i] = src[i];
}

// ============================================================================
// Fills
// ============================================================================
__attribute__((target("sse2"))) static void
fill_sse2(uint32_t *dst, uint32_t color, size_t n) {
  __m128i v = _mm_set1_epi32((int)color);
  size_t i = 0;
  for (; i + 4 <= n; i += 4)
    _mm_storeu_si128((__m128i *)&dst[i], v);
  for (; i < n; i++)
    dst[i] = color;
}

__attribute__((target("avx2"))) static void
fill_avx2(uint32_t *dst, uint32_t color, size_t n) {
  __m256i v = _mm256_set1_epi32((int)color);
  size_t i = 0;
  for (; i + 8 <= n; i += 8)
    _mm256_storeu_si256((__m256i *)&dst[i], v);
  for (; i < n; i++)
    dst[i] = color;
}

__attribute__((target("avx512f"))) static void
fill_avx512(uint32_t *dst, uint32_t color, size_t n) {
  __m512i v = _mm512_set1_epi32((int)color);
  size_t i = 0;
  for (; i + 16 <= n; i += 16)
    _mm512_storeu_si512((void *)&dst[i], v);
  for (; i < n; i++)
    dst[i] = color;
}

#endif // HAVE_X86_SIMD

// ============================================================================
// Init: elige la mejor implementación
// ============================================================================
void simd_init(void) {
#if HAVE_X86_SIMD
  switch (g_cpu_features.level) {
  case CPU_SIMD_AVX512:
    s_blend = blend_avx2; // reutilizamos AVX2 por ahora para blend
    s_copy_vram = copy_nt_avx512;
    s_copy_norm = copy_avx512;
    s_fill = fill_avx512;
    break;
  case CPU_SIMD_AVX2:
    s_blend = blend_avx2;
    s_copy_vram = copy_nt_avx2;
    s_copy_norm = copy_avx2;
    s_fill = fill_avx2;
    break;
  case CPU_SIMD_SSE41:
    s_blend = blend_sse41;
    s_copy_vram = copy_nt_sse2;
    s_copy_norm = copy_sse2;
    s_fill = fill_sse2;
    break;
  case CPU_SIMD_SSE2:
    s_blend = blend_sse2;
    s_copy_vram = copy_nt_sse2;
    s_copy_norm = copy_sse2;
    s_fill = fill_sse2;
    break;
  default:
    s_blend = blend_scalar;
    s_copy_vram = copy_scalar;
    s_copy_norm = copy_scalar;
    s_fill = fill_scalar;
    break;
  }
#else
  s_blend = blend_scalar;
  s_copy_vram = copy_scalar;
  s_copy_norm = copy_scalar;
  s_fill = fill_scalar;
#endif
  LOG_INFO("[SIMD] Dispatch inicializado (blend/copy_nt/copy/fill)");
}

// ============================================================================
// Public API
// ============================================================================
void simd_blit_row(uint32_t *dst, const uint32_t *src, int n) {
  if (n <= 0)
    return;
  s_blend(dst, src, n);
}

void simd_copy_to_vram(uint32_t *dst, const uint32_t *src, size_t n) {
  if (n == 0)
    return;
  s_copy_vram(dst, src, n);
}

void simd_copy_normal(uint32_t *dst, const uint32_t *src, size_t n) {
  if (n == 0)
    return;
  s_copy_norm(dst, src, n);
}

void simd_fill_row(uint32_t *dst, uint32_t color, size_t n) {
  if (n == 0)
    return;
  s_fill(dst, color, n);
}