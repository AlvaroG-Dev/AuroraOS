// kernel/gfx/gfx.h
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
  int x;
  int y;
  int w;
  int h;
} rect_t;

typedef enum {
  FONT_ID_MAIN_REGULAR = 0,
  FONT_ID_MAIN_BOLD,
  FONT_ID_TITLEBAR,
  FONT_ID_MONO,
  FONT_ID_MONO_BOLD,
  FONT_ID_COUNT
} font_id_t;

// Operaciones de color y mezcla
static inline uint32_t blend_pixel_fast(uint32_t src, uint32_t dst) {
  uint32_t sa = (src >> 24) & 0xFF;
  if (sa == 0)
    return dst;
  if (sa == 255)
    return src;

  // IMPORTANTE: el alpha de salida se calcula con la formula "src over dst"
  // en vez de forzarse a 0xFF. Cuando dst ya es opaco (framebuffer/backbuffer)
  // el resultado es identico al anterior. Cuando dst es transparente
  // (superficies offscreen como win->surface, usadas para sombras) esto
  // preserva la transparencia real en lugar de "sellar" el pixel como
  // opaco en el primer blend (que es lo que causaba sombras solidas/opacas
  // en vez de un degradado).
  uint32_t da = (dst >> 24) & 0xFF;
  uint32_t inv_sa = 255 - sa;

  uint32_t out_a = sa + (da * inv_sa) / 255;

  uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
  uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;

  uint32_t r, g, b;
  if (out_a == 0) {
    r = g = b = 0;
  } else {
    r = (sr * sa + dr * da * inv_sa / 255) / out_a;
    g = (sg * sa + dg * da * inv_sa / 255) / out_a;
    b = (sb * sa + db * da * inv_sa / 255) / out_a;
    /* out_a se trunca, así que con destino semitransparente el cociente puede
     * pasar de 255; sin recortar, el sobrante se cuela en el canal vecino al
     * empaquetar (franjas verdosas en los bordes de iconos y esquinas). */
    if (r > 255)
      r = 255;
    if (g > 255)
      g = 255;
    if (b > 255)
      b = 255;
  }

  return (out_a << 24) | (r << 16) | (g << 8) | b;
}

rect_t rect_bounding_box(rect_t a, rect_t b);
rect_t rect_clip(rect_t a, rect_t b);
int rect_intersects(rect_t a, rect_t b);

// Primitivas gráficas
void gfx_blend_rect(uint32_t *dst, int dst_stride, rect_t clip, rect_t rect,
                    uint32_t color);
void gfx_fill_rounded_rect(uint32_t *dst, int dst_stride, rect_t clip,
                           rect_t rect, int radius, uint32_t color);
void gfx_fill_top_rounded_rect(uint32_t *dst, int dst_stride, rect_t clip,
                               rect_t rect, int radius, uint32_t color);
void gfx_draw_rect_outline(uint32_t *dst, int dst_stride, rect_t clip,
                           rect_t rect, uint32_t color);
void gfx_draw_rounded_border(uint32_t *dst, int dst_stride, rect_t clip,
                             rect_t rect, int radius, uint32_t color);
void gfx_draw_shadow(uint32_t *dst, int dst_stride, rect_t clip, rect_t rect,
                     int radius, int shadow_size);
void gfx_draw_line(uint32_t *dst, int dst_stride, rect_t clip, int x0, int y0,
                   int x1, int y1, uint32_t color);

// Renderizado de texto
void gfx_draw_string(uint32_t *dst, int dst_stride, rect_t clip, int x, int y,
                     const char *str, uint32_t color, font_id_t font);

// Transferencia de buffers (Blit)
void gfx_bit_blat(uint32_t *dst, int dst_stride, const uint32_t *src,
                  int src_stride, rect_t clip, int dst_x, int dst_y, int w,
                  int h);
void gfx_bit_blat_rounded(uint32_t *dst, int dst_stride, const uint32_t *src,
                          int src_w, int src_h, rect_t clip, int dst_x,
                          int dst_y, int win_x, int win_y, int win_w, int win_h,
                          int radius);

