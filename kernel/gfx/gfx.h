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