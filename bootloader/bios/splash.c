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
  for (int yy = 0; yy < h; yy++)
    for (int xx = 0; xx < w; xx++)
      put_px(x + xx, y + yy, rgb);
}

/* 12 puntos precalculados, R = 26 */
static const int16_t k_dx[12] = {26,  22,  13,  0, -13, -22,
                                 -26, -22, -13, 0, 13,  22};
static const int16_t k_dy[12] = {0, 13,  22,  26,  22,  13,
                                 0, -13, -22, -26, -22, -13};

static void draw_frame(uint32_t ms) {
  int cx = (int)g_w / 2;
  int cy = (int)g_h / 2;
  int R = 26 + 5 + 4; /* pad */
  const int dot_r = 4;
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
  /* ~2 GHz asumido: 16 ms ≈ 32 M ciclos. Aproximación sin TSC calibrado. */
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