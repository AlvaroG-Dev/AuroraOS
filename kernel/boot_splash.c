/* kernel/boot_splash.c  (reemplaza al actual)
 * Usa el MISMO núcleo gráfico que los bootloaders: continuidad total.
 * Sin buffer sombra (ahorra 8 MB a 1080p) y sin dependencia de fuentes.
 */
#include "boot_splash.h"
#include "../bootloader/common/aurora_splash_core.h"
#include "heap.h"
#include <stddef.h>
#include <stdint.h>

extern void fb_flush_rect(const uint32_t *src, uint32_t src_pitch, uint32_t x,
                          uint32_t y, uint32_t w, uint32_t h);
extern void sched_yield(void);
extern void kfree(void *p); /* ajusta el nombre si tu heap lo llama distinto */

int g_boot_splash_enabled = 0;
uint32_t g_boot_splash_initial_phase = 1; /* compat; el estado real es el ms */
uint32_t g_boot_splash_initial_ms = 0;

/* Tiempos mínimos: lo justo para que el título termine de aparecer. */
#ifndef MIN_SPLASH_MS
#define MIN_SPLASH_MS (ASP_TITLE_END + 150)
#endif
#define FRAME_MS 16
#define FADE_MS 120
#define WIPE_MS 300
#define COL_DESKTOP 0xFF101828u
#define TSC_HZ_DEFAULT 3000000000ULL

static asp_t S;
static uint32_t g_line[ASP_MAX_W];
static uint32_t *g_fb;
static uint32_t g_w, g_h, g_pitch;
static int g_phase, g_active;
static uint64_t g_tsc_hz = TSC_HZ_DEFAULT, g_start, g_last, g_reveal_start;
static volatile int g_lock; /* splash task y kmain pueden pintar a la vez */
static int32_t g_prev_b;
static uint32_t *g_desktop_bm;

