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
#define FONT_SCALE g_font_scale
static int g_font_scale = 4;
static int g_title_y = 0;
#define LETTER_FADE_MS 210
#define LETTER_STAGGER_MS 105
#define TITLE_TOTAL_MS (LETTER_STAGGER_MS * (TITLE_LEN - 1) + LETTER_FADE_MS)

#define SPIN_R 18 /* Spinner ajustado */
#define SPIN_DOT_R 3
#define SPIN_DOTS 12
#define SPIN_ROT_MS 1000
#define SPIN_PAD (SPIN_R + SPIN_DOT_R + 8)

static uint8_t *g_fb = NULL;
static UINT32 g_w, g_h, g_pitch, g_bpp;
static UINT64 g_start = 0, g_last = 0;
static UINT64 g_tsc_hz = 2000000000ULL;
static int g_active = 0;
static int g_spin_cx, g_spin_cy;

/* 12 puntos precalculados, R=18 */
static const int16_t k_spin_dx[12] = {18,  16,  9,  0, -9, -16,
                                      -18, -16, -9, 0, 9,  16};
static const int16_t k_spin_dy[12] = {0, 9,  16,  18,  16,  9,
                                      0, -9, -16, -18, -16, -9};

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
  if (!g_fb || w <= 0 || h <= 0)
    return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x >= (int)g_w || y >= (int)g_h || w <= 0 || h <= 0)
    return;
  if (w > (int)g_w - x) w = (int)g_w - x;
  if (h > (int)g_h - y) h = (int)g_h - y;
  if (g_bpp == 32) {
    for (int yy = 0; yy < h; yy++) {
      UINT32 *row = (UINT32 *)(g_fb + (UINT64)(y + yy) * g_pitch) + x;
      for (int xx = 0; xx < w; xx++) row[xx] = rgb;
    }
  } else if (g_bpp == 24) {
    for (int yy = 0; yy < h; yy++) {
      UINT8 *row = g_fb + (UINT64)(y + yy) * g_pitch + (UINT64)x * 3;
      for (int xx = 0; xx < w; xx++) {
        row[xx * 3 + 0] = rgb & 0xFF;
        row[xx * 3 + 1] = (rgb >> 8) & 0xFF;
        row[xx * 3 + 2] = (rgb >> 16) & 0xFF;
      }
    }
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
  int y = g_title_y;
  fill_rect(x - 4, y - 4, total_w + 8, FONT_MAIN.height * FONT_SCALE + 12, 0x000000);

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
          UINT32 r = (0xE8 * ea) / 255, g_c = (0xF0 * ea) / 255,
                 b = (0xFF * ea) / 255;
          put_px(bx + dx, by + dy, (r << 16) | (g_c << 8) | b);
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

  fill_rect(g_spin_cx - SPIN_PAD, g_spin_cy - SPIN_PAD, 2 * SPIN_PAD,
            2 * SPIN_PAD, 0x000000);

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
  /* Calibrate TSC against the UEFI timer instead of assuming a 2 GHz CPU. */
  UINT64 cal_start = rdtsc_now();
  gBS->Stall(10000);
  UINT64 cal_delta = rdtsc_now() - cal_start;
  if (cal_delta >= 1000)
    g_tsc_hz = cal_delta * 100ULL;
  g_start = rdtsc_now();
  g_last = 0;
  g_active = 1;

  g_font_scale = 4;
  int natural_w = 0;
  for (int i = 0; i < TITLE_LEN; i++)
    natural_w += FONT_MAIN.glyphs[(unsigned char)TITLE_STR[i]].advance;
  while (g_font_scale > 1 &&
         ((UINT64)natural_w * g_font_scale > (UINT64)g_w * 9 / 10 ||
          (UINT64)FONT_MAIN.height * g_font_scale + 100 > g_h))
    g_font_scale--;
  int title_h = FONT_MAIN.height * FONT_SCALE;
  int spin_pad = SPIN_PAD;
  int layout_h = title_h + 20 + 2 * spin_pad;
  g_title_y = ((int)g_h - layout_h) / 2;
  g_spin_cx = (int)g_w / 2;
  g_spin_cy = g_title_y + title_h + 20 + spin_pad;

  fill_rect(0, 0, (int)w, (int)h, 0x000000);
  draw_title(0);
  g_last = g_start;
  return 0;
}

void efi_splash_tick(void) {
  if (!g_active)
    return;
  UINT64 now = rdtsc_now();
  if (now - g_last < g_tsc_hz / 60ULL)
    return;
  g_last = now;
  UINT32 ms = (UINT32)((now - g_start) / (g_tsc_hz / 1000ULL));

  if (ms < TITLE_TOTAL_MS) {
    draw_title(ms);
  } else {
    draw_spinner(ms);
  }
}

void efi_splash_get_state(UINT32 *phase, UINT32 *elapsed_ms) {
  if (!phase || !elapsed_ms)
    return;
  UINT32 ms = (UINT32)((rdtsc_now() - g_start) / (g_tsc_hz / 1000ULL));
  *elapsed_ms = ms;
  *phase = (ms >= TITLE_TOTAL_MS) ? 2 : 1;
}

void efi_splash_done(void) {
  if (!g_active)
    return;
  g_active = 0;
}