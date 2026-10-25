// kernel/gfx/gfx.c
#include "gfx.h"
#include "../cpu.h"
#include "../simd.h"
#include "font_aa.h"
#include "font_manager.h"
#include "theme.h"
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
/* Dos capas (theme.h): contacto (corta, casi sin desplazar) y ambiental
 * (ancha, desplazada hacia abajo). Cada una tiene su LUT indexada por d² hasta
 * (R + alcance)², así el bucle no hace raíces. Dentro de la forma de cada capa
 * el alpha es máximo: con desplazamiento > 0 la forma asoma por debajo de la
 * ventana y si fuera 0 quedaría una franja sin sombra pegada al borde. El
 * cuerpo de la ventana tapa el resto. */
#define AURORA_SHADOW_RADIUS AURORA_CORNER_RADIUS
#define SHADOW_AMB_D2_MAX                                                      \
  ((AURORA_SHADOW_RADIUS + AURORA_SHADOW_BLUR) *                               \
       (AURORA_SHADOW_RADIUS + AURORA_SHADOW_BLUR) +                           \
   1)
#define SHADOW_KEY_D2_MAX                                                      \
  ((AURORA_SHADOW_RADIUS + AURORA_SHADOW_KEY_BLUR) *                           \
       (AURORA_SHADOW_RADIUS + AURORA_SHADOW_KEY_BLUR) +                       \
   1)

static uint8_t g_shadow_amb[SHADOW_AMB_D2_MAX];
static uint8_t g_shadow_key[SHADOW_KEY_D2_MAX];
static int g_shadow_lut_ready = 0;

/* Caída suave de amax (sdf = 0) a 0 (sdf = reach): smoothstep al cuadrado,
 * que deja una cola larga y difusa como la de macOS. */
static void shadow_fill_lut(uint8_t *lut, int d2_max, int reach, int amax) {
  const int R = AURORA_SHADOW_RADIUS;
  for (int d2 = 0; d2 < d2_max; d2++) {
    int d = 0;
    while ((d + 1) * (d + 1) <= d2)
      d++;
    int sdf = d - R;
    if (sdf < 0) {
      lut[d2] = (uint8_t)amax;
      continue;
    }
    if (sdf >= reach) {
      lut[d2] = 0;
      continue;
    }
    int x = ((reach - sdf) * 1024) / reach;              /* 1024 -> 0 */
    int s = (3 * x * x - (2 * x * x * x) / 1024) / 1024; /* smoothstep */
    s = (s * s) / 1024;
    lut[d2] = (uint8_t)((amax * s) / 1024);
  }
}

static void shadow_lut_init(void) {
  if (g_shadow_lut_ready)
    return;
  shadow_fill_lut(g_shadow_amb, SHADOW_AMB_D2_MAX, AURORA_SHADOW_BLUR,
                  AURORA_SHADOW_AMB_ALPHA);
  shadow_fill_lut(g_shadow_key, SHADOW_KEY_D2_MAX, AURORA_SHADOW_KEY_BLUR,
                  AURORA_SHADOW_KEY_ALPHA);
  g_shadow_lut_ready = 1;
}

/* d² de (px, py) al rectángulo redondeado [x0, x1] x [y0, y1] (ya encogido R)
 * desplazado oy hacia abajo. */
static inline int shadow_d2(int px, int py, int x0, int y0, int x1, int y1,
                            int oy) {
  int sy = py - oy;
  int dy = sy < y0 ? y0 - sy : (sy > y1 ? sy - y1 : 0);
  int dx = px < x0 ? x0 - px : (px > x1 ? px - x1 : 0);
  return dx * dx + dy * dy;
}

