/* bootloader/common/aurora_splash_core.h
 *
 * Núcleo gráfico COMPARTIDO del splash (BIOS stage2, UEFI y kernel).
 * - Freestanding, solo enteros: sin float, sin libgcc (divisiones de 32 bits),
 *   sin memcpy/memset. Válido en PM32, long mode y UEFI.
 * - Wordmark vectorial con cobertura analítica (nítido a cualquier resolución).
 * - Todo es función pura del tiempo `ms`: cada etapa dibuja lo mismo y el
 *   traspaso bootloader -> kernel es invisible.
 * - Cada píxel se escribe UNA vez por frame (fondo+contenido ya compuestos):
 *   sin parpadeo y sin buffer sombra.
 */
#ifndef AURORA_SPLASH_CORE_H
#define AURORA_SPLASH_CORE_H
#include "aurora_logo_data.h"
#include "aurora_emblem_mask.h"
#include <stdint.h>


#define ASP_FN static __attribute__((unused))
#ifndef ASP_MAX_W
#define ASP_MAX_W 1024 /* >= ancho máximo del título (~930 px a cap 128) */
#endif

/* Cronología (ms) */
#define ASP_LETTER_STAGGER 55
#define ASP_LETTER_FADE 240
#define ASP_TITLE_END                                                          \
  (ASP_LETTER_STAGGER * (LOGO_NLETTERS - 1) + ASP_LETTER_FADE)
#define ASP_SPIN_START (ASP_TITLE_END - 150)
#define ASP_SPIN_FADE 350
#define ASP_STEP_MS 84 /* 12 pasos = ~1 vuelta/seg */

/* Paleta (negro-verdoso discreto; spinner en verde aurora #3EE89B) */
#define ASP_TOP_R 3
#define ASP_TOP_G 7
#define ASP_TOP_B 10
#define ASP_BOT_R 5
#define ASP_BOT_G 20
#define ASP_BOT_B 24
#define ASP_GLOW_R 2 /* niveles añadidos en el centro del halo */
#define ASP_GLOW_G 20
#define ASP_GLOW_B 16
#define ASP_COL_TITLE 0x00E6F2EEu
#define ASP_COL_SPIN 0x003EE89Bu
#define ASP_EMB_C0 0x006BF083u /* emblema: extremo izquierdo (verde) */
#define ASP_EMB_C1 0x0000D4D8u /* emblema: extremo derecho (turquesa) */
#define ASP_EMB_FADE 420
#define ASP_EMB_GAP_PCT 45 /* separación emblema-título, % de cap */

#define ASP_GAP_UNITS 230

typedef struct {
  int32_t w, h;
  int32_t cap, hw16, rise, padx, pady;
  int32_t mask_w, mask_h, title_x, title_y;
  int32_t ox16[LOGO_NLETTERS];
  int32_t rng[LOGO_NLETTERS + 1];
  uint8_t *mask;
  int32_t spin_cx, spin_cy, spin_r, dot_r16, spin_half;
  int32_t dotx16[12], doty16[12];
  int32_t glow_cx, glow_cy, glow_r2, glow_gain, glow_sh;
  int title_final;
  int32_t emb_x, emb_y, emb_w, emb_h;
  int emb_final;
} asp_t;

typedef void (*asp_row_fn)(void *ctx, int x, int y, int w, const uint32_t *px);

typedef struct {
  int32_t r, g, b, dy2;
} asp_row_t;

static const int16_t k_asp_cos[12] = {0, 512,  887,  1024,  887,  512,
                                      0, -512, -887, -1024, -887, -512};
static const int16_t k_asp_sin[12] = {-1024, -887, -512, 0, 512,  887,
                                      1024,  887,  512,  0, -512, -887};
static const uint8_t k_asp_trail[12] = {255, 214, 178, 146, 118, 94,
                                        74,  58,  46,  38,  32,  28};
static const uint8_t k_asp_bayer[16] = {0, 8,  2, 10, 12, 4, 14, 6,
                                        3, 11, 1, 9,  15, 7, 13, 5};

