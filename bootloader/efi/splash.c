/* bootloader/efi/splash.c */
#include "splash.h"
#include "font_aa.h"
#include "font_inter_regular14.h"

/* El compilador nos ha chivado el nombre exacto de la variable */
#define FONT_MAIN (font_inter_regular_14)

extern EFI_SYSTEM_TABLE *gST;
extern EFI_BOOT_SERVICES *gBS;

#define TITLE_STR "AuroraOS"
#define TITLE_LEN 8
#define FONT_SCALE 4
#define LETTER_FADE_MS 210
#define LETTER_STAGGER_MS 105
#define TITLE_TOTAL_MS (LETTER_STAGGER_MS * (TITLE_LEN - 1) + LETTER_FADE_MS)

#define SPIN_R 26
#define SPIN_DOT_R 4
#define SPIN_DOTS 12
#define SPIN_ROT_MS 900
#define SPIN_PAD (SPIN_R + SPIN_DOT_R + 8)

static uint8_t *g_fb = NULL;
static UINT32 g_w, g_h, g_pitch, g_bpp;
static UINT64 g_start = 0, g_last = 0;
static int g_active = 0;
static int g_spin_cx, g_spin_cy;
static int g_title_x, g_title_y, g_title_w, g_title_h;

/* 12 puntos precalculados, R=18 */
static const int16_t k_spin_dx[12] = {26, 22, 13, 0, -13, -22, -26, -22, -13, 0, 13, 22};
static const int16_t k_spin_dy[12] = {0, 13, 22, 26, 22, 13, 0, -13, -22, -26, -22, -13};

static UINT64 rdtsc_now(void) {
  UINT32 lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((UINT64)hi << 32) | lo;
}

static void put_px(int x, int y, UINT32 rgb) {
  if (x < 0 || y < 0 || x >= (int)g_w || y >= (int)g_h)
    return;
  if (g_bpp == 32) {
    UINT32 *p = (UINT32 *)(g_fb + (UINT64)y * g_pitch + (UINT64)x * 4);
    *p = rgb;
  } else if (g_bpp == 24) {
    UINT8 *p = g_fb + (UINT64)y * g_pitch + (UINT64)x * 3;
    p[0] = rgb & 0xFF;
    p[1] = (rgb >> 8) & 0xFF;
    p[2] = (rgb >> 16) & 0xFF;
  }
}

static void fill_rect(int x, int y, int w, int h, UINT32 rgb) {
  if (w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > (int)g_w) w = (int)g_w - x;
  if (y + h > (int)g_h) h = (int)g_h - y;
  if (w <= 0 || h <= 0) return;
  for (int yy = y; yy < y + h; yy++) {
    if (g_bpp == 32) {
      UINT32 *row = (UINT32 *)(g_fb + (UINT64)yy * g_pitch + (UINT64)x * 4);
      for (int xx = 0; xx < w; xx++) row[xx] = rgb;
    } else {
      UINT8 *row = g_fb + (UINT64)yy * g_pitch + (UINT64)x * 3;
      UINT8 b = (UINT8)rgb, g = (UINT8)(rgb >> 8), r = (UINT8)(rgb >> 16);
      for (int xx = 0; xx < w; xx++) { row[xx*3] = b; row[xx*3+1] = g; row[xx*3+2] = r; }
    }
  }
}

#define BG_TOP 0x060A13u
#define BG_BOTTOM 0x0B1426u
#define COL_TITLE 0xF1F5FFu
#define COL_BLUE 0x3B82F6u
#define COL_CYAN 0x60D5FFu
#define COL_VIOLET 0x8B9DFFu

static UINT32 bg_color(UINT32 y) {
  UINT32 t = (g_h > 1) ? (y * 255u) / (g_h - 1u) : 0;
  UINT32 r = 6u + (5u * t) / 255u;
  UINT32 g = 10u + (10u * t) / 255u;
  UINT32 b = 19u + (19u * t) / 255u;
  return (r << 16) | (g << 8) | b;
}
static void fill_bg_rect(int x, int y, int w, int h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > (int)g_w) w = (int)g_w - x;
  if (y + h > (int)g_h) h = (int)g_h - y;
  if (w <= 0 || h <= 0) return;
  for (int yy = y; yy < y + h; yy++) fill_rect(x, yy, w, 1, bg_color((UINT32)yy));
}
static void draw_background(void) {
  for (UINT32 y = 0; y < g_h; y++) fill_rect(0, (int)y, (int)g_w, 1, bg_color(y));
}
static void draw_segment(int x0, int y0, int x1, int y1, UINT32 color) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
  int dy = y0 > y1 ? y1 - y0 : y0 - y1, sy = y0 < y1 ? 1 : -1;
  dy = -dy;
  int err = dx + dy;
  for (;;) {
    fill_rect(x0 - 1, y0 - 1, 3, 3, color);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}
