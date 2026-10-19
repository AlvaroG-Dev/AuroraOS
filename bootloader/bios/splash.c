/* bootloader/bios/splash.c
 *
 * Splash mínimo del bootloader BIOS: spinner blanco-azulado sobre
 * fondo negro. No depende de la fuente del kernel. Corre en PM32.
 */
#include "splash.h"

extern void bios_log(const char *fmt, ...);

static uint8_t *g_fb = (uint8_t *)0;
static uint32_t g_w, g_h, g_pitch, g_bpp;
static uint32_t g_start_tsc = 0;
static uint32_t g_last_draw = 0;
static int g_active = 0;

static inline uint32_t rdtsc_lo(void) {
  uint32_t lo;
  __asm__ volatile("rdtsc" : "=a"(lo) : : "edx");
  return lo;
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
  if (w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > (int)g_w) w = (int)g_w - x;
  if (y + h > (int)g_h) h = (int)g_h - y;
  if (w <= 0 || h <= 0) return;
  for (int yy = y; yy < y + h; yy++) {
    if (g_bpp == 32) {
      uint32_t *row = (uint32_t *)(g_fb + (uint64_t)yy * g_pitch + (uint64_t)x * 4);
      for (int xx = 0; xx < w; xx++) row[xx] = rgb;
    } else {
      uint8_t *row = g_fb + (uint64_t)yy * g_pitch + (uint64_t)x * 3;
      uint8_t b = (uint8_t)rgb, g = (uint8_t)(rgb >> 8), r = (uint8_t)(rgb >> 16);
      for (int xx = 0; xx < w; xx++) { row[xx*3] = b; row[xx*3+1] = g; row[xx*3+2] = r; }
    }
  }
}
static uint32_t bg_color(uint32_t y) {
  uint32_t t = (g_h > 1) ? (y * 255u) / (g_h - 1u) : 0;
  uint32_t r = 6u + (5u * t) / 255u;
  uint32_t g = 10u + (10u * t) / 255u;
  uint32_t b = 19u + (19u * t) / 255u;
  return (r << 16) | (g << 8) | b;
}
static void fill_bg_rect(int x, int y, int w, int h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > (int)g_w) w = (int)g_w - x;
  if (y + h > (int)g_h) h = (int)g_h - y;
  if (w <= 0 || h <= 0) return;
  for (int yy = y; yy < y + h; yy++) fill_rect(x, yy, w, 1, bg_color((uint32_t)yy));
}
static void draw_background(void) {
  for (uint32_t y = 0; y < g_h; y++) fill_rect(0, (int)y, (int)g_w, 1, bg_color(y));
}
static void draw_segment(int x0, int y0, int x1, int y1, uint32_t color) {
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
  int cx = (int)g_w / 2, cy = (int)g_h / 2 - 55;
  if (cy < 36) cy = 36;
  draw_segment(cx - 24, cy + 14, cx - 8, cy - 16, 0x3B82F6u);
  draw_segment(cx - 8, cy - 16, cx + 8, cy + 14, 0x60D5FFu);
  draw_segment(cx + 2, cy + 14, cx + 18, cy - 16, 0x8B9DFFu);
  draw_segment(cx + 18, cy - 16, cx + 28, cy + 4, 0x3B82F6u);
}

/* 12 puntos precalculados, R = 26 */
static const int16_t k_dx[12] = {26,  22,  13,  0, -13, -22,
                                 -26, -22, -13, 0, 13,  22};
static const int16_t k_dy[12] = {0, 13,  22,  26,  22,  13,
                                 0, -13, -22, -26, -22, -13};

static void draw_frame(uint32_t ms) {
  int cx = (int)g_w / 2;
  int cy = (int)g_h / 2 + 60;
  int R = 26 + 5 + 4; /* pad */
  const int dot_r = 4;
  const int dots = 12;
  const uint32_t rot_ms = 1100;

  /* caja de fondo */
  fill_bg_rect(cx - R, cy - R, 2 * R, 2 * R);

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
  g_start_tsc = rdtsc_lo();
  g_last_draw = 0;
  g_active = 1;

  /* Fondo degradado y marca vectorial; solo se renderiza una vez. */
  draw_background();
  draw_logo();
  draw_frame(0);
  g_last_draw = g_start_tsc;
  return 0;
}

void bios_splash_tick(void) {
  if (!g_active)
    return;
  uint32_t now = rdtsc_lo();
  /* ~2 GHz asumido: 16 ms ≈ 32 M ciclos. Aproximación sin TSC calibrado. */
  if (now - g_last_draw < 32000000u)
    return;
  g_last_draw = now;
  uint32_t ms = (now - g_start_tsc) / 2000000u;
  draw_frame(ms);
}

void bios_splash_done(void) {
  if (!g_active)
    return;
  g_active = 0;
}