void gfx_draw_shadow_aurora_ex(uint32_t *dst, int dst_stride, rect_t clip,
                               rect_t win_rect, int corner_radius,
                               int strength256) {
  if (!dst || strength256 <= 0)
    return;
  if (strength256 > 256)
    strength256 = 256;
  shadow_lut_init();

  int wx = win_rect.x, wy = win_rect.y;
  int ww = win_rect.w, wh = win_rect.h;
  const int R = corner_radius;
  const int OA = AURORA_SHADOW_OFFSET_Y, OK = AURORA_SHADOW_KEY_OFFSET_Y;

  int sx0 = wx + R, sy0 = wy + R;
  int sx1 = wx + ww - R, sy1 = wy + wh - R;

  /* La capa ambiental es la más grande: su caja contiene a la de contacto. */
  int bx0 = MAX(wx - AURORA_SHADOW_BLUR, clip.x);
  int by0 = MAX(MIN(wy - AURORA_SHADOW_BLUR + OA,
                    wy - AURORA_SHADOW_KEY_BLUR + OK),
                clip.y);
  int bx1 = MIN(wx + ww + AURORA_SHADOW_BLUR, clip.x + clip.w);
  int by1 = MIN(wy + wh + AURORA_SHADOW_BLUR + OA, clip.y + clip.h);
  if (bx0 >= bx1 || by0 >= by1)
    return;

  for (int py = by0; py < by1; py++) {
    uint32_t *row = &dst[py * dst_stride];

    /* El núcleo de la ventana (rect encogido R px) siempre queda cubierto por
     * el cuerpo: no gastar tiempo sombreándolo. */
    int skip0 = 0, skip1 = 0;
    if (py >= wy + R && py < wy + wh - R) {
      skip0 = wx + R;
      skip1 = wx + ww - R;
    }

    for (int px = bx0; px < bx1; px++) {
      if (px >= skip0 && px < skip1) {
        px = skip1 - 1;
        continue;
      }
      int da = shadow_d2(px, py, sx0, sy0, sx1, sy1, OA);
      int dk = shadow_d2(px, py, sx0, sy0, sx1, sy1, OK);
      uint32_t a = 0;
      if (da < SHADOW_AMB_D2_MAX)
        a += g_shadow_amb[da];
      if (dk < SHADOW_KEY_D2_MAX)
        a += g_shadow_key[dk];
      a = (a * (uint32_t)strength256) >> 8;
      if (a == 0)
        continue;
      if (a > 255)
        a = 255;
      row[px] = blend_pixel_fast((a << 24), row[px]);
    }
  }
}

// Función auxiliar de mezcla ponderada para la sombra
void gfx_draw_shadow_aurora(uint32_t *dst, int dst_stride, rect_t clip,
                            rect_t win_rect, int corner_radius) {
  gfx_draw_shadow_aurora_ex(dst, dst_stride, clip, win_rect, corner_radius,
                            256);
}

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

// ---------------------------------------------------------------------------
// Rectángulos redondeados por tramos. La geometría (y por tanto cada píxel) es
// la misma que la de la versión por píxel original: misma distancia al centro
// del arco y misma precedencia entre esquinas cuando el radio supera la mitad
// del lado. La diferencia es que solo las columnas de las esquinas se evalúan
// una a una; el resto de cada fila es un tramo de color constante. Antes el
// borde recorría el área entera del rectángulo píxel a píxel, y eso es lo que
// costaba cada vez que el terminal actualizaba su contenido.
// ---------------------------------------------------------------------------

/* Tramo [x0, x1) de una fila con color constante. */
static inline void span_fill(uint32_t *row, int x0, int x1, uint32_t color) {
  if (x0 >= x1)
    return;
  if ((color >> 24) == 0xFF) {
    simd_fill_row(&row[x0], color, (size_t)(x1 - x0));
    return;
  }
  for (int px = x0; px < x1; px++)
    row[px] = blend_pixel_fast(color, row[px]);
}

static inline void corner_fill_px(uint32_t *p, int cx, int cy, int r_fp,
                                  uint32_t base_alpha, uint32_t rgb) {
  int dist_fp = int_sqrt((cx * cx + cy * cy) << 16);
  uint32_t a = smooth_edge_alpha(dist_fp, r_fp, base_alpha);
  if (a)
    *p = blend_pixel_fast((a << 24) | rgb, *p);
}

static inline void corner_ring_px(uint32_t *p, int cx, int cy, int r_fp,
                                  uint32_t base_alpha, uint32_t rgb) {
  int dist_fp = int_sqrt((cx * cx + cy * cy) << 16);
  uint32_t a = smooth_ring_alpha(dist_fp - r_fp, base_alpha);
  if (a)
    *p = blend_pixel_fast((a << 24) | rgb, *p);
}

/* Fila de la banda de esquinas (superior o inferior) del relleno: cy es la
 * distancia vertical al centro del arco, [sx, ex) el tramo visible. */
