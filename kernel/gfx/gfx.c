// kernel/gfx/gfx.c
#include "gfx.h"
#include "font_manager.h"
#include "font_aa.h"

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

// Raíz cuadrada entera rápida (Método de Newton) para anti-aliasing
static inline int int_sqrt(int n) {
  if (n <= 0) return 0;
  int x = n;
  int y = (x + 1) >> 1;
  while (y < x) {
    x = y;
    y = (x + n / x) >> 1;
  }
  return x;
}

// Función auxiliar de mezcla ponderada para la sombra
static inline void gfx_blend_shadow_pixel(uint32_t *dst, int x, int y, int stride, uint32_t base_color, uint8_t alpha_factor) {
    // Aquí puedes integrar tu función estándar de blend o esta optimizada para sombras
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
  if (!dst || rect.w <= 0 || rect.h <= 0) return;

  int start_x = MAX(rect.x, clip.x);
  int start_y = MAX(rect.y, clip.y);
  int end_x = MIN(rect.x + rect.w, clip.x + clip.w);
  int end_y = MIN(rect.y + rect.h, clip.y + clip.h);

  if (start_x >= end_x || start_y >= end_y) return;

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
        cx = radius - rx; cy = radius - ry; in_corner = 1;
      } else if (rx >= rect.w - radius && ry < radius) {
        cx = rx - (rect.w - radius - 1); cy = radius - ry; in_corner = 1;
      } else if (rx < radius && ry >= rect.h - radius) {
        cx = radius - rx; cy = ry - (rect.h - radius - 1); in_corner = 1;
      } else if (rx >= rect.w - radius && ry >= rect.h - radius) {
        cx = rx - (rect.w - radius - 1); cy = ry - (rect.h - radius - 1); in_corner = 1;
      }

      if (in_corner) {
        int dist_fp = int_sqrt((cx * cx + cy * cy) << 16);
        int diff = dist_fp - r_fp;

        if (diff > 128) continue;

        uint32_t alpha = base_alpha;
        if (diff > -128) {
          int coverage = 128 - diff;
          alpha = (base_alpha * coverage) >> 8;
        }
        row[px] = blend_pixel_fast((alpha << 24) | rgb, row[px]);
      } else {
        row[px] = blend_pixel_fast(color, row[px]);
      }
    }
  }
}

void gfx_fill_top_rounded_rect(uint32_t *dst, int dst_stride, rect_t clip,
                               rect_t rect, int radius, uint32_t color) {
  if (!dst || rect.w <= 0 || rect.h <= 0) return;

  int start_x = MAX(rect.x, clip.x);
  int start_y = MAX(rect.y, clip.y);
  int end_x = MIN(rect.x + rect.w, clip.x + clip.w);
  int end_y = MIN(rect.y + rect.h, clip.y + clip.h);

  if (start_x >= end_x || start_y >= end_y) return;

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
        cx = radius - rx; cy = radius - ry; in_corner = 1;
      } else if (rx >= rect.w - radius && ry < radius) {
        cx = rx - (rect.w - radius - 1); cy = radius - ry; in_corner = 1;
      }

      if (in_corner) {
        int dist_fp = int_sqrt((cx * cx + cy * cy) << 16);
        int diff = dist_fp - r_fp;

        if (diff > 128) continue;

        uint32_t alpha = base_alpha;
        if (diff > -128) {
          int coverage = 128 - diff;
          alpha = (base_alpha * coverage) >> 8;
        }
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
  if (!dst || rect.w <= 0 || rect.h <= 0) return;

  int start_x = MAX(rect.x, clip.x);
  int start_y = MAX(rect.y, clip.y);
  int end_x = MIN(rect.x + rect.w, clip.x + clip.w);
  int end_y = MIN(rect.y + rect.h, clip.y + clip.h);

  if (start_x >= end_x || start_y >= end_y) return;

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
        cx = radius - rx; cy = radius - ry; in_corner = 1;
      } else if (rx >= rect.w - radius && ry < radius) {
        cx = rx - (rect.w - radius - 1); cy = radius - ry; in_corner = 1;
      } else if (rx < radius && ry >= rect.h - radius) {
        cx = radius - rx; cy = ry - (rect.h - radius - 1); in_corner = 1;
      } else if (rx >= rect.w - radius && ry >= rect.h - radius) {
        cx = rx - (rect.w - radius - 1); cy = ry - (rect.h - radius - 1); in_corner = 1;
      }

      if (in_corner) {
        int dist_fp = int_sqrt((cx * cx + cy * cy) << 16);
        int diff = dist_fp - r_fp;

        int abs_diff = (diff < 0) ? -diff : diff;
        if (abs_diff <= 256) {
          int coverage = 256 - abs_diff;
          uint32_t alpha = (base_alpha * coverage) >> 8;
          row[px] = blend_pixel_fast((alpha << 24) | rgb, row[px]);
        }
      } else {
        if (rx == 0 || rx == rect.w - 1 || ry == 0 || ry == rect.h - 1) {
          row[px] = blend_pixel_fast(color, row[px]);
        }
      }
    }
  }
}

void gfx_draw_shadow(uint32_t *dst, int dst_stride, rect_t clip, rect_t win_rect, int corner_radius, int shadow_size) {
    if (!dst || shadow_size <= 0) return;

    // Dibujar capas concéntricas difuminadas hacia afuera
    for (int i = shadow_size; i >= 1; i--) {
        float progress = (float)i / (float)shadow_size;
        // Caída logarítmica suave para evitar el efecto "caja negra"
        uint8_t alpha = (uint8_t)(28 * (1.0f - progress)); 
        if (alpha == 0) continue;

        int expand = i;
        rect_t s_rect = {
            win_rect.x - expand,
            win_rect.y - expand + (i / 2), // Suave desplazamiento vertical hacia abajo
            win_rect.w + (expand * 2),
            win_rect.h + (expand * 2)
        };

        int r = corner_radius + expand;
        
        // Renderizado optimizado de caja con bordes redondeados para la sombra
        // Si ya tienes un rectangulo con esquinas redondeadas translúcido, úsalo aquí:
        gfx_fill_rounded_rect(dst, dst_stride, clip, s_rect, r, ((uint32_t)alpha << 24) | 0x00000000);
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

    for (int px = start_x; px < end_x; px++) {
      int sx = px - dst_x;
      if (sx < 0 || sx >= w)
        continue;
      d_row[px] = blend_pixel_fast(s_row[sx], d_row[px]);
    }
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

  int r2 = radius * radius;

  for (int py = start_y; py < end_y; py++) {
    int sy = py - dst_y;
    if (sy < 0 || sy >= src_h)
      continue;

    uint32_t *d_row = &dst[py * dst_stride];
    const uint32_t *s_row = &src[sy * src_w];

    for (int px = start_x; px < end_x; px++) {
      int sx = px - dst_x;
      if (sx < 0 || sx >= src_w)
        continue;

      int rx = px - win_x;
      int ry = py - win_y;

      int in_corner = 0;
      int cx = 0, cy = 0;

      if (rx < radius && ry >= win_h - radius) {
        cx = radius - rx;
        cy = ry - (win_h - radius - 1);
        in_corner = 1;
      } else if (rx >= win_w - radius && ry >= win_h - radius) {
        cx = rx - (win_w - radius - 1);
        cy = ry - (win_h - radius - 1);
        in_corner = 1;
      }

      if (in_corner && (cx * cx + cy * cy > r2)) {
        continue;
      }

      d_row[px] = blend_pixel_fast(s_row[sx], d_row[px]);
    }
  }
}