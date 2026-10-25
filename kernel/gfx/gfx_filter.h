// kernel/gfx/gfx_filter.h
#pragma once
#include "gfx.h"
#include <stdint.h>

typedef enum {
  GFX_FILTER_NEAREST = 0,
  GFX_FILTER_BILINEAR, // Bilineal pura (up-scale) / box (down-scale)
  GFX_FILTER_AUTO      // Elige automáticamente
} gfx_filter_t;

/* Escala src (sw x sh) al rect (dx,dy,dw,dh) sobre dst, mezclando con
 * blend_pixel_fast. Usa alpha PREMULTIPLICADO internamente para evitar
 * halos oscuros en bordes con transparencia. */
void gfx_blit_scaled(uint32_t *dst, int dst_stride, rect_t clip, int dx, int dy,
                     int dw, int dh, const uint32_t *src, int sw, int sh,
                     gfx_filter_t filter);

/* Igual que gfx_blit_scaled pero modula el alpha global por alpha/255.
 * Pensado para animaciones de fade-in/fade-out. */
void gfx_blit_scaled_alpha(uint32_t *dst, int dst_stride, rect_t clip, int dx,
                           int dy, int dw, int dh, const uint32_t *src, int sw,
                           int sh, gfx_filter_t filter, uint8_t alpha);

/* Escala src con alfa PREMULTIPLICADO (bilineal, sin divisiones) sobre dst.
 * Para imágenes que se redibujan a cada movimiento del ratón (iconos del
 * Dock). dw <= 160. */
void gfx_blit_scaled_premul(uint32_t *dst, int dst_stride, rect_t clip, int dx,
                            int dy, int dw, int dh, const uint32_t *src, int sw,
                            int sh);

/* Blit 1:1 modulando el alpha global (para blits de surface sin escala,
 * usado por la animación de opacidad de ventanas). */
void gfx_blit_with_alpha(uint32_t *dst, int dst_stride, const uint32_t *src,
                         int src_stride, rect_t clip, int dst_x, int dst_y,
                         int w, int h, uint8_t alpha);