static void rounded_fill_row(uint32_t *row, rect_t rect, int r, int sx, int ex,
                             int cy, uint32_t color) {
  uint32_t base_alpha = color >> 24, rgb = color & 0x00FFFFFF;
  int r_fp = r << 8;
  int lend = MIN(rect.x + MIN(r, rect.w), ex);
  int rbeg = MAX(rect.x + MAX(r, rect.w - r), sx);
  for (int px = sx; px < lend; px++)
    corner_fill_px(&row[px], r - (px - rect.x), cy, r_fp, base_alpha, rgb);
  span_fill(row, MAX(sx, rect.x + r), MIN(ex, rect.x + rect.w - r), color);
  for (int px = rbeg; px < ex; px++)
    corner_fill_px(&row[px], (px - rect.x) - (rect.w - r - 1), cy, r_fp,
                   base_alpha, rgb);
}

static void rounded_fill(uint32_t *dst, int dst_stride, rect_t clip,
                         rect_t rect, int r, uint32_t color, int bottom) {
  if (!dst || rect.w <= 0 || rect.h <= 0)
    return;
  int sx = MAX(rect.x, clip.x);
  int sy = MAX(rect.y, clip.y);
  int ex = MIN(rect.x + rect.w, clip.x + clip.w);
  int ey = MIN(rect.y + rect.h, clip.y + clip.h);
  if (sx >= ex || sy >= ey)
    return;
  if (r < 0)
    r = 0;

  for (int py = sy; py < ey; py++) {
    uint32_t *row = &dst[py * dst_stride];
    int ry = py - rect.y;
    if (ry < r)
      rounded_fill_row(row, rect, r, sx, ex, r - ry, color);
    else if (bottom && ry >= rect.h - r)
      rounded_fill_row(row, rect, r, sx, ex, ry - (rect.h - r - 1), color);
    else
      span_fill(row, sx, ex, color);
  }
}

void gfx_fill_rounded_rect(uint32_t *dst, int dst_stride, rect_t clip,
                           rect_t rect, int radius, uint32_t color) {
  rounded_fill(dst, dst_stride, clip, rect, radius, color, 1);
}

