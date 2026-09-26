// kernel/gfx/gfx.c
#include "gfx.h"
#include "../cpu.h"
#include "../simd.h"
#include "font_aa.h"
#include "font_manager.h"
#include <emmintrin.h>
#include <immintrin.h>

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

// Raíz cuadrada entera rápida (Método de Newton) para anti-aliasing
static inline int int_sqrt(int n) {
  if (n <= 0)
    return 0;
  int x = n;
  int y = (x + 1) >> 1;
  while (y < x) {
    x = y;
    y = (x + n / x) >> 1;
  }
  return x;
}

/* Raíz cuadrada escalar usando SSE (sqrtss). No hay libm en bare-metal,
 * así que no podemos llamar a sqrtf(). El builtin kernel_sqrtf a veces
 * sí genera una llamada a sqrtf si el compilador no ve -ffast-math, así
 * que lo hacemos explícito con el intrínseco. */
static inline float kernel_sqrtf(float x) {
  __m128 v = _mm_set_ss(x);
  v = _mm_sqrt_ss(v);
  return _mm_cvtss_f32(v);
}

// ---------------------------------------------------------------------------
// [MEJORA] Anti-aliasing suave para bordes de esquinas redondeadas.
//
// La versión anterior usaba una rampa LINEAL de ~0.5px de ancho
// (coverage = 128 - diff, corte duro en diff > 128), lo que en pantalla se
// notaba como "escalones" (staircase) en los bordes redondeados, sobre todo
// en radios pequeños como los de los botones de la taskbar.
//
// Esta versión usa un smoothstep clásico (3t² - 2t³) sobre una banda de
// ~1px de ancho, que da una transición de opacidad mucho más suave y
// "cara" sin tocar la geometría (mismo dist_fp/r_fp de siempre).
//
// dist_fp / r_fp están en fixed-point 8.8 (ver int_sqrt(... << 16) y
// radius << 8 en las funciones de abajo). Ya se usa `float` en este mismo
// archivo (ver gfx_draw_shadow), así que no introduce ningún requisito
// nuevo de FPU/SSE.
// ---------------------------------------------------------------------------
static inline uint32_t smooth_edge_alpha(int dist_fp, int r_fp,
                                         uint32_t base_alpha) {
  float diff_px =
      (float)(dist_fp - r_fp) / 256.0f; // distancia al borde, en píxeles
  if (diff_px >= 1.0f)
    return 0; // claramente fuera
  if (diff_px <= -1.0f)
    return base_alpha; // claramente dentro

  float t = (1.0f - diff_px) * 0.5f;   // 0 (fuera) .. 1 (dentro)
  float s = t * t * (3.0f - 2.0f * t); // smoothstep
  return (uint32_t)(base_alpha * s);
}

// Igual que arriba pero para un ANILLO (borde), donde la banda de alpha
// máximo está centrada en dist == radio y se desvanece a ambos lados.
static inline uint32_t smooth_ring_alpha(int diff_fp, uint32_t base_alpha) {
  const float half_width_px = 1.1f; // medio-ancho del borde, en píxeles
  float diff_px = (float)diff_fp / 256.0f;
  float adiff = (diff_px < 0.0f) ? -diff_px : diff_px;
  if (adiff >= half_width_px)
    return 0;
  float t = 1.0f - (adiff / half_width_px);
  float s = t * t * (3.0f - 2.0f * t);
  return (uint32_t)(base_alpha * s);
}

// ---------------------------------------------------------------------------
// Sombra Aurora — LUT precalculada, sin sqrt en runtime.
//
// La LUT está indexada por d² = dx² + dy². Cada entrada guarda el alpha de
// la sombra a esa distancia. Con BLUR=14, la LUT tiene ~400 entradas (400 B),
// cabe en L1 y el lookup es una instrucción.
//
// Regla clave: dentro del rectángulo redondeado (d² < R²) el shadow NO se
// dibuja (alpha=0 en la LUT). El cuerpo de la ventana cubre esa zona por
// su cuenta.
// ---------------------------------------------------------------------------
#define AURORA_SHADOW_BLUR 10
#define AURORA_SHADOW_ALPHA_MAX 60
#define AURORA_SHADOW_OFFSET_Y 0
#define AURORA_SHADOW_RADIUS 8 /* corner radius de las ventanas */
#define AURORA_SHADOW_D2_MAX (AURORA_SHADOW_BLUR * AURORA_SHADOW_BLUR * 2 + 4)

