// kernel/gfx/gfx_filter.c
#include "gfx_filter.h"
#include <stddef.h>

#define GF_MAX(a, b) ((a) > (b) ? (a) : (b))
#define GF_MIN(a, b) ((a) < (b) ? (a) : (b))

/* ---------- Helpers de alpha premultiplicado ---------- */
static inline uint32_t px_premul(uint32_t c) {
  uint32_t a = (c >> 24) & 0xFF;
  if (a == 0)
    return 0;
  if (a == 255)
    return c;
  uint32_t r = (c >> 16) & 0xFF;
  uint32_t g = (c >> 8) & 0xFF;
  uint32_t b = c & 0xFF;
  r = (r * a + 127) / 255;
  g = (g * a + 127) / 255;
  b = (b * a + 127) / 255;
  return (a << 24) | (r << 16) | (g << 8) | b;
}

static inline uint32_t px_unpremul(uint32_t c) {
  uint32_t a = (c >> 24) & 0xFF;
  if (a == 0)
    return 0;
  if (a == 255)
    return c;
  uint32_t r = (c >> 16) & 0xFF;
  uint32_t g = (c >> 8) & 0xFF;
  uint32_t b = c & 0xFF;
  r = (r * 255 + (a >> 1)) / a;
  g = (g * 255 + (a >> 1)) / a;
  b = (b * 255 + (a >> 1)) / a;
  if (r > 255)
    r = 255;
  if (g > 255)
    g = 255;
  if (b > 255)
    b = 255;
  return (a << 24) | (r << 16) | (g << 8) | b;
}

static inline uint32_t px_lerp_premul(uint32_t a, uint32_t b, int w0, int w1) {
  uint32_t alpha =
      (((a >> 24) & 0xFF) * w0 + ((b >> 24) & 0xFF) * w1 + 128) >> 8;
  uint32_t r = ((((a >> 16) & 0xFF) * w0 + ((b >> 16) & 0xFF) * w1) + 128) >> 8;
  uint32_t g = ((((a >> 8) & 0xFF) * w0 + ((b >> 8) & 0xFF) * w1) + 128) >> 8;
  uint32_t bl = (((a & 0xFF) * w0 + (b & 0xFF) * w1) + 128) >> 8;
  return (alpha << 24) | (r << 16) | (g << 8) | bl;
}

/* Bilineal con coords 8.8 fixed-point. */
static uint32_t sample_bilinear(const uint32_t *src, int sw, int sh, int fx,
                                int fy) {
  int x0 = fx >> 8;
  int y0 = fy >> 8;
  int wx = fx & 0xFF;
  int wy = fy & 0xFF;
  int x1 = x0 + 1;
  int y1 = y0 + 1;

  if (x0 < 0)
    x0 = 0;
  if (y0 < 0)
    y0 = 0;
  if (x1 < 0)
    x1 = 0;
  if (y1 < 0)
    y1 = 0;
  if (x0 >= sw)
    x0 = sw - 1;
  if (y0 >= sh)
    y0 = sh - 1;
  if (x1 >= sw)
    x1 = sw - 1;
  if (y1 >= sh)
    y1 = sh - 1;

  uint32_t c00 = px_premul(src[y0 * sw + x0]);
  uint32_t c10 = px_premul(src[y0 * sw + x1]);
  uint32_t c01 = px_premul(src[y1 * sw + x0]);
  uint32_t c11 = px_premul(src[y1 * sw + x1]);

  int wx0 = 256 - wx, wx1 = wx;
  int wy0 = 256 - wy, wy1 = wy;

  uint32_t top = px_lerp_premul(c00, c10, wx0, wx1);
  uint32_t bot = px_lerp_premul(c01, c11, wx0, wx1);
  return px_lerp_premul(top, bot, wy0, wy1);
}

/* Box filter para reducciones fuertes: promedio pesado por alpha. */
static uint32_t sample_box(const uint32_t *src, int sw, int sh, int fx0,
                           int fy0, int fx1, int fy1) {
  int x0 = fx0 >> 8;
  int y0 = fy0 >> 8;
  int x1 = (fx1 + 255) >> 8;
  int y1 = (fy1 + 255) >> 8;

  if (x0 < 0)
    x0 = 0;
  if (y0 < 0)
    y0 = 0;
  if (x1 > sw)
    x1 = sw;
  if (y1 > sh)
    y1 = sh;
  if (x1 <= x0)
    x1 = x0 + 1;
  if (y1 <= y0)
    y1 = y0 + 1;
  if (x1 > sw)
    x1 = sw;
  if (y1 > sh)
    y1 = sh;

  uint64_t r_acc = 0, g_acc = 0, b_acc = 0, a_acc = 0;
  int count = 0;

  for (int y = y0; y < y1; y++) {
    const uint32_t *row = &src[y * sw];
    for (int x = x0; x < x1; x++) {
      uint32_t c = row[x];
      uint32_t a = (c >> 24) & 0xFF;
      r_acc += (uint64_t)((c >> 16) & 0xFF) * a;
      g_acc += (uint64_t)((c >> 8) & 0xFF) * a;
      b_acc += (uint64_t)(c & 0xFF) * a;
      a_acc += a;
      count++;
    }
  }
  if (count == 0 || a_acc == 0)
    return 0;

  /* out_a = promedio de alpha; r,g,b = promedio ponderado por alpha
   * (que es exactamente el color "unpremultiplied" medio). */
  uint32_t out_a = (uint32_t)((a_acc + count / 2) / count);
  uint32_t r = (uint32_t)((r_acc + a_acc / 2) / a_acc);
  uint32_t g = (uint32_t)((g_acc + a_acc / 2) / a_acc);
  uint32_t b = (uint32_t)((b_acc + a_acc / 2) / a_acc);
  if (r > 255)
    r = 255;
  if (g > 255)
    g = 255;
  if (b > 255)
    b = 255;
  return (out_a << 24) | (r << 16) | (g << 8) | b;
}