static void draw_logo(void) {
  int cx = (int)g_w / 2, cy = (int)g_h / 2 - 145;
  if (cy < 36) cy = 36;
  draw_segment(cx - 24, cy + 14, cx - 8, cy - 16, COL_BLUE);
  draw_segment(cx - 8, cy - 16, cx + 8, cy + 14, COL_CYAN);
  draw_segment(cx + 2, cy + 14, cx + 18, cy - 16, COL_VIOLET);
  draw_segment(cx + 18, cy - 16, cx + 28, cy + 4, COL_BLUE);
}
static UINT32 read_px(int x, int y) {
  UINT8 *p = g_fb + (UINT64)y * g_pitch + (UINT64)x * (g_bpp == 32 ? 4 : 3);
  if (g_bpp == 32) return (*(UINT32 *)p) & 0xFFFFFFu;
  return ((UINT32)p[2] << 16) | ((UINT32)p[1] << 8) | p[0];
}
static UINT32 blend_rgb(UINT32 fg, UINT32 bg, UINT32 a) {
  UINT32 ia = 255u - a;
  UINT32 r = ((((fg >> 16) & 255u) * a) + (((bg >> 16) & 255u) * ia)) / 255u;
  UINT32 g = ((((fg >> 8) & 255u) * a) + (((bg >> 8) & 255u) * ia)) / 255u;
  UINT32 b = (((fg & 255u) * a) + ((bg & 255u) * ia)) / 255u;
  return (r << 16) | (g << 8) | b;
}
static void draw_caption(void) {
  static const char caption[] = "INITIALIZING YOUR WORKSPACE";
  int width = 0;
  for (int i = 0; caption[i]; i++) width += FONT_MAIN.glyphs[(unsigned char)caption[i]].advance;
  int x = ((int)g_w - width) / 2, y = (int)g_h / 2 - 24;
  for (int i = 0; caption[i]; i++) {
    const glyph_aa_t *g = &FONT_MAIN.glyphs[(unsigned char)caption[i]];
    int bx = x + g->bearing_x, by = y + (FONT_MAIN.height - g->bearing_y);
    for (int gy = 0; gy < g->height; gy++) {
      int py = by + gy;
      if (py < 0 || py >= (int)g_h) continue;
      for (int gx = 0; gx < g->width; gx++) {
        int px = bx + gx;
        if (px < 0 || px >= (int)g_w) continue;
        UINT32 a = g->bitmap[gy * g->width + gx] / 2u;
        if (a) put_px(px, py, blend_rgb(0xB5C6DFu, read_px(px, py), a));
      }
    }
    x += g->advance;
  }
}

static inline uint8_t gsample(const glyph_aa_t *g, int x, int y) {
  if (x < 0)
    x = 0;
  if (y < 0)
    y = 0;
  if (x >= g->width)
    x = g->width - 1;
  if (y >= g->height)
    y = g->height - 1;
  return g->bitmap[y * g->width + x];
}

static inline uint8_t bilinear(const glyph_aa_t *g, int dx, int dy, int scale) {
  int sx = (dx * 2 + 1) * 65536 / (2 * scale) - 32768;
  int sy = (dy * 2 + 1) * 65536 / (2 * scale) - 32768;
  if (sx < 0)
    sx = 0;
  if (sy < 0)
    sy = 0;
  int max_x = (g->width - 1) << 16, max_y = (g->height - 1) << 16;
  if (sx > max_x)
    sx = max_x;
  if (sy > max_y)
    sy = max_y;
  int x0 = sx >> 16, y0 = sy >> 16;
  uint32_t fx = sx & 0xFFFF, fy = sy & 0xFFFF;
  uint32_t a00 = gsample(g, x0, y0), a10 = gsample(g, x0 + 1, y0);
  uint32_t a01 = gsample(g, x0, y0 + 1), a11 = gsample(g, x0 + 1, y0 + 1);
  uint32_t a0 = (a00 * (65536 - fx) + a10 * fx) >> 16;
  uint32_t a1 = (a01 * (65536 - fx) + a11 * fx) >> 16;
  return (uint8_t)((a0 * (65536 - fy) + a1 * fy) >> 16);
}