void gfx_fill_top_rounded_rect(uint32_t *dst, int dst_stride, rect_t clip,
                               rect_t rect, int radius, uint32_t color) {
  rounded_fill(dst, dst_stride, clip, rect, radius, color, 0);
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

/* Borde (anillo de ~1 px) de un rect redondeado. Coste proporcional al
 * perímetro: en las filas centrales solo se tocan los dos píxeles laterales. */
void gfx_draw_rounded_border(uint32_t *dst, int dst_stride, rect_t clip,
                             rect_t rect, int radius, uint32_t color) {
  if (!dst || rect.w <= 0 || rect.h <= 0)
    return;

  int sx = MAX(rect.x, clip.x);
  int sy = MAX(rect.y, clip.y);
  int ex = MIN(rect.x + rect.w, clip.x + clip.w);
  int ey = MIN(rect.y + rect.h, clip.y + clip.h);
  if (sx >= ex || sy >= ey)
    return;

  uint32_t base_alpha = (color >> 24) & 0xFF;
  uint32_t rgb = color & 0x00FFFFFF;
  int r = radius < 0 ? 0 : radius;
  int r_fp = r << 8;
  int lx = rect.x, rx = rect.x + rect.w - 1;

  for (int py = sy; py < ey; py++) {
    uint32_t *row = &dst[py * dst_stride];
    int ry = py - rect.y;
    int edge_row = (ry == 0 || ry == rect.h - 1);
    int cy;
    if (ry < r)
      cy = r - ry;
    else if (ry >= rect.h - r)
      cy = ry - (rect.h - r - 1);
    else
      cy = -1;

    if (cy < 0) { /* fila central: solo los laterales */
      if (edge_row) {
        span_fill(row, sx, ex, color);
      } else {
        if (lx >= sx && lx < ex)
          row[lx] = blend_pixel_fast(color, row[lx]);
        if (rx != lx && rx >= sx && rx < ex)
          row[rx] = blend_pixel_fast(color, row[rx]);
      }
      continue;
    }

    int lend = MIN(rect.x + MIN(r, rect.w), ex);
    int rbeg = MAX(rect.x + MAX(r, rect.w - r), sx);
    for (int px = sx; px < lend; px++)
      corner_ring_px(&row[px], r - (px - rect.x), cy, r_fp, base_alpha, rgb);
    if (edge_row)
      span_fill(row, MAX(sx, rect.x + r), MIN(ex, rect.x + rect.w - r), color);
    for (int px = rbeg; px < ex; px++)
      corner_ring_px(&row[px], (px - rect.x) - (rect.w - r - 1), cy, r_fp,
                     base_alpha, rgb);
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

// ============================================================================
// Métricas de texto
// ============================================================================
int gfx_text_width(const char *str, font_id_t font) {
  return font_aa_text_width(font_manager_get(font), str);
}

int gfx_font_ascent(font_id_t font) {
  const font_aa_t *f = font_manager_get(font);
  return f ? f->height : 0;
}

int gfx_text_center_y(font_id_t font, int top, int height) {
  const font_aa_t *f = font_manager_get(font);
  if (!f)
    return top;
  int cap = font_aa_cap_height(f);
  /* baseline = y + f->height; queremos baseline = centro + cap/2 */
  return top + (height + cap) / 2 - f->height;
}

// ============================================================================
// Desenfoque de fondo localizado (Dock, barra de menús, menús)
// ============================================================================
static inline int blur_in_shape(int x, int y, int w, int h, int cr) {
  if (cr <= 0)
    return 1;
  int dx = (x < cr) ? (cr - 1 - x) : (x >= w - cr ? x - (w - cr) : -1);
  int dy = (y < cr) ? (cr - 1 - y) : (y >= h - cr ? y - (h - cr) : -1);
  if (dx < 0 || dy < 0)
    return 1;
  return 4 * (dx * dx + dx + dy * dy + dy) + 2 <= 4 * cr * cr;
}

static inline int blur_clampi(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static inline uint32_t blur_pack(uint32_t r, uint32_t g, uint32_t b,
                                 int sat256) {
  if (sat256 != 256) {
    int lum = (int)((77 * r + 150 * g + 29 * b) >> 8);
    int rr = lum + (((int)r - lum) * sat256) / 256;
    int gg = lum + (((int)g - lum) * sat256) / 256;
    int bb = lum + (((int)b - lum) * sat256) / 256;
    r = (uint32_t)(rr < 0 ? 0 : (rr > 255 ? 255 : rr));
    g = (uint32_t)(gg < 0 ? 0 : (gg > 255 ? 255 : gg));
    b = (uint32_t)(bb < 0 ? 0 : (bb > 255 ? 255 : bb));
  }
  return 0xFF000000 | (r << 16) | (g << 8) | b;
}

/* Sumas por columna de la pasada vertical. Solo las usa el hilo del
 * compositor (Dock, menús y tooltips se pintan desde él). */
#define BLUR_MAX_W 4096
static uint32_t g_blur_sr[BLUR_MAX_W], g_blur_sg[BLUR_MAX_W],
    g_blur_sb[BLUR_MAX_W];

/* Box blur separable de radio blur_radius sobre el rectángulo r, con los
 * bordes de r replicados. Solo calcula la parte visible o = clip ∩ r: la
 * pasada horizontal recorre las filas de o ampliadas blur_radius (lo único que
 * la vertical necesita) y la vertical va fila a fila con sumas por columna, en
 * vez de por columnas (que salta de fila en fila y no aprovecha la caché). El
 * resultado dentro de o es idéntico al de desenfocar r entero; el que llama
 * debe tener compuesto el fondo de o ampliado blur_radius (ver
 * compositor.c). tmp necesita o.w * (o.h + 2 * blur_radius) píxeles. */
static void blur_core(uint32_t *dst, int dst_stride, rect_t clip, rect_t r,
                      int corner_radius, int blur_radius, int sat256,
                      uint32_t *tmp, size_t tmp_px) {
  if (!dst || !tmp || blur_radius < 1 || r.w <= 0 || r.h <= 0)
    return;
  rect_t o = rect_clip(clip, r);
  if (o.w <= 0 || o.h <= 0 || o.w > BLUR_MAX_W)
    return;

  const int br = blur_radius;
  const int xl = r.x, xr = r.x + r.w - 1;
  const int sy0 = MAX(r.y, o.y - br);
  const int sy1 = MIN(r.y + r.h, o.y + o.h + br); /* exclusivo */
  const int ow = o.w;
  if ((size_t)ow * (size_t)(sy1 - sy0) > tmp_px)
    return;
  const uint32_t mul = 65536u / (uint32_t)(2 * br + 1);

  /* Pasada horizontal: filas [sy0, sy1) de dst -> tmp (solo columnas de o) */
  for (int y = sy0; y < sy1; y++) {
    const uint32_t *row = &dst[y * dst_stride];
    uint32_t *out = &tmp[(y - sy0) * ow];
    uint32_t sr = 0, sg = 0, sb = 0;
    for (int i = -br; i <= br; i++) {
      uint32_t p = row[blur_clampi(o.x + i, xl, xr)];
      sr += (p >> 16) & 0xFF;
      sg += (p >> 8) & 0xFF;
      sb += p & 0xFF;
    }
    for (int x = 0; x < ow; x++) {
      out[x] = 0xFF000000 | (((sr * mul) >> 16) << 16) |
               (((sg * mul) >> 16) << 8) | ((sb * mul) >> 16);
      uint32_t pa = row[blur_clampi(o.x + x + br + 1, xl, xr)];
      uint32_t ps = row[blur_clampi(o.x + x - br, xl, xr)];
      sr += ((pa >> 16) & 0xFF) - ((ps >> 16) & 0xFF);
      sg += ((pa >> 8) & 0xFF) - ((ps >> 8) & 0xFF);
      sb += (pa & 0xFF) - (ps & 0xFF);
    }
  }

  /* Pasada vertical: tmp -> dst, fila a fila. Las filas fuera de r se
   * replican desde su borde, que siempre está dentro de [sy0, sy1). */
#define TMP_ROW(yy) (&tmp[(blur_clampi((yy), sy0, sy1 - 1) - sy0) * ow])
  for (int x = 0; x < ow; x++)
    g_blur_sr[x] = g_blur_sg[x] = g_blur_sb[x] = 0;
  for (int i = -br; i <= br; i++) {
    const uint32_t *t = TMP_ROW(o.y + i);
    for (int x = 0; x < ow; x++) {
      g_blur_sr[x] += (t[x] >> 16) & 0xFF;
      g_blur_sg[x] += (t[x] >> 8) & 0xFF;
      g_blur_sb[x] += t[x] & 0xFF;
    }
  }
  for (int y = o.y; y < o.y + o.h; y++) {
    uint32_t *drow = &dst[y * dst_stride + o.x];
    int ly = y - r.y;
    /* Fuera de las bandas de esquina toda la fila está dentro de la forma. */
    int corner_band = corner_radius > 0 &&
                      (ly < corner_radius || ly >= r.h - corner_radius);
    for (int x = 0; x < ow; x++) {
      if (corner_band &&
          !blur_in_shape(o.x + x - r.x, ly, r.w, r.h, corner_radius))
        continue;
      drow[x] = blur_pack((g_blur_sr[x] * mul) >> 16,
                          (g_blur_sg[x] * mul) >> 16,
                          (g_blur_sb[x] * mul) >> 16, sat256);
    }
    const uint32_t *ta = TMP_ROW(y + br + 1);
    const uint32_t *ts = TMP_ROW(y - br);
    for (int x = 0; x < ow; x++) {
      g_blur_sr[x] += ((ta[x] >> 16) & 0xFF) - ((ts[x] >> 16) & 0xFF);
      g_blur_sg[x] += ((ta[x] >> 8) & 0xFF) - ((ts[x] >> 8) & 0xFF);
      g_blur_sb[x] += (ta[x] & 0xFF) - (ts[x] & 0xFF);
    }
  }
#undef TMP_ROW
}

/* Región de fondo que hay que tener compuesta para poder desenfocar la parte
 * visible (clip ∩ r) de un panel de cristal de radio blur. */
rect_t gfx_glass_source_rect(rect_t clip, rect_t r, int blur) {
  rect_t o = rect_clip(clip, r);
  if (o.w <= 0 || o.h <= 0)
    return (rect_t){0, 0, 0, 0};
  return rect_clip(rect_grow(o, blur), r);
}

void gfx_backdrop_blur_rounded(uint32_t *dst, int dst_stride, rect_t clip,
                               rect_t r, int corner_radius, int blur_radius,
                               uint32_t *tmp, size_t tmp_px) {
  blur_core(dst, dst_stride, clip, r, corner_radius, blur_radius, 256, tmp,
            tmp_px);
}

// ============================================================================
// Cristal líquido (estilo Tahoe)
//   1. fondo desenfocado y más saturado (solo dentro de la forma redondeada)
//   2. tinte
//   3. brillo suave en la mitad superior
//   4. borde especular: 1 px, intenso arriba, tenue en los lados, algo de luz
//      abajo; más un segundo filete interior muy sutil
// ============================================================================
static inline int lerp_i(int a, int b, int t256) {
  return a + (((b - a) * t256) >> 8);
}

void gfx_glass_panel(uint32_t *dst, int dst_stride, rect_t clip, rect_t r,
                     int radius, uint32_t tint, int blur_radius, int sat256,
                     uint32_t *tmp, size_t tmp_px) {
  if (r.w <= 0 || r.h <= 0)
    return;
  int cr = radius;
  if (cr > r.h / 2)
    cr = r.h / 2;
  if (cr > r.w / 2)
    cr = r.w / 2;

  if (tmp && blur_radius > 0)
    blur_core(dst, dst_stride, clip, r, cr, blur_radius, sat256, tmp, tmp_px);

  gfx_fill_rounded_rect(dst, dst_stride, clip, r, cr, tint);

  /* Brillo superior: filas de blanco que se desvanece hacia el centro,
   * recortadas por el contorno redondeado. (gfx_fill_rounded_rect_gradient_v
   * ignora el alfa, por eso no se usa aquí.) */
  int gh = (r.h / 2 > 6) ? r.h / 2 : r.h;
  for (int dy = 0; dy < gh; dy++) {
    int a = 0x24 * (gh - dy) / gh;
    if (a == 0)
      break;
    int inset = 0;
    if (dy < cr) {
      int d = cr - dy, v = cr * cr - d * d, s = 0;
      while ((s + 1) * (s + 1) <= v)
        s++;
      inset = cr - s;
    }
    gfx_blend_rect(dst, dst_stride, clip,
                   (rect_t){r.x + inset, r.y + dy, r.w - 2 * inset, 1},
                   ((uint32_t)a << 24) | 0x00FFFFFF);
  }

  /* Borde especular por bandas verticales (el gradiente va en el clip). */
  const int bands = r.h < 48 ? 4 : 8;
  for (int i = 0; i < bands; i++) {
    int y0 = r.y + (r.h * i) / bands;
    int y1 = r.y + (r.h * (i + 1)) / bands;
    int t = ((i * 2 + 1) * 256) / (bands * 2); /* 0..256 al centro de la banda */
    int a = (t < 128) ? lerp_i(0x88, 0x30, t * 2)
                      : lerp_i(0x30, 0x50, (t - 128) * 2);
    rect_t bc = rect_clip(clip, (rect_t){r.x - 1, y0, r.w + 2, y1 - y0});
    if (bc.w > 0 && bc.h > 0)
      gfx_draw_rounded_border(dst, dst_stride, bc, r, cr,
                              ((uint32_t)a << 24) | 0x00FFFFFF);
  }
  /* Filete interior, apenas visible, que da grosor al cristal. */
  if (r.w > 8 && r.h > 8)
    gfx_draw_rounded_border(dst, dst_stride, clip,
                            (rect_t){r.x + 1, r.y + 1, r.w - 2, r.h - 2},
                            cr > 1 ? cr - 1 : 0, 0x12FFFFFF);
}

// ============================================================================
// Formas vectoriales con antialiasing (semáforos, glifos, marcas, cursor).
// Cobertura = distancia con signo al borde, en píxeles, recortada a [0, 1]:
// un borde de 1 px de transición, igual que el resto de primitivas.
// ============================================================================
static inline float cov01(float v) {
  return v <= 0.0f ? 0.0f : (v >= 1.0f ? 1.0f : v);
}

static inline void blend_cov(uint32_t *p, uint32_t color, float cov) {
  uint32_t a = (uint32_t)((float)(color >> 24) * cov + 0.5f);
  if (a)
    *p = blend_pixel_fast((a << 24) | (color & 0x00FFFFFF), *p);
}

/* Caja [x0, x1) x [y0, y1) que contiene la forma, recortada al clip. */
static inline int shape_box(rect_t clip, float fx0, float fy0, float fx1,
                            float fy1, int *x0, int *y0, int *x1, int *y1) {
  *x0 = MAX((int)fx0 - 1, clip.x);
  *y0 = MAX((int)fy0 - 1, clip.y);
  *x1 = MIN((int)fx1 + 2, clip.x + clip.w);
  *y1 = MIN((int)fy1 + 2, clip.y + clip.h);
  return *x0 < *x1 && *y0 < *y1;
}

void gfx_fill_circle(uint32_t *dst, int dst_stride, rect_t clip, float cx,
                     float cy, float r, uint32_t color) {
  int x0, y0, x1, y1;
  if (!dst || r <= 0.0f ||
      !shape_box(clip, cx - r, cy - r, cx + r, cy + r, &x0, &y0, &x1, &y1))
    return;
  float inner = r > 1.0f ? (r - 1.0f) * (r - 1.0f) : 0.0f;
  for (int py = y0; py < y1; py++) {
    uint32_t *row = &dst[py * dst_stride];
    float dy = (float)py + 0.5f - cy;
    for (int px = x0; px < x1; px++) {
      float dx = (float)px + 0.5f - cx;
      float d2 = dx * dx + dy * dy;
      if (d2 <= inner) { /* interior: sin raíz */
        blend_cov(&row[px], color, 1.0f);
        continue;
      }
      float c = cov01(r + 0.5f - kernel_sqrtf(d2));
      if (c > 0.0f)
        blend_cov(&row[px], color, c);
    }
  }
}

void gfx_draw_capsule(uint32_t *dst, int dst_stride, rect_t clip, float x0,
                      float y0, float x1, float y1, float hw, uint32_t color) {
  int bx0, by0, bx1, by1;
  if (!dst || hw <= 0.0f ||
      !shape_box(clip, MIN(x0, x1) - hw, MIN(y0, y1) - hw, MAX(x0, x1) + hw,
                 MAX(y0, y1) + hw, &bx0, &by0, &bx1, &by1))
    return;
  float vx = x1 - x0, vy = y1 - y0;
  float len2 = vx * vx + vy * vy;
  for (int py = by0; py < by1; py++) {
    uint32_t *row = &dst[py * dst_stride];
    for (int px = bx0; px < bx1; px++) {
      float wx = (float)px + 0.5f - x0, wy = (float)py + 0.5f - y0;
      float t = len2 > 0.0f ? (wx * vx + wy * vy) / len2 : 0.0f;
      t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
      float dx = wx - vx * t, dy = wy - vy * t;
      float c = cov01(hw + 0.5f - kernel_sqrtf(dx * dx + dy * dy));
      if (c > 0.0f)
        blend_cov(&row[px], color, c);
    }
  }
}

void gfx_fill_triangle(uint32_t *dst, int dst_stride, rect_t clip, float x0,
                       float y0, float x1, float y1, float x2, float y2,
                       uint32_t color) {
  int bx0, by0, bx1, by1;
  if (!dst || !shape_box(clip, MIN(x0, MIN(x1, x2)), MIN(y0, MIN(y1, y2)),
                         MAX(x0, MAX(x1, x2)), MAX(y0, MAX(y1, y2)), &bx0,
                         &by0, &bx1, &by1))
    return;
  /* Orientación antihoraria para que la distancia sea positiva dentro. */
  float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
  if (area == 0.0f)
    return;
  if (area < 0.0f) {
    float tx = x1, ty = y1;
    x1 = x2;
    y1 = y2;
    x2 = tx;
    y2 = ty;
  }
  float ex[3] = {x0, x1, x2}, ey[3] = {y0, y1, y2};
  float nx[3], ny[3], nc[3];
  for (int i = 0; i < 3; i++) {
    int j = (i + 1) % 3;
    float dx = ex[j] - ex[i], dy = ey[j] - ey[i];
    float inv = 1.0f / kernel_sqrtf(dx * dx + dy * dy);
    nx[i] = -dy * inv;
    ny[i] = dx * inv;
    nc[i] = -(nx[i] * ex[i] + ny[i] * ey[i]);
  }
  for (int py = by0; py < by1; py++) {
    uint32_t *row = &dst[py * dst_stride];
    float fy = (float)py + 0.5f;
    for (int px = bx0; px < bx1; px++) {
      float fx = (float)px + 0.5f;
      float d = nx[0] * fx + ny[0] * fy + nc[0];
      for (int i = 1; i < 3; i++) {
        float di = nx[i] * fx + ny[i] * fy + nc[i];
        if (di < d)
          d = di;
      }
      float c = cov01(d + 0.5f);
      if (c > 0.0f)
        blend_cov(&row[px], color, c);
    }
  }
}
