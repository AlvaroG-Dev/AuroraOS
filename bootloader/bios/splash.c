/* bootloader/bios/splash.c
 *
 * Splash mínimo del bootloader BIOS: spinner blanco-azulado sobre
 * fondo negro. No depende de la fuente del kernel. Corre en PM32.
 */
#include "splash.h"

extern void bios_log(const char *fmt, ...);

static uint8_t *g_fb = (uint8_t *)0;
static uint32_t g_w, g_h, g_pitch, g_bpp;
static uint64_t g_start_tsc = 0;
static uint64_t g_last_draw = 0;
static int g_active = 0;

static inline uint64_t rdtsc_now(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

/* Escribe un pixel en la posición (x, y). Soporta 24 y 32 bpp. */
static void put_px(int x, int y, uint32_t rgb) {
  if (x < 0 || y < 0 || x >= (int)g_w || y >= (int)g_h)
    return;
  if (g_bpp == 32) {
    uint32_t *p = (uint32_t *)(g_fb + (uint64_t)y * g_pitch + (uint64_t)x * 4);
    *p = rgb;
  } else if (g_bpp == 24) {
    uint8_t *p = g_fb + (uint64_t)y * g_pitch + (uint64_t)x * 3;
    p[0] = rgb & 0xFF;
    p[1] = (rgb >> 8) & 0xFF;
    p[2] = (rgb >> 16) & 0xFF;
  }
}

static void fill_rect(int x, int y, int w, int h, uint32_t rgb) {
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
      uint32_t *row = (uint32_t *)(g_fb + (uint64_t)(y + yy) * g_pitch) + x;
      for (int xx = 0; xx < w; xx++) row[xx] = rgb;
    }
  } else if (g_bpp == 24) {
    for (int yy = 0; yy < h; yy++) {
      uint8_t *row = g_fb + (uint64_t)(y + yy) * g_pitch + (uint64_t)x * 3;
      for (int xx = 0; xx < w; xx++) {
        row[xx * 3 + 0] = rgb & 0xFF;
        row[xx * 3 + 1] = (rgb >> 8) & 0xFF;
        row[xx * 3 + 2] = (rgb >> 16) & 0xFF;
      }
    }
  }
}

/* Minimal 12-dot spinner, radius 18. */
static const int16_t k_dx[12] = {18, 16, 9, 0, -9, -16, -18, -16, -9, 0, 9, 16};
static const int16_t k_dy[12] = {0, 9, 16, 18, 16, 9, 0, -9, -16, -18, -16, -9};

static void draw_frame(uint32_t ms) {
  int cx = (int)g_w / 2;
  int cy = (int)g_h / 2;
  const int dot_r = 2;
  int R = 18 + dot_r + 4; /* clipped dirty rectangle */
  const int dots = 12;
  const uint32_t rot_ms = 1100;

  /* caja de fondo */
  fill_rect(cx - R, cy - R, 2 * R, 2 * R, 0x000000);

  int head = (int)((ms / (rot_ms / dots)) % dots);
  for (int i = 0; i < dots; i++) {
    int d = (head - i + dots) % dots;
    uint32_t bright = 255 - (uint32_t)(d * 230 / dots);
    if (bright < 25)
      bright = 25;
    /* color azulado modulado por brightness */
    uint32_t r = (uint32_t)(0x5A * bright / 255);
    uint32_t g = (uint32_t)(0xB0 * bright / 255);
    uint32_t b = (uint32_t)(0xFF * bright / 255);
    uint32_t rgb = (r << 16) | (g << 8) | b;
    int px = cx + k_dx[i];
    int py = cy + k_dy[i];
    /* disco duro sin AA (es solo un spinner rápido) */
    for (int yy = -dot_r; yy <= dot_r; yy++)
      for (int xx = -dot_r; xx <= dot_r; xx++)
        if (xx * xx + yy * yy <= dot_r * dot_r)
          put_px(px + xx, py + yy, rgb);
  }
}

int bios_splash_init(uint32_t lfb_phys, uint32_t w, uint32_t h, uint32_t pitch,
                     uint32_t bpp) {
  if (!lfb_phys || !w || !h || !pitch || (bpp != 24 && bpp != 32))
    return -1;
  g_fb = (uint8_t *)(uintptr_t)lfb_phys;
  g_w = w;
  g_h = h;
  g_pitch = pitch;
  g_bpp = bpp;
  g_start_tsc = rdtsc_now();
  g_last_draw = 0;
  g_active = 1;

  /* Fondo negro + primer frame */
  fill_rect(0, 0, (int)w, (int)h, 0x000000);
  draw_frame(0);
  g_last_draw = g_start_tsc;
  return 0;
}

void bios_splash_tick(void) {
  if (!g_active)
    return;
  uint64_t now = rdtsc_now();
  /* 60 Hz throttle; 64-bit TSC avoids the ~2.1 s 32-bit wraparound. */
  if (now - g_last_draw < 32000000ULL)
    return;
  g_last_draw = now;
  uint32_t ms = (uint32_t)((now - g_start_tsc) / 2000000ULL);
  draw_frame(ms);
}

void bios_splash_done(void) {
  if (!g_active)
    return;
  g_active = 0;
}