static uint8_t g_shadow_lut[AURORA_SHADOW_D2_MAX];
static int g_shadow_lut_ready = 0;

static void shadow_lut_init(void) {
  if (g_shadow_lut_ready)
    return;
  const int BLUR = AURORA_SHADOW_BLUR;
  const int AMAX = AURORA_SHADOW_ALPHA_MAX;
  const int R = AURORA_SHADOW_RADIUS;
  for (int d2 = 0; d2 < AURORA_SHADOW_D2_MAX; d2++) {
    /* sqrt entero por búsqueda lineal (d2 < 400 → máximo 20 iteraciones) */
    int d = 0;
    while ((d + 1) * (d + 1) <= d2)
      d++;
    int sdf = d - R;
    if (sdf < 0) {
      g_shadow_lut[d2] = 0;
      continue;
    } /* dentro */
    if (sdf >= BLUR) {
      g_shadow_lut[d2] = 0;
      continue;
    } /* muy lejos */
    /* Falloff cuadrático: al borde (sdf=0) alpha=AMAX, cae a 0 en sdf=BLUR. */
    int t = BLUR - sdf;
    g_shadow_lut[d2] = (uint8_t)((AMAX * t * t) / (BLUR * BLUR));
  }
  g_shadow_lut_ready = 1;
}

void gfx_draw_shadow_aurora(uint32_t *dst, int dst_stride, rect_t clip,
                            rect_t win_rect, int corner_radius) {
  if (!dst)
    return;
  shadow_lut_init();

  int wx = win_rect.x, wy = win_rect.y;
  int ww = win_rect.w, wh = win_rect.h;
  const int BLUR = AURORA_SHADOW_BLUR;
  const int R = corner_radius;

  int sx0 = wx + R, sy0 = wy + R;
  int sx1 = wx + ww - R, sy1 = wy + wh - R;

  int bx0 = wx - BLUR;
  int by0 = wy - BLUR + AURORA_SHADOW_OFFSET_Y;
  int bx1 = wx + ww + BLUR;
  int by1 = wy + wh + BLUR + AURORA_SHADOW_OFFSET_Y;

  if (bx0 < clip.x)
    bx0 = clip.x;
  if (by0 < clip.y)
    by0 = clip.y;
  if (bx1 > clip.x + clip.w)
    bx1 = clip.x + clip.w;
  if (by1 > clip.y + clip.h)
    by1 = clip.y + clip.h;
  if (bx0 >= bx1 || by0 >= by1)
    return;

  for (int py = by0; py < by1; py++) {
    uint32_t *row = &dst[py * dst_stride];
    int sy = py - AURORA_SHADOW_OFFSET_Y;
    int dy = 0;
    if (sy < sy0)
      dy = sy0 - sy;
    else if (sy > sy1)
      dy = sy - sy1;
    int dy2 = dy * dy;
    if (dy2 >= AURORA_SHADOW_D2_MAX)
      continue;

    for (int px = bx0; px < bx1; px++) {
      int dx = 0;
      if (px < sx0)
        dx = sx0 - px;
      else if (px > sx1)
        dx = px - sx1;

      int d2 = dx * dx + dy2;
      if (d2 >= AURORA_SHADOW_D2_MAX)
        continue;

      uint32_t a = g_shadow_lut[d2];
      if (a == 0)
        continue;

      row[px] = blend_pixel_fast((a << 24), row[px]);
    }
  }
}

