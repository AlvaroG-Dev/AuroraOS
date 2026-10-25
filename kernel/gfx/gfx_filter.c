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

  /* Box solo en reducciones fuertes (iconos del Dock): con reducciones
   * suaves (la animación de apertura escala a ~0.94) promediaría 2x2 píxeles
   * y emborronaría la ventana entera. */
  int use_box = (filter == GFX_FILTER_BILINEAR) &&
                (step_x > 320 || step_y > 320);

  /* fx_center(px) = ((px - dx) * sw * 256) / dw + (sw * 128) / dw - 128,
   * calculado de forma incremental (cociente + resto) sin dividir por
   * píxel. */
  const int fx_off = (sw * 128) / dw - 128;
  const int fx_num_step = sw * 256;
  const int fx_q_step = fx_num_step / dw, fx_r_step = fx_num_step % dw;

  for (int py = start_y; py < end_y; py++) {
    int fy_center = ((py - dy) * sh * 256) / dh + (sh * 128) / dh - 128;
    uint32_t *drow = &dst[py * dst_stride];
    int fx_num0 = (start_x - dx) * sw * 256;
    int fx_q = fx_num0 / dw, fx_r = fx_num0 % dw;

    for (int px = start_x; px < end_x; px++) {
      int fx_center = fx_q + fx_off;
      fx_q += fx_q_step;
      fx_r += fx_r_step;
      if (fx_r >= dw) {
        fx_q++;
        fx_r -= dw;
      }

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
/* Escalado bilineal de una imagen con alfa PREMULTIPLICADO sobre un destino
 * opaco o con alfa. Sin divisiones: los canales se interpolan de dos en dos
 * (R/B y A/G en un entero de 32 bits) y se mezcla con dst*(256-a)/256 + src.
 * Es el camino de los iconos del Dock, que se redibujan en cada movimiento del
 * ratón. dw <= PREMUL_MAX_W. */
#define PREMUL_MAX_W 160

void gfx_blit_scaled_premul(uint32_t *dst, int dst_stride, rect_t clip, int dx,
                            int dy, int dw, int dh, const uint32_t *src, int sw,
                            int sh) {
  static int xi0[PREMUL_MAX_W], xi1[PREMUL_MAX_W], xwt[PREMUL_MAX_W];
  if (!dst || !src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 ||
      dw > PREMUL_MAX_W)
    return;

  int start_x = GF_MAX(dx, clip.x);
  int start_y = GF_MAX(dy, clip.y);
  int end_x = GF_MIN(dx + dw, clip.x + clip.w);
  int end_y = GF_MIN(dy + dh, clip.y + clip.h);
  if (start_x >= end_x || start_y >= end_y)
    return;

  for (int px = start_x; px < end_x; px++) {
    int fx = ((px - dx) * sw * 256) / dw + (sw * 128) / dw - 128;
    int x0 = fx >> 8, x1 = x0 + 1;
    int i = px - start_x;
    xwt[i] = fx & 0xFF;
    xi0[i] = x0 < 0 ? 0 : (x0 >= sw ? sw - 1 : x0);
    xi1[i] = x1 < 0 ? 0 : (x1 >= sw ? sw - 1 : x1);
  }

  const uint32_t M = 0x00FF00FFu;
  for (int py = start_y; py < end_y; py++) {
    int fy = ((py - dy) * sh * 256) / dh + (sh * 128) / dh - 128;
    int y0 = fy >> 8, y1 = y0 + 1, wy = fy & 0xFF;
    y0 = y0 < 0 ? 0 : (y0 >= sh ? sh - 1 : y0);
    y1 = y1 < 0 ? 0 : (y1 >= sh ? sh - 1 : y1);
    const uint32_t *r0 = &src[y0 * sw], *r1 = &src[y1 * sw];
    uint32_t *drow = &dst[py * dst_stride];

    for (int px = start_x; px < end_x; px++) {
      int i = px - start_x, wx = xwt[i];
      uint32_t c00 = r0[xi0[i]], c10 = r0[xi1[i]];
      uint32_t c01 = r1[xi0[i]], c11 = r1[xi1[i]];
      if ((c00 | c10 | c01 | c11) == 0)
        continue; /* cuatro píxeles transparentes: lo más común en los bordes */

      uint32_t w00 = (uint32_t)((256 - wx) * (256 - wy)) >> 8;
      uint32_t w10 = (uint32_t)(wx * (256 - wy)) >> 8;
      uint32_t w01 = (uint32_t)((256 - wx) * wy) >> 8;
      uint32_t w11 = 256 - w00 - w10 - w01;

      uint32_t rb = w00 * (c00 & M) + w10 * (c10 & M) + w01 * (c01 & M) +
                    w11 * (c11 & M);
      uint32_t ag = w00 * ((c00 >> 8) & M) + w10 * ((c10 >> 8) & M) +
                    w01 * ((c01 >> 8) & M) + w11 * ((c11 >> 8) & M);
      uint32_t p = ((rb >> 8) & M) | (((ag >> 8) & M) << 8);

      uint32_t a = p >> 24;
      if (a == 0)
        continue;
      if (a == 255) {
        drow[px] = p;
        continue;
      }
      uint32_t d = drow[px], inv = 256 - a;
      uint32_t t_rb = (((d & M) * inv) >> 8) & M;
      uint32_t t_ag = ((((d >> 8) & M) * inv) >> 8) & M;
      drow[px] = (t_rb | (t_ag << 8)) + p; /* cada canal <= 255: sin acarreo */
    }
  }
}