static void gfx_blit_scaled_impl(uint32_t *dst, int dst_stride, rect_t clip,
                                 int dx, int dy, int dw, int dh,
                                 const uint32_t *src, int sw, int sh,
                                 gfx_filter_t filter, uint8_t alpha_mul) {
  if (!dst || !src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
    return;

  int start_x = GF_MAX(dx, clip.x);
  int start_y = GF_MAX(dy, clip.y);
  int end_x = GF_MIN(dx + dw, clip.x + clip.w);
  int end_y = GF_MIN(dy + dh, clip.y + clip.h);
  if (start_x >= end_x || start_y >= end_y)
    return;

  if (filter == GFX_FILTER_AUTO)
    filter = GFX_FILTER_BILINEAR;

  int step_x = (sw << 8) / dw;
  int step_y = (sh << 8) / dh;
  if (step_x < 1)
    step_x = 1;
  if (step_y < 1)
    step_y = 1;

  int use_box = (filter == GFX_FILTER_BILINEAR) &&
                (step_x > 256 || step_y > 256); /* reducción */

  for (int py = start_y; py < end_y; py++) {
    int fy_center = ((py - dy) * sh * 256) / dh + (sh * 128) / dh - 128;
    uint32_t *drow = &dst[py * dst_stride];

    for (int px = start_x; px < end_x; px++) {
      int fx_center = ((px - dx) * sw * 256) / dw + (sw * 128) / dw - 128;

      uint32_t c;
      if (filter == GFX_FILTER_NEAREST) {
        int sx = (fx_center + 128) >> 8;
        int sy = (fy_center + 128) >> 8;
        if (sx < 0)
          sx = 0;
        if (sy < 0)
          sy = 0;
        if (sx >= sw)
          sx = sw - 1;
        if (sy >= sh)
          sy = sh - 1;
        c = src[sy * sw + sx];
      } else if (use_box) {
        c = sample_box(src, sw, sh, fx_center - step_x / 2,
                       fy_center - step_y / 2, fx_center + step_x / 2,
                       fy_center + step_y / 2);
      } else {
        c = px_unpremul(sample_bilinear(src, sw, sh, fx_center, fy_center));
      }

      if (alpha_mul < 255) {
        if (alpha_mul == 0)
          continue;
        uint32_t a = (c >> 24) & 0xFF;
        a = (a * alpha_mul + 127) / 255;
        c = (a << 24) | (c & 0x00FFFFFF);
      }

      drow[px] = blend_pixel_fast(c, drow[px]);
    }
  }
}

void gfx_blit_scaled(uint32_t *dst, int dst_stride, rect_t clip, int dx, int dy,
                     int dw, int dh, const uint32_t *src, int sw, int sh,
                     gfx_filter_t filter) {
  gfx_blit_scaled_impl(dst, dst_stride, clip, dx, dy, dw, dh, src, sw, sh,
                       filter, 255);
}

void gfx_blit_scaled_alpha(uint32_t *dst, int dst_stride, rect_t clip, int dx,
                           int dy, int dw, int dh, const uint32_t *src, int sw,
                           int sh, gfx_filter_t filter, uint8_t alpha) {
  gfx_blit_scaled_impl(dst, dst_stride, clip, dx, dy, dw, dh, src, sw, sh,
                       filter, alpha);
}

void gfx_blit_with_alpha(uint32_t *dst, int dst_stride, const uint32_t *src,
                         int src_stride, rect_t clip, int dst_x, int dst_y,
                         int w, int h, uint8_t alpha) {
  if (!dst || !src || alpha == 0)
    return;

  int start_x = GF_MAX(dst_x, clip.x);
  int start_y = GF_MAX(dst_y, clip.y);
  int end_x = GF_MIN(dst_x + w, clip.x + clip.w);
  int end_y = GF_MIN(dst_y + h, clip.y + clip.h);
  if (start_x >= end_x || start_y >= end_y)
    return;

  if (alpha >= 255) {
    /* Fast path: delega en gfx_bit_blat. */
    gfx_bit_blat(dst, dst_stride, src, src_stride, clip, dst_x, dst_y, w, h);
    return;
  }

  for (int py = start_y; py < end_y; py++) {
    int sy = py - dst_y;
    uint32_t *drow = &dst[py * dst_stride];
    const uint32_t *srow = &src[sy * src_stride];

    for (int px = start_x; px < end_x; px++) {
      int sx = px - dst_x;
      uint32_t s = srow[sx];
      uint32_t sa = (s >> 24) & 0xFF;
      if (sa == 0)
        continue;
      sa = (sa * alpha + 127) / 255;
      if (sa == 0)
        continue;
      uint32_t c = (sa << 24) | (s & 0x00FFFFFF);
      drow[px] = blend_pixel_fast(c, drow[px]);
    }
  }
}