// Función auxiliar de mezcla ponderada para la sombra
static inline void gfx_blend_shadow_pixel(uint32_t *dst, int x, int y,
                                          int stride, uint32_t base_color,
                                          uint8_t alpha_factor) {
  // Aquí puedes integrar tu función estándar de blend o esta optimizada para
  // sombras
  uint32_t dest_pixel = dst[y * stride + x];

  // Extraer canales del fondo
  uint32_t dr = (dest_pixel >> 16) & 0xFF;
  uint32_t dg = (dest_pixel >> 8) & 0xFF;
  uint32_t db = dest_pixel & 0xFF;

  // Extraer canales del color de sombra (negro con alpha variable)
  uint32_t sr = (base_color >> 16) & 0xFF;
  uint32_t sg = (base_color >> 8) & 0xFF;
  uint32_t sb = base_color & 0xFF;

  uint32_t final_r = (sr * alpha_factor + dr * (255 - alpha_factor)) / 255;
  uint32_t final_g = (sg * alpha_factor + dg * (255 - alpha_factor)) / 255;
  uint32_t final_b = (sb * alpha_factor + db * (255 - alpha_factor)) / 255;

  dst[y * stride + x] = 0xFF000000 | (final_r << 16) | (final_g << 8) | final_b;
}

rect_t rect_bounding_box(rect_t a, rect_t b) {
  if (a.w <= 0 || a.h <= 0)
    return b;
  if (b.w <= 0 || b.h <= 0)
    return a;

  int x1 = MIN(a.x, b.x);
  int y1 = MIN(a.y, b.y);
  int x2 = MAX(a.x + a.w, b.x + b.w);
  int y2 = MAX(a.y + a.h, b.y + b.h);

  return (rect_t){x1, y1, x2 - x1, y2 - y1};
}

rect_t rect_clip(rect_t a, rect_t b) {
  int x1 = MAX(a.x, b.x);
  int y1 = MAX(a.y, b.y);
  int x2 = MIN(a.x + a.w, b.x + b.w);
  int y2 = MIN(a.y + a.h, b.y + b.h);

  if (x2 <= x1 || y2 <= y1) {
    return (rect_t){0, 0, 0, 0};
  }

  return (rect_t){x1, y1, x2 - x1, y2 - y1};
}

int rect_intersects(rect_t a, rect_t b) {
  if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0)
    return 0;
  return !(a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y ||
           b.y + b.h <= a.y);
}

void gfx_blend_rect(uint32_t *dst, int dst_stride, rect_t clip, rect_t rect,
                    uint32_t color) {
  if (!dst || rect.w <= 0 || rect.h <= 0)
    return;

  int start_x = MAX(rect.x, clip.x);
  int start_y = MAX(rect.y, clip.y);
  int end_x = MIN(rect.x + rect.w, clip.x + clip.w);
  int end_y = MIN(rect.y + rect.h, clip.y + clip.h);

  if (start_x >= end_x || start_y >= end_y)
    return;

  uint32_t alpha = (color >> 24) & 0xFF;

  for (int py = start_y; py < end_y; py++) {
    uint32_t *row = &dst[py * dst_stride];
    for (int px = start_x; px < end_x; px++) {
      if (alpha == 255) {
        row[px] = color;
      } else {
        row[px] = blend_pixel_fast(color, row[px]);
      }
    }
  }
}