static void draw_title(UINT32 ms) {
  int total_w = 0;
  for (int i = 0; i < TITLE_LEN; i++)
    total_w +=
        FONT_MAIN.glyphs[(unsigned char)TITLE_STR[i]].advance * FONT_SCALE;
  int x = ((int)g_w - total_w) / 2;
  int y = (int)g_h / 2 - FONT_MAIN.height * FONT_SCALE - 40;
  g_title_x = x - 10; g_title_y = y - 8; g_title_w = total_w + 20;
  g_title_h = FONT_MAIN.height * FONT_SCALE + 16;
  fill_bg_rect(g_title_x, g_title_y, g_title_w, g_title_h);

  for (int i = 0; i < TITLE_LEN; i++) {
    const glyph_aa_t *g = &FONT_MAIN.glyphs[(unsigned char)TITLE_STR[i]];
    UINT64 st = (UINT64)i * LETTER_STAGGER_MS;
    UINT32 la = (ms < st) ? 0
                          : ((ms - st >= LETTER_FADE_MS)
                                 ? 255
                                 : (UINT32)(255 * (ms - st) / LETTER_FADE_MS));
    int rise = (int)((255 - la) * 12 / 255);
    int gw = g->width * FONT_SCALE, gh = g->height * FONT_SCALE;
    int bx = x + g->bearing_x * FONT_SCALE;
    int by = y + (FONT_MAIN.height - g->bearing_y) * FONT_SCALE + rise;
    if (la) {
      for (int dy = 0; dy < gh; dy++) {
        for (int dx = 0; dx < gw; dx++) {
          uint8_t a = bilinear(g, dx, dy, FONT_SCALE);
          if (!a)
            continue;
          uint32_t ea = (a * la) / 255;
          if (!ea)
            continue;
          /* Fondo negro, así que hacemos alpha blending directo */
          put_px(bx + dx, by + dy, blend_rgb(COL_TITLE, read_px(bx + dx, by + dy), ea));
        }
      }
    }
    x += g->advance * FONT_SCALE;
  }
}

static void draw_spinner(UINT32 ms) {
  if (ms < TITLE_TOTAL_MS)
    return;
  UINT32 sp_ms = ms - TITLE_TOTAL_MS;
  int head = (int)((sp_ms / (SPIN_ROT_MS / SPIN_DOTS)) % SPIN_DOTS);

  fill_bg_rect(g_spin_cx - SPIN_PAD, g_spin_cy - SPIN_PAD, 2 * SPIN_PAD,
               2 * SPIN_PAD);

  for (int i = 0; i < SPIN_DOTS; i++) {
    int d = (head - i + SPIN_DOTS) % SPIN_DOTS;
    UINT32 bright = 255 - (UINT32)(d * 230 / SPIN_DOTS);
    if (bright < 25)
      bright = 25;
    UINT32 r = (0x5A * bright) / 255, g = (0xB0 * bright) / 255,
           b = (0xFF * bright) / 255;
    int px = g_spin_cx + k_spin_dx[i], py = g_spin_cy + k_spin_dy[i];

    for (int yy = -SPIN_DOT_R; yy <= SPIN_DOT_R; yy++) {
      for (int xx = -SPIN_DOT_R; xx <= SPIN_DOT_R; xx++) {
        if (xx * xx + yy * yy <= SPIN_DOT_R * SPIN_DOT_R)
          put_px(px + xx, py + yy, (r << 16) | (g << 8) | b);
      }
    }
  }
}

int efi_splash_init(EFI_PHYSICAL_ADDRESS lfb_phys, UINT32 w, UINT32 h,
                    UINT32 pitch, UINT32 bpp) {
  if (!lfb_phys || !w || !h || !pitch || (bpp != 24 && bpp != 32))
    return -1;
  g_fb = (uint8_t *)(UINTN)lfb_phys;
  g_w = w;
  g_h = h;
  g_pitch = pitch;
  g_bpp = bpp;
  g_start = rdtsc_now();
  g_last = 0;
  g_active = 1;

  g_spin_cx = (int)g_w / 2;
  g_spin_cy = (int)g_h / 2 + 60;

  draw_background();
  draw_logo();
  draw_title(0);
  draw_caption();
  g_last = g_start;
  return 0;
}

void efi_splash_tick(void) {
  if (!g_active)
    return;
  UINT64 now = rdtsc_now();
  if (now - g_last < 32000000ULL)
    return; // ~16ms asumiendo 2GHz
  g_last = now;
  UINT32 ms = (UINT32)((now - g_start) / 2000000ULL);

  if (ms < TITLE_TOTAL_MS) {
    draw_title(ms);
  } else {
    draw_spinner(ms);
  }
}

void efi_splash_get_state(UINT32 *phase, UINT32 *elapsed_ms) {
  UINT32 ms = (UINT32)((rdtsc_now() - g_start) / 2000000ULL);
  *elapsed_ms = ms;
  *phase = (ms >= TITLE_TOTAL_MS) ? 2 : 1;
}

void efi_splash_done(void) {
  if (!g_active)
    return;
  g_active = 0;
}