static inline uint64_t rdtsc_now(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t ms_to_ticks(uint64_t ms) {
  return ms * (g_tsc_hz / 1000ULL);
}
static inline uint64_t elapsed_ms(uint64_t now, uint64_t then) {
  return (now - then) / (g_tsc_hz / 1000ULL);
}

static void put_row(void *ctx, int x, int y, int w, const uint32_t *px) {
  (void)ctx;
  uint32_t *d = g_fb + (uint64_t)y * g_pitch + x;
  for (int i = 0; i < w; i++)
    d[i] = px[i];
  fb_flush_rect(g_fb, g_pitch, (uint32_t)x, (uint32_t)y, (uint32_t)w, 1);
}

void boot_splash_tick(void) {
  if (!g_active || g_phase != 1)
    return;
  uint64_t now = rdtsc_now();
  if (g_last && (now - g_last) < ms_to_ticks(FRAME_MS))
    return;
  if (__sync_lock_test_and_set(&g_lock, 1))
    return;
  g_last = now;
  asp_frame(&S, (uint32_t)elapsed_ms(now, g_start), put_row, 0, g_line);
  __sync_lock_release(&g_lock);
}

void boot_splash_task_loop(void) {
  while (g_active && g_phase == 1) {
    boot_splash_tick();
#ifdef BOOT_SPLASH_HLT
    /* Opcional: duerme hasta la próxima IRQ de timer (requiere IF=1). */
    __asm__ volatile("sti; hlt");
#else
    __asm__ volatile("pause");
#endif
    sched_yield();
  }
}

void boot_splash_begin_reveal(boot_splash_render_fn_t render_below) {
  if (!g_active || g_phase != 1)
    return;
  while (elapsed_ms(rdtsc_now(), g_start) < MIN_SPLASH_MS) {
    boot_splash_tick();
    __asm__ volatile("pause");
  }
  if (render_below) {
    g_desktop_bm =
        (uint32_t *)kmalloc((size_t)g_pitch * g_h * sizeof(uint32_t));
    if (g_desktop_bm)
      render_below(g_desktop_bm, g_w, g_h, g_pitch); /* directo, sin copias */
  }
  while (__sync_lock_test_and_set(&g_lock, 1))
    __asm__ volatile("pause");
  g_prev_b = (int32_t)g_h;
  g_last = 0;
  g_reveal_start = rdtsc_now();
  g_phase = 3;
  g_active = 0;
  __sync_lock_release(&g_lock);
}

/* 1) fade-out del título+spinner  2) barrido suave con borde difuminado. */
int boot_splash_tick_reveal(void) {
  if (g_phase != 3)
    return 0;
  uint64_t now = rdtsc_now();
  if (g_last && (now - g_last) < ms_to_ticks(FRAME_MS))
    return 1;
  g_last = now;
  uint32_t rms = (uint32_t)elapsed_ms(now, g_reveal_start);

  if (rms < FADE_MS) {
    uint32_t master = 255 - asp_smooth(rms * 255 / FADE_MS);
    uint32_t ms = (uint32_t)elapsed_ms(now, g_start);
    asp_draw_title(&S, ms, master, put_row, 0, g_line);
    asp_draw_spinner(&S, ms, master, put_row, 0, g_line);
    return 1;
  }

  uint32_t wms = rms - FADE_MS;
  int done = wms >= WIPE_MS;
  uint64_t t16 = done ? 65536 : ((uint64_t)wms * 65536) / WIPE_MS;
  uint64_t e =
      (t16 * t16 / 65536) * (3 * 65536 - 2 * t16) / 65536; /* smoothstep */
  int32_t edge = (int32_t)g_h / 16;
  if (edge < 16)
    edge = 16;
  int32_t b = (int32_t)g_h - (int32_t)((e * (uint64_t)(g_h + edge)) / 65536);
  int32_t ys = b < 0 ? 0 : b;
  int32_t ye = g_prev_b + edge;
  if (ye > (int32_t)g_h)
    ye = (int32_t)g_h;

  for (int32_t y = ys; y < ye; y++) {
    asp_row_t rw;
    asp_row_begin(&S, y, &rw);
    uint32_t alpha = y >= b + edge ? 255u : (uint32_t)((y - b) * 255 / edge);
    const uint32_t *drow =
        g_desktop_bm ? g_desktop_bm + (uint64_t)y * g_pitch : 0;
    for (uint32_t x0 = 0; x0 < g_w; x0 += ASP_MAX_W) {
      uint32_t n = g_w - x0 > ASP_MAX_W ? ASP_MAX_W : g_w - x0;
      for (uint32_t x = 0; x < n; x++) {
        uint32_t d = drow ? drow[x0 + x] : COL_DESKTOP;
        g_line[x] =
            alpha >= 255
                ? d
                : asp_mix(d, asp_bg_px(&S, &rw, (int)(x0 + x), y), alpha);
      }
      put_row(0, (int)x0, y, (int)n, g_line);
    }
  }
  g_prev_b = b;
  if (done) {
    g_phase = 4;
    if (g_desktop_bm) {
      kfree(g_desktop_bm);
      g_desktop_bm = NULL;
    }
    return 0;
  }
  return 1;
}

int boot_splash_init(uint32_t *fb, uint32_t width, uint32_t height,
                     uint32_t pitch_px) {
  if (!fb || !width || !height || pitch_px < width ||
      (size_t)pitch_px > (size_t)-1 / height / sizeof(uint32_t))
    return -1;
  g_fb = fb;
  g_w = width;
  g_h = height;
  g_pitch = pitch_px;
  asp_layout(&S, width, height);
  S.mask = (uint8_t *)kmalloc((size_t)S.mask_w * (size_t)S.mask_h);
  if (!S.mask)
    return -1;
  asp_render_mask(&S);

  /* Retomamos el tiempo donde lo dejó el bootloader. Como el fondo es
   * analítico, repintarlo es idéntico a lo que ya hay en pantalla. */
  g_start = rdtsc_now() - ms_to_ticks(g_boot_splash_initial_ms);
  g_last = 0;
  g_phase = 1;
#ifdef BOOT_SPLASH_KEEP_BG
  /* LFB directo de 32 bpp: el bootloader ya pintó el fondo, no lo repintamos.
   */
  if (!g_boot_splash_initial_ms)
#endif
    asp_fill_bg(&S, put_row, 0, g_line);
  asp_frame(&S, g_boot_splash_initial_ms, put_row, 0, g_line);
  g_active = 1;
  return 0;
}

int boot_splash_active(void) { return g_active; }

/* Reancla el tiempo transcurrido al re-calibrar la frecuencia del TSC. */
void boot_splash_set_tsc_hz(uint64_t hz) {
  if (!hz)
    return;
  uint64_t now = rdtsc_now();
  uint64_t ms = elapsed_ms(now, g_start);
  g_tsc_hz = hz;
  if (g_start)
    g_start = now - ms_to_ticks(ms);
}

void boot_splash_finish(void) {
  g_phase = 4;
  g_active = 0;
  if (g_desktop_bm) {
    kfree(g_desktop_bm);
    g_desktop_bm = NULL;
  }
  if (S.mask) {
    kfree(S.mask);
    S.mask = NULL;
  }
}