void gfx_fill_rounded_rect(uint32_t *dst, int dst_stride, rect_t clip,
                           rect_t rect, int radius, uint32_t color) {
  if (!dst || rect.w <= 0 || rect.h <= 0)
    return;

  int start_x = MAX(rect.x, clip.x);
  int start_y = MAX(rect.y, clip.y);
  int end_x = MIN(rect.x + rect.w, clip.x + clip.w);
  int end_y = MIN(rect.y + rect.h, clip.y + clip.h);
  if (start_x >= end_x || start_y >= end_y)
    return;

  uint32_t base_alpha = (color >> 24) & 0xFF;
  uint32_t rgb = color & 0x00FFFFFF;
  int r = radius;
  int x_left_corner_end = rect.x + r;
  int x_right_corner_beg = rect.x + rect.w - r;
  int y_top_corner_end = rect.y + r;
  int y_bot_corner_beg = rect.y + rect.h - r;
  int r_fp = r << 8;

  for (int py = start_y; py < end_y; py++) {
    uint32_t *row = &dst[py * dst_stride];
    int row_in_corner = (py < y_top_corner_end) || (py >= y_bot_corner_beg);

    if (!row_in_corner) {
      /* Fila sin esquinas: relleno SIMD desde x_left_corner_end a
       * x_right_corner_beg */
      int rx0 = MAX(start_x, x_left_corner_end);
      int rx1 = MIN(end_x, x_right_corner_beg);
      if (rx0 < rx1) {
        if (base_alpha == 255) {
          simd_fill_row(&row[rx0], color, rx1 - rx0);
        } else {
          for (int px = rx0; px < rx1; px++)
            row[px] = blend_pixel_fast(color, row[px]);
        }
      }
      /* Extremos izquierdo y derecho (también sin esquina, pero por si clip) */
      for (int px = start_x; px < MIN(end_x, x_left_corner_end); px++)
        row[px] = blend_pixel_fast(color, row[px]);
      for (int px = MAX(start_x, x_right_corner_beg); px < end_x; px++)
        row[px] = blend_pixel_fast(color, row[px]);
      continue;
    }

    /* Fila con esquinas: cálculo por píxel */
    for (int px = start_x; px < end_x; px++) {
      int rx = px - rect.x;
      int ry = py - rect.y;

      int in_corner = 0, cx = 0, cy = 0;
      if (rx < r && ry < r) {
        cx = r - rx;
        cy = r - ry;
        in_corner = 1;
      } else if (rx >= rect.w - r && ry < r) {
        cx = rx - (rect.w - r - 1);
        cy = r - ry;
        in_corner = 1;
      } else if (rx < r && ry >= rect.h - r) {
        cx = r - rx;
        cy = ry - (rect.h - r - 1);
        in_corner = 1;
      } else if (rx >= rect.w - r && ry >= rect.h - r) {
        cx = rx - (rect.w - r - 1);
        cy = ry - (rect.h - r - 1);
        in_corner = 1;
      }

      if (in_corner) {
        int dist_fp = int_sqrt((cx * cx + cy * cy) << 16);
        uint32_t alpha = smooth_edge_alpha(dist_fp, r_fp, base_alpha);
        if (alpha == 0)
          continue;
        row[px] = blend_pixel_fast((alpha << 24) | rgb, row[px]);
      } else {
        row[px] = blend_pixel_fast(color, row[px]);
      }
    }
  }
}

void gfx_fill_top_rounded_rect(uint32_t *dst, int dst_stride, rect_t clip,
                               rect_t rect, int radius, uint32_t color) {
  if (!dst || rect.w <= 0 || rect.h <= 0)
    return;

  int start_x = MAX(rect.x, clip.x);
  int start_y = MAX(rect.y, clip.y);
  int end_x = MIN(rect.x + rect.w, clip.x + clip.w);
  int end_y = MIN(rect.y + rect.h, clip.y + clip.h);

  if (start_x >= end_x || start_y >= end_y)
    return;

  uint32_t base_alpha = (color >> 24) & 0xFF;
  uint32_t rgb = color & 0x00FFFFFF;
  int r_fp = radius << 8;

  for (int py = start_y; py < end_y; py++) {
    uint32_t *row = &dst[py * dst_stride];
    for (int px = start_x; px < end_x; px++) {
      int rx = px - rect.x;
      int ry = py - rect.y;

      int in_corner = 0;
      int cx = 0, cy = 0;

      if (rx < radius && ry < radius) {
        cx = radius - rx;
        cy = radius - ry;
        in_corner = 1;
      } else if (rx >= rect.w - radius && ry < radius) {
        cx = rx - (rect.w - radius - 1);
        cy = radius - ry;
        in_corner = 1;
      }

      if (in_corner) {
        int dist_fp = int_sqrt((cx * cx + cy * cy) << 16);
        uint32_t alpha = smooth_edge_alpha(dist_fp, r_fp, base_alpha);
        if (alpha == 0)
          continue;
        row[px] = blend_pixel_fast((alpha << 24) | rgb, row[px]);
      } else {
        row[px] = blend_pixel_fast(color, row[px]);
      }
    }
  }
}

void gfx_draw_rect_outline(uint32_t *dst, int dst_stride, rect_t clip,
                           rect_t rect, uint32_t color) {
  gfx_blend_rect(dst, dst_stride, clip, (rect_t){rect.x, rect.y, rect.w, 1},
                 color);
  gfx_blend_rect(dst, dst_stride, clip,
                 (rect_t){rect.x, rect.y + rect.h - 1, rect.w, 1}, color);
  gfx_blend_rect(dst, dst_stride, clip, (rect_t){rect.x, rect.y, 1, rect.h},
                 color);
  gfx_blend_rect(dst, dst_stride, clip,
                 (rect_t){rect.x + rect.w - 1, rect.y, 1, rect.h}, color);
}