// ---------------------------------------------------------------------------
// Aurora compositing helpers
// ---------------------------------------------------------------------------
/* Relleno con gradiente vertical entre dos colores. */
void gfx_gradient_rect_v(uint32_t *dst, int dst_stride, rect_t clip,
                         rect_t rect, uint32_t top_color,
                         uint32_t bottom_color);

/* Relleno con gradiente horizontal entre dos colores. */
void gfx_gradient_rect_h(uint32_t *dst, int dst_stride, rect_t clip,
                         rect_t rect, uint32_t left_color,
                         uint32_t right_color);

/* Relleno redondeado con gradiente vertical. */
void gfx_fill_rounded_rect_gradient_v(uint32_t *dst, int dst_stride,
                                      rect_t clip, rect_t rect, int radius,
                                      uint32_t top_color,
                                      uint32_t bottom_color);

/* Sombra de ventana (LUT precalculada). corner_radius debe coincidir con
 * AURORA_CORNER_RADIUS (la LUT se genera para ese radio). */
void gfx_draw_shadow_aurora(uint32_t *dst, int dst_stride, rect_t clip,
                            rect_t win_rect, int corner_radius);
/* Igual, con intensidad 0..256 (256 = normal). Ventanas inactivas: ~140. */
void gfx_draw_shadow_aurora_ex(uint32_t *dst, int dst_stride, rect_t clip,
                               rect_t win_rect, int corner_radius,
                               int strength256);

/* Desenfoque de fondo (box blur separable, 2 pasadas) sobre el rectángulo r
 * de dst, escribiendo solo dentro de la forma redondeada y del clip. Usa
 * `tmp` (>= r.w*r.h píxeles) como buffer intermedio. r debe estar
 * completamente dentro del buffer. */
void gfx_backdrop_blur_rounded(uint32_t *dst, int dst_stride, rect_t clip,
                               rect_t r, int corner_radius, int blur_radius,
                               uint32_t *tmp, size_t tmp_px);

/* Panel de "cristal líquido" (Dock, menús, tooltips): desenfoque + saturación
 * del fondo ya compuesto, tinte, brillo superior y borde especular. `tmp` es
 * el buffer de desenfoque (NULL = solo tinte). El rectángulo debe estar
 * dentro del buffer y el fondo debajo recién compuesto. */
void gfx_glass_panel(uint32_t *dst, int dst_stride, rect_t clip, rect_t r,
                     int radius, uint32_t tint, int blur_radius, int sat256,
                     uint32_t *tmp, size_t tmp_px);

/* Rectángulo con desenfoque de fondo (panel de cristal). El compositor lo usa
 * para saber qué fondo extra debe componer antes de pintar el cristal. */
typedef struct {
  rect_t r;
  int blur;
} glass_rect_t;

/* Fondo que debe estar compuesto para desenfocar clip ∩ r con radio blur. */
rect_t gfx_glass_source_rect(rect_t clip, rect_t r, int blur);

/* Formas vectoriales con antialiasing (coordenadas en píxeles, el centro del
 * píxel (x, y) es (x + 0.5, y + 0.5)). */
void gfx_fill_circle(uint32_t *dst, int dst_stride, rect_t clip, float cx,
                     float cy, float r, uint32_t color);
/* Segmento con extremos redondeados de semiancho hw. */
void gfx_draw_capsule(uint32_t *dst, int dst_stride, rect_t clip, float x0,
                      float y0, float x1, float y1, float hw, uint32_t color);
void gfx_fill_triangle(uint32_t *dst, int dst_stride, rect_t clip, float x0,
                       float y0, float x1, float y1, float x2, float y2,
                       uint32_t color);

static inline rect_t rect_grow(rect_t r, int d) {
  return (rect_t){r.x - d, r.y - d, r.w + 2 * d, r.h + 2 * d};
}

static inline int rect_contains(rect_t outer, rect_t inner) {
  return inner.x >= outer.x && inner.y >= outer.y &&
         inner.x + inner.w <= outer.x + outer.w &&
         inner.y + inner.h <= outer.y + outer.h;
}

/* Métricas de texto (UTF-8). */
int gfx_text_width(const char *str, font_id_t font);
int gfx_font_ascent(font_id_t font);
/* Devuelve la y donde dibujar para que la altura de mayúsculas quede
 * centrada verticalmente en [top, top+height). */
int gfx_text_center_y(font_id_t font, int top, int height);