ASP_FN int32_t asp_clamp(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

ASP_FN void asp_zero(uint8_t *p, uint32_t n) {
  __asm__ volatile("rep stosb" : "+D"(p), "+c"(n) : "a"(0) : "memory");
}

/* smoothstep 0..255 -> 0..255 */
ASP_FN uint32_t asp_smooth(uint32_t t) {
  if (t > 255)
    t = 255;
  return (t * t * (765u - 2u * t)) / 65025u;
}

ASP_FN uint32_t asp_mix(uint32_t fg, uint32_t bg, uint32_t a) {
  if (a == 0)
    return bg;
  if (a >= 255)
    return fg | 0xFF000000u;
  uint32_t ia = 255 - a;
  uint32_t r = (((fg >> 16) & 255) * a + ((bg >> 16) & 255) * ia + 127) / 255;
  uint32_t g = (((fg >> 8) & 255) * a + ((bg >> 8) & 255) * ia + 127) / 255;
  uint32_t b = ((fg & 255) * a + (bg & 255) * ia + 127) / 255;
  return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* ---------------- Fondo analítico (degradado + halo + dither) ------------- */
ASP_FN void asp_row_begin(const asp_t *s, int y, asp_row_t *o) {
  int32_t t = (int32_t)(((uint32_t)y << 16) / (uint32_t)s->h);
  o->r = (ASP_TOP_R << 16) + (ASP_BOT_R - ASP_TOP_R) * t;
  o->g = (ASP_TOP_G << 16) + (ASP_BOT_G - ASP_TOP_G) * t;
  o->b = (ASP_TOP_B << 16) + (ASP_BOT_B - ASP_TOP_B) * t;
  int32_t dy = y - s->glow_cy;
  o->dy2 = dy * dy;
}

ASP_FN uint32_t asp_bg_px(const asp_t *s, const asp_row_t *o, int x, int y) {
  int32_t dx = x - s->glow_cx;
  int32_t d2 = dx * dx + o->dy2;
  int32_t g = 0;
  if (d2 < s->glow_r2) {
    g = (((s->glow_r2 - d2) >> s->glow_sh) * s->glow_gain) >> 8;
    if (g > 65536)
      g = 65536;
    g = (int32_t)(((uint32_t)g * (uint32_t)g) >> 16); /* caída suave */
  }
  int32_t th = (int32_t)k_asp_bayer[((y & 3) << 2) | (x & 3)] << 12;
  int32_t r = (o->r + g * ASP_GLOW_R + th) >> 16;
  int32_t gg = (o->g + g * ASP_GLOW_G + th) >> 16;
  int32_t b = (o->b + g * ASP_GLOW_B + th) >> 16;
  return 0xFF000000u | ((uint32_t)asp_clamp(r, 0, 255) << 16) |
         ((uint32_t)asp_clamp(gg, 0, 255) << 8) |
         (uint32_t)asp_clamp(b, 0, 255);
}

/* ---------------- Layout ----------------------------------------------------
 */
ASP_FN void asp_layout(asp_t *s, uint32_t w, uint32_t h) {
  int32_t units = 0, i;
  for (i = 0; i < LOGO_NLETTERS; i++)
    units += k_logo_glyph[i][0];
  units += ASP_GAP_UNITS * (LOGO_NLETTERS - 1);
  s->w = (int32_t)w;
  s->h = (int32_t)h;
  int32_t cap = asp_clamp((int32_t)h / 14, 20, 128);
  while (cap > 14 &&
         (units * cap / 1000 + 2 * (cap / 10 + 8)) > (int32_t)w * 7 / 10)
    cap--;
  s->cap = cap;
  s->hw16 = cap * 80 / 100;
  if (s->hw16 < 12)
    s->hw16 = 12;
  int32_t hwpx = (s->hw16 + 15) >> 4;
  s->rise = cap / 6 + 2;
  s->padx = hwpx + 4;
  s->pady = hwpx + 4 + s->rise;
  int32_t total_w = (units * cap + 500) / 1000;
  s->mask_w = total_w + 2 * s->padx;
  s->mask_h = cap + 2 * s->pady;

  int32_t xu = 0;
  for (i = 0; i < LOGO_NLETTERS; i++) {
    s->ox16[i] = s->padx * 16 + (xu * cap * 16) / 1000;
    s->rng[i] =
        (i == 0) ? 0 : s->padx + ((xu - ASP_GAP_UNITS / 2) * cap) / 1000;
    xu += k_logo_glyph[i][0] + ASP_GAP_UNITS;
  }
  s->rng[LOGO_NLETTERS] = s->mask_w;

  /* bloque vertical: título + separación + spinner, centrado (algo alto) */
  int32_t gap = cap * 90 / 100;
  s->spin_r = cap * 38 / 100;
  if (s->spin_r < 12)
    s->spin_r = 12;
  s->dot_r16 = s->spin_r * 16 * 15 / 100;
  if (s->dot_r16 < 28)
    s->dot_r16 = 28;
  s->spin_half = s->spin_r + (s->dot_r16 >> 4) + 3;
  int32_t eh = cap * 120 / 100, egap = cap * ASP_EMB_GAP_PCT / 100;
  s->emb_h = eh;
  s->emb_w = eh * ASP_EMB_W / ASP_EMB_H;
  int32_t block_h = eh + egap + cap + gap + 2 * s->spin_r;
  int32_t top0 = ((int32_t)h - block_h) / 2 - (int32_t)h / 40;
  s->emb_x = ((int32_t)w - s->emb_w) / 2;
  s->emb_y = top0;
  int32_t top = top0 + eh + egap;
  s->title_x = ((int32_t)w - total_w) / 2 - s->padx;
  s->title_y = top - s->pady;
  s->spin_cx = (int32_t)w / 2;
  s->spin_cy = top + cap + gap + s->spin_r;
  for (i = 0; i < 12; i++) {
    s->dotx16[i] = (s->spin_r * 16 * k_asp_cos[i]) / 1024;
    s->doty16[i] = (s->spin_r * 16 * k_asp_sin[i]) / 1024;
  }
  s->glow_cx = (int32_t)w / 2;
  s->glow_cy = top + block_h / 2;
  int32_t R = (int32_t)h * 60 / 100;
  s->glow_r2 = R * R;
  s->glow_sh = 0;
  while ((s->glow_r2 >> s->glow_sh) > 16384)
    s->glow_sh++;
  int32_t norm = s->glow_r2 >> s->glow_sh;
  s->glow_gain = norm > 0 ? (int32_t)((65536u * 256u) / (uint32_t)norm) : 1;
  s->title_final = 0;
  s->emb_final = 0;
}

/* ---------------- Máscara del título (se hace UNA vez) ---------------------
 */
ASP_FN void asp_stroke(asp_t *s, int32_t ax, int32_t ay, int32_t bx,
                       int32_t by) {
  int32_t hw = s->hw16, m = (hw >> 4) + 2;
  int32_t x0 = ((ax < bx ? ax : bx) >> 4) - m,
          x1 = ((ax > bx ? ax : bx) >> 4) + m;
  int32_t y0 = ((ay < by ? ay : by) >> 4) - m,
          y1 = ((ay > by ? ay : by) >> 4) + m;
  if (x0 < 0)
    x0 = 0;
  if (y0 < 0)
    y0 = 0;
  if (x1 >= s->mask_w)
    x1 = s->mask_w - 1;
  if (y1 >= s->mask_h)
    y1 = s->mask_h - 1;
  int32_t vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy;
  int32_t hw2 = hw * hw, lim = (hw + 16) * (hw + 16);
  for (int32_t y = y0; y <= y1; y++) {
    uint8_t *row = s->mask + (uint32_t)y * (uint32_t)s->mask_w;
    for (int32_t x = x0; x <= x1; x++) {
      int32_t wx = x * 16 + 8 - ax, wy = y * 16 + 8 - ay, dx, dy;
      int32_t dot = wx * vx + wy * vy;
      if (dot <= 0 || len2 == 0) {
        dx = wx;
        dy = wy;
      } else if (dot >= len2) {
        dx = wx - vx;
        dy = wy - vy;
      } else {
        int32_t t = (int32_t)(((uint32_t)dot << 8) / (uint32_t)len2);
        dx = wx - ((vx * t) >> 8);
        dy = wy - ((vy * t) >> 8);
      }
      int32_t d2 = dx * dx + dy * dy;
      if (d2 >= lim)
        continue;
      int32_t cov = 128 + ((hw2 - d2) / (2 * hw)) * 16;
      cov = asp_clamp(cov, 0, 255);
      if (cov > row[x])
        row[x] = (uint8_t)cov;
    }
  }
}

ASP_FN void asp_render_mask(asp_t *s) {
  asp_zero(s->mask, (uint32_t)s->mask_w * (uint32_t)s->mask_h);
  int32_t py = s->pady * 16;
  for (int i = 0; i < LOGO_NLETTERS; i++) {
    for (int p = 0; p < k_logo_glyph[i][2]; p++) {
      const uint16_t *pl = k_logo_poly[k_logo_glyph[i][1] + p];
      for (int k = 0; k + 1 < pl[1]; k++) {
        const int16_t *a = k_logo_pts[pl[0] + k],
                      *b = k_logo_pts[pl[0] + k + 1];
        asp_stroke(s, s->ox16[i] + (a[0] * s->cap * 16) / 1000,
                   py + (a[1] * s->cap * 16) / 1000,
                   s->ox16[i] + (b[0] * s->cap * 16) / 1000,
                   py + (b[1] * s->cap * 16) / 1000);
      }
    }
  }
}

/* ---------------- Dibujo ----------------------------------------------------
 */
ASP_FN void asp_fill_bg(const asp_t *s, asp_row_fn put, void *ctx,
                        uint32_t *line) {
  for (int y = 0; y < s->h; y++) {
    asp_row_t rw;
    asp_row_begin(s, y, &rw);
    for (int x0 = 0; x0 < s->w; x0 += ASP_MAX_W) {
      int n = s->w - x0 > ASP_MAX_W ? ASP_MAX_W : s->w - x0;
      for (int x = 0; x < n; x++)
        line[x] = asp_bg_px(s, &rw, x0 + x, y);
      put(ctx, x0, y, n, line);
    }
  }
}

ASP_FN uint32_t asp_letter_eased(int i, uint32_t ms) {
  uint32_t st = (uint32_t)i * ASP_LETTER_STAGGER;
  if (ms <= st)
    return 0;
  uint32_t e = ms - st;
  if (e >= ASP_LETTER_FADE)
    return 255;
  return asp_smooth(e * 255 / ASP_LETTER_FADE);
}

/* master: opacidad global 0..255 (fade-out del reveal) */
ASP_FN void asp_draw_title(const asp_t *s, uint32_t ms, uint32_t master,
                           asp_row_fn put, void *ctx, uint32_t *line) {
  uint32_t la[LOGO_NLETTERS];
  int32_t rise[LOGO_NLETTERS];
  for (int i = 0; i < LOGO_NLETTERS; i++) {
    uint32_t e = asp_letter_eased(i, ms);
    rise[i] = (int32_t)((255 - e) * (uint32_t)s->rise / 255);
    la[i] = (e * master + 127) / 255;
  }
  int32_t cx0 = s->title_x < 0 ? -s->title_x : 0;
  int32_t cx1 = s->mask_w;
  if (s->title_x + cx1 > s->w)
    cx1 = s->w - s->title_x;
  if (cx1 - cx0 > ASP_MAX_W)
    cx1 = cx0 + ASP_MAX_W;
  if (cx1 <= cx0)
    return;
  for (int32_t my = 0; my < s->mask_h; my++) {
    int32_t sy = s->title_y + my;
    if (sy < 0 || sy >= s->h)
      continue;
    asp_row_t rw;
    asp_row_begin(s, sy, &rw);
    for (int i = 0; i < LOGO_NLETTERS; i++) {
      int32_t a0 = s->rng[i] > cx0 ? s->rng[i] : cx0;
      int32_t a1 = s->rng[i + 1] < cx1 ? s->rng[i + 1] : cx1;
      int32_t sm = my - rise[i];
      const uint8_t *mrow = (sm >= 0 && sm < s->mask_h)
                                ? s->mask + (uint32_t)sm * (uint32_t)s->mask_w
                                : 0;
      for (int32_t px = a0; px < a1; px++) {
        int32_t sx = s->title_x + px;
        uint32_t c = asp_bg_px(s, &rw, sx, sy);
        if (mrow && la[i]) {
          uint32_t a = mrow[px];
          if (a)
            c = asp_mix(ASP_COL_TITLE, c, (a * la[i] + 127) / 255);
        }
        line[px - cx0] = c;
      }
    }
    put(ctx, s->title_x + cx0, sy, cx1 - cx0, line);
  }
}


/* Emblema (máscara alfa 8 bits, bilineal entera, degradado horizontal). */
ASP_FN uint32_t asp_emb_px(uint32_t x, uint32_t y) {
  uint32_t i = y * ASP_EMB_W + x;
  uint32_t n = (k_asp_emb[i >> 1] >> ((i & 1) ? 0 : 4)) & 15;
  return n * 17;
}

ASP_FN uint32_t asp_emb_alpha(const asp_t *s, int32_t x, int32_t y) {
  uint32_t fx = ((uint32_t)x * (ASP_EMB_W - 1) << 8) / (uint32_t)(s->emb_w - 1 ? s->emb_w - 1 : 1);
  uint32_t fy = ((uint32_t)y * (ASP_EMB_H - 1) << 8) / (uint32_t)(s->emb_h - 1 ? s->emb_h - 1 : 1);
  uint32_t x0 = fx >> 8, y0 = fy >> 8, ax = fx & 255, ay = fy & 255;
  uint32_t x1 = x0 + 1 < ASP_EMB_W ? x0 + 1 : x0, y1 = y0 + 1 < ASP_EMB_H ? y0 + 1 : y0;
  uint32_t a00 = asp_emb_px(x0, y0), a10 = asp_emb_px(x1, y0);
  uint32_t a01 = asp_emb_px(x0, y1), a11 = asp_emb_px(x1, y1);
  uint32_t top = a00 * (256 - ax) + a10 * ax, bot = a01 * (256 - ax) + a11 * ax;
  return (top * (256 - ay) + bot * ay) >> 16;
}

ASP_FN void asp_draw_emblem(const asp_t *s, uint32_t ms, asp_row_fn put,
                            void *ctx, uint32_t *line) {
  uint32_t f = ms >= ASP_EMB_FADE ? 255 : asp_smooth(ms * 255 / ASP_EMB_FADE);
  int32_t x0 = s->emb_x < 0 ? 0 : s->emb_x;
  int32_t x1 = s->emb_x + s->emb_w > s->w ? s->w : s->emb_x + s->emb_w;
  if (x1 - x0 > ASP_MAX_W)
    x1 = x0 + ASP_MAX_W;
  if (x1 <= x0)
    return;
  for (int32_t my = 0; my < s->emb_h; my++) {
    int32_t sy = s->emb_y + my;
    if (sy < 0 || sy >= s->h)
      continue;
    asp_row_t rw;
    asp_row_begin(s, sy, &rw);
    for (int32_t sx = x0; sx < x1; sx++) {
      uint32_t c = asp_bg_px(s, &rw, sx, sy);
      uint32_t a = asp_emb_alpha(s, sx - s->emb_x, my);
      if (a) {
        uint32_t t = (uint32_t)(sx - s->emb_x) * 255 / (uint32_t)(s->emb_w > 1 ? s->emb_w - 1 : 1);
        uint32_t r = (((ASP_EMB_C0 >> 16) & 255) * (255 - t) + ((ASP_EMB_C1 >> 16) & 255) * t) / 255;
        uint32_t g = (((ASP_EMB_C0 >> 8) & 255) * (255 - t) + ((ASP_EMB_C1 >> 8) & 255) * t) / 255;
        uint32_t b = ((ASP_EMB_C0 & 255) * (255 - t) + (ASP_EMB_C1 & 255) * t) / 255;
        c = asp_mix((r << 16) | (g << 8) | b, c, (a * f + 127) / 255);
      }
      line[sx - x0] = c;
    }
    put(ctx, x0, sy, x1 - x0, line);
  }
}

ASP_FN void asp_draw_spinner(const asp_t *s, uint32_t ms, uint32_t master,
                             asp_row_fn put, void *ctx, uint32_t *line) {
  if (ms <= ASP_SPIN_START)
    return;
  uint32_t f = (ms - ASP_SPIN_START) * 255 / ASP_SPIN_FADE;
  uint32_t amast = (asp_smooth(f) * master + 127) / 255;
  uint32_t step = ms / ASP_STEP_MS;
  uint32_t frac = (ms - step * ASP_STEP_MS) * 255 / ASP_STEP_MS;
  int head = (int)(step % 12);
  uint32_t br[12];
  for (int i = 0; i < 12; i++) {
    int d = (head - i + 12) % 12;
    uint32_t b0 = k_asp_trail[d], b1 = k_asp_trail[(d + 1) % 12];
    br[i] = (b0 * (255 - frac) + b1 * frac) / 255;
  }
  int32_t x0 = s->spin_cx - s->spin_half, x1 = s->spin_cx + s->spin_half + 1;
  int32_t y0 = s->spin_cy - s->spin_half, y1 = s->spin_cy + s->spin_half + 1;
  if (x0 < 0)
    x0 = 0;
  if (y0 < 0)
    y0 = 0;
  if (x1 > s->w)
    x1 = s->w;
  if (y1 > s->h)
    y1 = s->h;
  if (x1 - x0 > ASP_MAX_W)
    x1 = x0 + ASP_MAX_W;
  int32_t dr = s->dot_r16, dr2 = dr * dr, rej = dr + 16, rej2 = rej * rej;
  for (int32_t y = y0; y < y1; y++) {
    asp_row_t rw;
    asp_row_begin(s, y, &rw);
    for (int32_t x = x0; x < x1; x++) {
      uint32_t c = asp_bg_px(s, &rw, x, y);
      int32_t px16 = x * 16 + 8 - s->spin_cx * 16,
              py16 = y * 16 + 8 - s->spin_cy * 16;
      for (int i = 0; i < 12; i++) {
        int32_t dx = px16 - s->dotx16[i], dy = py16 - s->doty16[i];
        if (dx > rej || dx < -rej || dy > rej || dy < -rej)
          continue;
        int32_t d2 = dx * dx + dy * dy;
        if (d2 >= rej2)
          continue;
        int32_t cov = asp_clamp(128 + ((dr2 - d2) / (2 * dr)) * 16, 0, 255);
        if (cov) {
          uint32_t a = ((uint32_t)cov * br[i] / 255) * amast / 255;
          c = asp_mix(ASP_COL_SPIN, c, a);
        }
        break;
      }
      line[x - x0] = c;
    }
    put(ctx, x0, y, x1 - x0, line);
  }
}

/* Frame normal: el título solo se redibuja mientras anima. */
ASP_FN void asp_frame(asp_t *s, uint32_t ms, asp_row_fn put, void *ctx,
                      uint32_t *line) {
  if (ms < ASP_EMB_FADE) {
    asp_draw_emblem(s, ms, put, ctx, line);
    s->emb_final = 0;
  } else if (!s->emb_final) {
    asp_draw_emblem(s, ms, put, ctx, line);
    s->emb_final = 1;
  }
  if (ms < ASP_TITLE_END) {
    asp_draw_title(s, ms, 255, put, ctx, line);
    s->title_final = 0;
  } else if (!s->title_final) {
    asp_draw_title(s, ms, 255, put, ctx, line);
    s->title_final = 1;
  }
  asp_draw_spinner(s, ms, 255, put, ctx, line);
}

#endif