void gfx_draw_rounded_border(uint32_t *dst, int dst_stride, rect_t clip,
                             rect_t rect, int radius, uint32_t color) {
  if (!dst || rect.w <= 0 || rect.h <= 0)
    return;

  int start_x = MAX(rect.x, clip.x);
  int start_y = MAX(rect.y, clip.y);
  int end_x = MIN(rect.x + rect.w, clip.x + clip.w);
  int end_y = MIN(rect.y + rect.h, clip.y + clip.h);

  if (start_x >= end_x || start_y >= end_y)
    return;

  uint32_t base_alpha = (color >> 24) & 0xFF;
  uint32_t rgb = color & 0x00FFFFFF;
  int r_fp = radius << 8;

  for (int py = start_y; py < end_y; py++) {
    uint32_t *row = &dst[py * dst_stride];
    for (int px = start_x; px < end_x; px++) {
      int rx = px - rect.x;
      int ry = py - rect.y;

      int in_corner = 0;
      int cx = 0, cy = 0;

      if (rx < radius && ry < radius) {
        cx = radius - rx;
        cy = radius - ry;
        in_corner = 1;
      } else if (rx >= rect.w - radius && ry < radius) {
        cx = rx - (rect.w - radius - 1);
        cy = radius - ry;
        in_corner = 1;
      } else if (rx < radius && ry >= rect.h - radius) {
        cx = radius - rx;
        cy = ry - (rect.h - radius - 1);
        in_corner = 1;
      } else if (rx >= rect.w - radius && ry >= rect.h - radius) {
        cx = rx - (rect.w - radius - 1);
        cy = ry - (rect.h - radius - 1);
        in_corner = 1;
      }

      if (in_corner) {
        int dist_fp = int_sqrt((cx * cx + cy * cy) << 16);
        int diff = dist_fp - r_fp;
        uint32_t alpha = smooth_ring_alpha(diff, base_alpha);
        if (alpha == 0)
          continue;
        row[px] = blend_pixel_fast((alpha << 24) | rgb, row[px]);
      } else {
        if (rx == 0 || rx == rect.w - 1 || ry == 0 || ry == rect.h - 1) {
          row[px] = blend_pixel_fast(color, row[px]);
        }
      }
    }
  }
}

void gfx_draw_shadow(uint32_t *dst, int dst_stride, rect_t clip,
                     rect_t win_rect, int corner_radius, int shadow_size) {
  if (!dst || shadow_size <= 0)
    return;

  // [MEJORA] Más capas (paso de 3 en vez de 5) para un degradado de sombra
  // más suave y "caro" visualmente, con una curva de caída cuadrática en
  // vez de lineal (se ve más parecido a una sombra real tipo Win11/macOS).
  for (int i = shadow_size; i >= 3; i -= 3) {
    float progress = (float)i / (float)shadow_size;
    float falloff = 1.0f - progress;
    uint8_t alpha = (uint8_t)(42.0f * falloff * falloff + 6.0f * falloff);
    if (alpha == 0)
      continue;

    int expand = i;
    rect_t s_rect = {win_rect.x - expand, win_rect.y - expand + (i / 2),
                     win_rect.w + (expand * 2), win_rect.h + (expand * 2)};

    int r = corner_radius + expand;
    gfx_fill_rounded_rect(dst, dst_stride, clip, s_rect, r,
                          ((uint32_t)alpha << 24) | 0x00000000);
  }
}

