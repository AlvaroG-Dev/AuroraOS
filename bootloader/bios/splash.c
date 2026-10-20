/* bootloader/bios/splash.c  (reemplaza al actual) — PM32, sin libgcc. */
#include "splash.h"
#include "../common/aurora_splash_core.h"

extern uint32_t bump_alloc_pages(uint32_t npages);

static asp_t S;
static uint32_t g_line[ASP_MAX_W];
static uint8_t *g_fb;
static uint32_t g_pitch, g_bpp;
static uint32_t g_per_ms =
    2000000; /* ticks TSC por ms (se calibra con el PIT) */
static uint32_t g_last_lo, g_last_hi, g_rem, g_ms;
static int g_active;

static inline void rdtsc2(uint32_t *lo, uint32_t *hi) {
  __asm__ volatile("rdtsc" : "=a"(*lo), "=d"(*hi));
}
static inline uint8_t pio_in(uint16_t p) {
  uint8_t v;
  __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
  return v;
}
static inline void pio_out(uint16_t p, uint8_t v) {
  __asm__ volatile("outb %0,%1" : : "a"(v), "Nd"(p));
}

/* Calibra el TSC con el canal 2 del PIT (5 ms). Con timeout por TSC. */
static void calibrate_tsc(void) {
  pio_out(0x61, (uint8_t)((pio_in(0x61) & ~0x02) | 0x01));
  pio_out(0x43, 0xB0);
  pio_out(0x42, 5966 & 0xFF);
  pio_out(0x42, 5966 >> 8);
  uint32_t lo0, hi0, lo1, hi1;
  rdtsc2(&lo0, &hi0);
  for (;;) {
    if (pio_in(0x61) & 0x20)
      break;
    rdtsc2(&lo1, &hi1);
    if ((uint32_t)(lo1 - lo0) > 0x40000000u)
      return; /* sin PIT: nos quedamos con el valor por defecto */
  }
  rdtsc2(&lo1, &hi1);
  uint32_t delta = lo1 - lo0;
  if (delta >= 50000u)
    g_per_ms = delta / 5u;
}

static void put_row(void *ctx, int x, int y, int w, const uint32_t *px) {
  (void)ctx;
  if (g_bpp == 32) {
    uint32_t *d = (uint32_t *)(g_fb + (uint32_t)y * g_pitch) + x;
    for (int i = 0; i < w; i++)
      d[i] = px[i];
  } else {
    uint8_t *d = g_fb + (uint32_t)y * g_pitch + (uint32_t)x * 3;
    for (int i = 0; i < w; i++) {
      d[i * 3 + 0] = (uint8_t)px[i];
      d[i * 3 + 1] = (uint8_t)(px[i] >> 8);
      d[i * 3 + 2] = (uint8_t)(px[i] >> 16);
    }
  }
}

/* Avanza g_ms con el TSC de 64 bits sin usar divisiones de 64 bits. */
static uint32_t advance(void) {
  uint32_t lo, hi;
  rdtsc2(&lo, &hi);
  uint64_t d =
      (((uint64_t)hi << 32) | lo) - (((uint64_t)g_last_hi << 32) | g_last_lo);
  uint32_t dlo = (uint32_t)d, dhi = (uint32_t)(d >> 32);
  g_last_lo = lo;
  g_last_hi = hi;
  /* acumulamos el resto en 64 bits: (dhi:dlo + g_rem) / per_ms */
  uint32_t t = dlo + g_rem;
  if (t < dlo)
    dhi++;
  uint32_t q, r;
  if (dhi >= g_per_ms) {
    q = 0xFFFFFFFFu;
    r = 0;
  } else {
    __asm__("divl %4" : "=a"(q), "=d"(r) : "a"(t), "d"(dhi), "r"(g_per_ms));
  }
  g_rem = r;
  g_ms += q;
  return g_ms;
}

int bios_splash_init(uint32_t lfb_phys, uint32_t w, uint32_t h, uint32_t pitch,
                     uint32_t bpp) {
  if (!lfb_phys || !w || !h || !pitch || (bpp != 24 && bpp != 32))
    return -1;
  g_fb = (uint8_t *)(uintptr_t)lfb_phys;
  g_pitch = pitch;
  g_bpp = bpp;
  calibrate_tsc();

  asp_layout(&S, w, h);
  uint32_t bytes = (uint32_t)S.mask_w * (uint32_t)S.mask_h;
  uint32_t phys = bump_alloc_pages((bytes + 0xFFFu) / 0x1000u);
  if (!phys)
    return -1;
  S.mask = (uint8_t *)(uintptr_t)phys;
  asp_render_mask(&S);

  asp_fill_bg(&S, put_row, 0, g_line);
  rdtsc2(&g_last_lo, &g_last_hi);
  g_rem = 0;
  g_ms = 0;
  g_active = 1;
  asp_frame(&S, 0, put_row, 0, g_line);
  return 0;
}

static uint32_t g_drawn_ms;

void bios_splash_tick(void) {
  if (!g_active)
    return;
  uint32_t ms = advance();
  if (ms - g_drawn_ms < 16u)
    return;
  g_drawn_ms = ms;
  asp_frame(&S, ms, put_row, 0, g_line);
}

uint32_t bios_splash_elapsed_ms(void) { return advance(); }

void bios_splash_get_state(uint32_t *phase, uint32_t *elapsed_ms) {
  uint32_t ms = advance();
  if (elapsed_ms)
    *elapsed_ms = ms;
  if (phase)
    *phase = (ms >= ASP_TITLE_END) ? 2 : 1;
}

/* Anima hasta min_ms desde el inicio y cierra. */
void bios_splash_wait(uint32_t min_ms) {
  while (g_active && advance() < min_ms)
    bios_splash_tick();
  g_active = 0;
}

void bios_splash_done(void) { g_active = 0; }