void gfx_draw_line(uint32_t *dst, int dst_stride, rect_t clip, int x0, int y0,
                   int x1, int y1, uint32_t color) {
  if (!dst)
    return;
  int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
  int sx = (x0 < x1) ? 1 : -1;
  int dy = (y1 > y0) ? (y0 - y1) : (y1 - y0);
  int sy = (y0 < y1) ? 1 : -1;
  int err = dx + dy;

  while (1) {
    if (x0 >= clip.x && x0 < clip.x + clip.w && y0 >= clip.y &&
        y0 < clip.y + clip.h) {
      dst[y0 * dst_stride + x0] =
          blend_pixel_fast(color, dst[y0 * dst_stride + x0]);
    }
    if (x0 == x1 && y0 == y1)
      break;
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void gfx_draw_string(uint32_t *dst, int dst_stride, rect_t clip, int x, int y,
                     const char *str, uint32_t color, font_id_t font_id) {
  if (!dst || !str)
    return;

  const font_aa_t *font = font_manager_get(font_id);
  if (font) {
    gfx_draw_string_aa(dst, dst_stride, clip, x, y, str, color, font);
  }
}

void gfx_bit_blat(uint32_t *dst, int dst_stride, const uint32_t *src,
                  int src_stride, rect_t clip, int dst_x, int dst_y, int w,
                  int h) {
  if (!dst || !src || w <= 0 || h <= 0)
    return;

  int start_x = MAX(dst_x, clip.x);
  int start_y = MAX(dst_y, clip.y);
  int end_x = MIN(dst_x + w, clip.x + clip.w);
  int end_y = MIN(dst_y + h, clip.y + clip.h);

  if (start_x >= end_x || start_y >= end_y)
    return;

  for (int py = start_y; py < end_y; py++) {
    int sy = py - dst_y;
    if (sy < 0 || sy >= h)
      continue;

    uint32_t *d_row = &dst[py * dst_stride];
    const uint32_t *s_row = &src[sy * src_stride];
    int sx = start_x - dst_x;
    int count = end_x - start_x;

    /* El clip ya garantiza que sx >= 0 && sx + count <= w. */
    simd_blit_row(&d_row[start_x], &s_row[sx], count);
  }
}

void gfx_bit_blat_rounded(uint32_t *dst, int dst_stride, const uint32_t *src,
                          int src_w, int src_h, rect_t clip, int dst_x,
                          int dst_y, int win_x, int win_y, int win_w, int win_h,
                          int radius) {
  if (!dst || !src || src_w <= 0 || src_h <= 0)
    return;

  int start_x = MAX(dst_x, clip.x);
  int start_y = MAX(dst_y, clip.y);
  int end_x = MIN(dst_x + src_w, clip.x + clip.w);
  int end_y = MIN(dst_y + src_h, clip.y + clip.h);
  if (start_x >= end_x || start_y >= end_y)
    return;

  /* Esquinas inferiores redondeadas del content. */
  int corner_x_l = win_x + radius;         // fin esquina izq
  int corner_x_r = win_x + win_w - radius; // inicio esquina der
  int corner_y_t = win_y + win_h - radius; // inicio banda inferior

  for (int py = start_y; py < end_y; py++) {
    int sy = py - dst_y;
    if (sy < 0 || sy >= src_h)
      continue;
    uint32_t *d_row = &dst[py * dst_stride];
    const uint32_t *s_row = &src[sy * src_w];

    int row_in_bottom = (py >= corner_y_t);

    if (!row_in_bottom) {
      /* Fila sin esquinas: blit directo SIMD */
      int sx = start_x - dst_x;
      int n = end_x - start_x;
      simd_blit_row(&d_row[start_x], &s_row[sx], n);
      continue;
    }

    /* Fila en banda de esquinas inferiores: tramos */
    int mid_start = MAX(start_x, corner_x_l);
    int mid_end = MIN(end_x, corner_x_r);
    if (mid_start < mid_end) {
      int sx = mid_start - dst_x;
      simd_blit_row(&d_row[mid_start], &s_row[sx], mid_end - mid_start);
    }
    /* Esquinas izq y der, escalar */
    int left_end = MIN(end_x, corner_x_l);
    for (int px = start_x; px < left_end; px++) {
      int sx = px - dst_x;
      int rx = px - win_x;
      int ry = py - win_y;
      int cx = radius - rx;
      int cy = ry - (win_h - radius - 1);
      int d2 = cx * cx + cy * cy;
      float d = kernel_sqrtf((float)(d2 << 16)) / 256.0f;
      float diff = d - (float)radius;
      uint32_t s = s_row[sx];
      uint32_t sa = (s >> 24) & 0xFF;
      uint32_t alpha;
      if (diff >= 1.0f)
        alpha = 0;
      else if (diff <= -1.0f)
        alpha = sa;
      else {
        float tt = (1.0f - diff) * 0.5f;
        float ss = tt * tt * (3.0f - 2.0f * tt);
        alpha = (uint32_t)(sa * ss);
      }
      if (alpha == 0)
        continue;
      d_row[px] = blend_pixel_fast((alpha << 24) | (s & 0x00FFFFFF), d_row[px]);
    }
    int right_start = MAX(start_x, corner_x_r);
    for (int px = right_start; px < end_x; px++) {
      int sx = px - dst_x;
      int rx = px - win_x;
      int ry = py - win_y;
      int cx = rx - (win_w - radius - 1);
      int cy = ry - (win_h - radius - 1);
      int d2 = cx * cx + cy * cy;
      float d = kernel_sqrtf((float)(d2 << 16)) / 256.0f;
      float diff = d - (float)radius;
      uint32_t s = s_row[sx];
      uint32_t sa = (s >> 24) & 0xFF;
      uint32_t alpha;
      if (diff >= 1.0f)
        alpha = 0;
      else if (diff <= -1.0f)
        alpha = sa;
      else {
        float tt = (1.0f - diff) * 0.5f;
        float ss = tt * tt * (3.0f - 2.0f * tt);
        alpha = (uint32_t)(sa * ss);
      }
      if (alpha == 0)
        continue;
      d_row[px] = blend_pixel_fast((alpha << 24) | (s & 0x00FFFFFF), d_row[px]);
    }
  }
}

// ============================================================================
// Aurora compositing helpers
// ============================================================================

static inline uint32_t lerp_argb(uint32_t a, uint32_t b, int t /*0..256*/) {
  if (t <= 0)
    return a;
  if (t >= 256)
    return b;
  uint32_t aa = (a >> 24) & 0xFF, ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF,
           ab = a & 0xFF;
  uint32_t ba = (b >> 24) & 0xFF, br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF,
           bb = b & 0xFF;
  uint32_t oa = (aa * (256 - t) + ba * t + 128) >> 8;
  uint32_t orr = (ar * (256 - t) + br * t + 128) >> 8;
  uint32_t og = (ag * (256 - t) + bg * t + 128) >> 8;
  uint32_t ob = (ab * (256 - t) + bb * t + 128) >> 8;
  return (oa << 24) | (orr << 16) | (og << 8) | ob;
}

void gfx_gradient_rect_v(uint32_t *dst, int dst_stride, rect_t clip,
                         rect_t rect, uint32_t top_color,
                         uint32_t bottom_color) {
  if (!dst || rect.w <= 0 || rect.h <= 0)
    return;

  int sx = MAX(rect.x, clip.x);
  int sy = MAX(rect.y, clip.y);
  int ex = MIN(rect.x + rect.w, clip.x + clip.w);
  int ey = MIN(rect.y + rect.h, clip.y + clip.h);
  if (sx >= ex || sy >= ey)
    return;

  int denom = rect.h > 1 ? rect.h - 1 : 1;
  for (int py = sy; py < ey; py++) {
    int t = ((py - rect.y) * 256) / denom;
    uint32_t c = lerp_argb(top_color, bottom_color, t);
    uint32_t *row = &dst[py * dst_stride];
    uint32_t alpha = (c >> 24) & 0xFF;
    if (alpha == 255) {
      for (int px = sx; px < ex; px++)
        row[px] = c;
    } else {
      for (int px = sx; px < ex; px++)
        row[px] = blend_pixel_fast(c, row[px]);
    }
  }
}

void gfx_gradient_rect_h(uint32_t *dst, int dst_stride, rect_t clip,
                         rect_t rect, uint32_t left_color,
                         uint32_t right_color) {
  if (!dst || rect.w <= 0 || rect.h <= 0)
    return;

  int sx = MAX(rect.x, clip.x);
  int sy = MAX(rect.y, clip.y);
  int ex = MIN(rect.x + rect.w, clip.x + clip.w);
  int ey = MIN(rect.y + rect.h, clip.y + clip.h);
  if (sx >= ex || sy >= ey)
    return;

  int denom = rect.w > 1 ? rect.w - 1 : 1;
  uint32_t *first_row = &dst[sy * dst_stride];
  uint32_t cache_color = 0;
  int cache_x = -1;

  for (int px = sx; px < ex; px++) {
    int t = ((px - rect.x) * 256) / denom;
    uint32_t c = lerp_argb(left_color, right_color, t);
    uint32_t a = (c >> 24) & 0xFF;
    (void)cache_color;
    (void)cache_x;
    if (a == 255) {
      for (int py = sy; py < ey; py++)
        dst[py * dst_stride + px] = c;
    } else {
      for (int py = sy; py < ey; py++)
        dst[py * dst_stride + px] =
            blend_pixel_fast(c, dst[py * dst_stride + px]);
    }
  }
  (void)first_row;
}

void gfx_fill_rounded_rect_gradient_v(uint32_t *dst, int dst_stride,
                                      rect_t clip, rect_t rect, int radius,
                                      uint32_t top_color,
                                      uint32_t bottom_color) {
  if (!dst || rect.w <= 0 || rect.h <= 0)
    return;

  int sx = MAX(rect.x, clip.x);
  int sy = MAX(rect.y, clip.y);
  int ex = MIN(rect.x + rect.w, clip.x + clip.w);
  int ey = MIN(rect.y + rect.h, clip.y + clip.h);
  if (sx >= ex || sy >= ey)
    return;

  int denom = rect.h > 1 ? rect.h - 1 : 1;
  int r = radius;
  int x_left_corner_end = rect.x + r;
  int x_right_corner_beg = rect.x + rect.w - r;
  int y_top_corner_end = rect.y + r;
  int y_bot_corner_beg = rect.y + rect.h - r;
  int r_fp = r << 8;

  for (int py = sy; py < ey; py++) {
    int t = ((py - rect.y) * 256) / denom;
    uint32_t c = lerp_argb(top_color, bottom_color, t);
    uint32_t base_a = (c >> 24) & 0xFF;
    uint32_t rgb = c & 0x00FFFFFF;
    uint32_t *row = &dst[py * dst_stride];

    int row_in_corner = (py < y_top_corner_end) || (py >= y_bot_corner_beg);

    if (!row_in_corner) {
      /* Fila completa: sin esquinas. Relleno SIMD. */
      if (base_a == 255) {
        simd_fill_row(&row[sx], c, ex - sx);
      } else {
        /* La surface está limpia, blend = store del color con su alpha */
        for (int px = sx; px < ex; px++)
          row[px] = c;
      }
      continue;
    }

    /* Fila en banda de esquinas: rellenar tramos laterales escalares */
    for (int px = sx; px < ex; px++) {
      int rx = px - rect.x;
      int ry = py - rect.y;
      int in_corner = 0, cx = 0, cy = 0;
      if (rx < r && ry < r) {
        cx = r - rx;
        cy = r - ry;
        in_corner = 1;
      } else if (rx >= rect.w - r && ry < r) {
        cx = rx - (rect.w - r - 1);
        cy = r - ry;
        in_corner = 1;
      } else if (rx < r && ry >= rect.h - r) {
        cx = r - rx;
        cy = ry - (rect.h - r - 1);
        in_corner = 1;
      } else if (rx >= rect.w - r && ry >= rect.h - r) {
        cx = rx - (rect.w - r - 1);
        cy = ry - (rect.h - r - 1);
        in_corner = 1;
      }
      if (in_corner) {
        int d2 = cx * cx + cy * cy;
        float d = kernel_sqrtf((float)(d2 << 16)) / 256.0f;
        float diff = d - (float)r;
        uint32_t alpha;
        if (diff >= 1.0f)
          alpha = 0;
        else if (diff <= -1.0f)
          alpha = base_a;
        else {
          float tt = (1.0f - diff) * 0.5f;
          float ss = tt * tt * (3.0f - 2.0f * tt);
          alpha = (uint32_t)(base_a * ss);
        }
        if (alpha == 0)
          continue;
        row[px] = (alpha << 24) | rgb;
      } else {
        row[px] = c;
      }
    }
  }
}