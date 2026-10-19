/* kernel/boot_splash.c */
#include "boot_splash.h"
#include "gfx/font_aa.h"
#include "gfx/font_manager.h"
#include "heap.h"
#include <stddef.h>
#include <stdint.h>

extern void fb_flush_rect(const uint32_t *src, uint32_t src_pitch, uint32_t x,
                          uint32_t y, uint32_t w, uint32_t h);
extern void sched_yield(void);

int g_boot_splash_enabled = 1;
uint32_t g_boot_splash_initial_phase = 1;
uint32_t g_boot_splash_initial_ms = 0;

#define TITLE_STR "AuroraOS"
#define TITLE_LEN 8
#define FONT_SCALE g_font_scale
static int g_font_scale = 4;
#define LETTER_FADE_MS 210
#define LETTER_STAGGER_MS 105
#define TITLE_TOTAL_MS (LETTER_STAGGER_MS * (TITLE_LEN - 1) + LETTER_FADE_MS)

#define SPIN_R 18
#define SPIN_DOT_R 3
#define SPIN_DOTS 12
#define SPIN_ROT_MS 1000
#define SPIN_PAD (SPIN_R + SPIN_DOT_R + 8)

#define MIN_SPLASH_MS 2200
#define REVEAL_MS 560
#define TSC_HZ_DEFAULT 3000000000ULL

#define COL_BG 0xFF000000u
#define COL_TITLE 0xFFE8F0FFu
#define COL_SPINNER 0xFF5AB0FFu
#define COL_DESKTOP 0xFF101828u

static const int16_t k_spin_dx[12] = {18,  16,  9,  0, -9, -16,
                                      -18, -16, -9, 0, 9,  16};
static const int16_t k_spin_dy[12] = {0, 9,  16,  18,  16,  9,
                                      0, -9, -16, -18, -16, -9};

static uint32_t *g_fb = NULL;
static uint32_t g_w = 0, g_h = 0, g_pitch = 0;
static uint32_t *g_shadow = NULL;
static int g_phase = 0, g_active = 0;
static const font_aa_t *g_font = NULL;

static uint32_t *g_title_bm = NULL;
static int g_tb_w = 0, g_tb_h = 0;
static int g_title_x = 0, g_title_y = 0;
static int g_letter_x[TITLE_LEN];
static int g_letter_pad = 24;

static int g_spin_cx = 0, g_spin_cy = 0;
static uint64_t g_tsc_hz = TSC_HZ_DEFAULT;
static uint64_t g_start = 0, g_last_tick = 0;

static uint64_t g_reveal_start = 0;
static uint32_t g_prev_offset = 0;
static uint32_t *g_desktop_bm = NULL;
static boot_splash_render_fn_t g_render_below = NULL;

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

static inline uint32_t blend_px(uint32_t src, uint32_t dst) {
  uint32_t a = src >> 24;
  if (a == 0)
    return dst;
  if (a == 255)
    return src;
  uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
  uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
  uint32_t ia = 255 - a;
  return 0xFF000000u | ((((sr * a) + (dr * ia)) / 255) << 16) |
         ((((sg * a) + (dg * ia)) / 255) << 8) | (((sb * a) + (db * ia)) / 255);
}

static void sh_fill(int x, int y, int w, int h, uint32_t c) {
  if (!g_shadow || w <= 0 || h <= 0)
    return;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > (int)g_w)
    w = g_w - x;
  if (x >= (int)g_w || y >= (int)g_h || w <= 0 || h <= 0)
    return;
  if (y + h > (int)g_h)
    h = g_h - y;
  if (w <= 0 || h <= 0)
    return;
  for (int yy = 0; yy < h; yy++) {
    uint32_t *row = g_shadow + (uint64_t)(y + yy) * g_pitch + x;
    for (int xx = 0; xx < w; xx++)
      row[xx] = c;
  }
}

static void sh_blit(int x, int y, int w, int h) {
  if (!g_fb || !g_shadow)
    return;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > (int)g_w)
    w = g_w - x;
  if (y + h > (int)g_h)
    h = g_h - y;
  if (w <= 0 || h <= 0)
    return;
  for (int yy = 0; yy < h; yy++) {
    uint32_t *dst = g_fb + (uint64_t)(y + yy) * g_pitch + x;
    uint32_t *src = g_shadow + (uint64_t)(y + yy) * g_pitch + x;
    for (int xx = 0; xx < w; xx++)
      dst[xx] = src[xx];
  }
  fb_flush_rect(g_fb, g_pitch, (uint32_t)x, (uint32_t)y, (uint32_t)w,
                (uint32_t)h);
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

static void prerender_title(void) {
  if (!g_font)
    return;
  int natural_w = 0;
  for (int i = 0; i < TITLE_LEN; i++)
    natural_w += g_font->glyphs[(unsigned char)TITLE_STR[i]].advance;
  g_font_scale = 4;
  while (g_font_scale > 1 &&
         ((uint64_t)natural_w * g_font_scale + 12 * g_font_scale >
              (uint64_t)g_w * 9 / 10 ||
          (uint64_t)g_font->height * g_font_scale + 120 > g_h))
    g_font_scale--;
  g_letter_pad = 6 * g_font_scale;
  int total_w = 0, max_h = 0;
  for (int i = 0; i < TITLE_LEN; i++) {
    const glyph_aa_t *g = &g_font->glyphs[(unsigned char)TITLE_STR[i]];
    g_letter_x[i] = total_w;
    total_w += g->advance * FONT_SCALE;
    int lh = g->height * FONT_SCALE;
    if (lh > max_h)
      max_h = lh;
  }
  g_tb_w = total_w + 2 * g_letter_pad;
  g_tb_h = max_h + 2 * g_letter_pad;
  g_title_bm = (uint32_t *)kmalloc((size_t)g_tb_w * g_tb_h * 4);
  if (!g_title_bm)
    return;
  for (int i = 0; i < g_tb_w * g_tb_h; i++)
    g_title_bm[i] = 0;

  for (int i = 0; i < TITLE_LEN; i++) {
    const glyph_aa_t *g = &g_font->glyphs[(unsigned char)TITLE_STR[i]];
    int gw = g->width * FONT_SCALE, gh = g->height * FONT_SCALE;
    int bx = g_letter_x[i] + g->bearing_x * FONT_SCALE + g_letter_pad;
    int by = (g_font->height - g->bearing_y) * FONT_SCALE + g_letter_pad;
    for (int dy = 0; dy < gh; dy++) {
      int py = by + dy;
      if (py < 0 || py >= g_tb_h)
        continue;
      for (int dx = 0; dx < gw; dx++) {
        int px = bx + dx;
        if (px < 0 || px >= g_tb_w)
          continue;
        uint8_t a = bilinear(g, dx, dy, FONT_SCALE);
        if (!a)
          continue;
        g_title_bm[py * g_tb_w + px] =
            ((uint32_t)a << 24) | (COL_TITLE & 0xFFFFFFu);
      }
    }
  }
  g_title_x = ((int)g_w - total_w) / 2 - g_letter_pad;
  int layout_h = g_tb_h + 18 + 2 * SPIN_PAD;
  g_title_y = ((int)g_h - layout_h) / 2;
}

static void draw_title_frame(uint64_t ms) {
  if (!g_shadow || !g_title_bm || !g_font)
    return;
  sh_fill(g_title_x, g_title_y, g_tb_w, g_tb_h, COL_BG);
  for (int i = 0; i < TITLE_LEN; i++) {
    uint64_t start_ms = (uint64_t)i * LETTER_STAGGER_MS;
    uint8_t la = (ms < start_ms)
                     ? 0
                     : ((ms - start_ms >= LETTER_FADE_MS)
                            ? 255
                            : (uint8_t)(255 * (ms - start_ms) / LETTER_FADE_MS));
    if (!la)
      continue;
    int rise = ((255 - la) * 5) / 255;
    const glyph_aa_t *g = &g_font->glyphs[(unsigned char)TITLE_STR[i]];
    int gw = g->width * FONT_SCALE, gh = g->height * FONT_SCALE;
    int bx = g_letter_x[i] + g->bearing_x * FONT_SCALE + g_letter_pad;
    int by = (g_font->height - g->bearing_y) * FONT_SCALE +
             g_letter_pad + rise;
    for (int dy = 0; dy < gh; dy++) {
      int py = by + dy;
      int sy = g_title_y + py;
      if (py < 0 || py >= g_tb_h || sy < 0 || sy >= (int)g_h)
        continue;
      uint32_t *dst_row = g_shadow + (uint64_t)sy * g_pitch;
      for (int dx = 0; dx < gw; dx++) {
        int px = bx + dx;
        if (px < 0 || px >= g_tb_w)
          continue;
        int sx = g_title_x + px;
        if (sx < 0 || sx >= (int)g_w)
          continue;
        uint32_t p = g_title_bm[py * g_tb_w + px];
        uint32_t a = p >> 24;
        if (!a)
          continue;
        uint32_t ea = (a * la) / 255;
        if (ea)
          dst_row[sx] = blend_px((ea << 24) | (p & 0xFFFFFFu), dst_row[sx]);
      }
    }
  }
  sh_blit(g_title_x, g_title_y, g_tb_w, g_tb_h);
}

static void draw_spinner_dot(int cx, int cy, int r, uint32_t rgb,
                             uint8_t alpha) {
  for (int dy = -r - 1; dy <= r + 1; dy++) {
    int y = cy + dy;
    if (y < 0 || y >= (int)g_h)
      continue;
    uint32_t *row = g_shadow + (uint64_t)y * g_pitch;
    for (int dx = -r - 1; dx <= r + 1; dx++) {
      int x = cx + dx;
      if (x < 0 || x >= (int)g_w)
        continue;
      int d2 = dx * dx + dy * dy, rr = r * r;
      if (d2 > (r + 1) * (r + 1))
        continue;
      int cov = 255;
      if (d2 > (r - 1) * (r - 1)) {
        int dist = 0;
        while ((dist + 1) * (dist + 1) <= d2)
          dist++;
        int edge = dist - (r - 1);
        cov = 255 - (edge * 255) / 2;
        if (cov < 0)
          cov = 0;
      }
      if (!cov)
        continue;
      uint32_t ea = (alpha * cov) >> 8;
      if (!ea)
        continue;
      row[x] = blend_px((ea << 24) | (rgb & 0xFFFFFFu), row[x]);
    }
  }
}

static void draw_spinner_frame(uint64_t ms) {
  if (ms < TITLE_TOTAL_MS)
    return;
  uint64_t sp_ms = ms - TITLE_TOTAL_MS;
  int head = (int)((sp_ms / (SPIN_ROT_MS / SPIN_DOTS)) % SPIN_DOTS);
  sh_fill(g_spin_cx - SPIN_PAD, g_spin_cy - SPIN_PAD, 2 * SPIN_PAD,
          2 * SPIN_PAD, COL_BG);
  for (int i = 0; i < SPIN_DOTS; i++) {
    int d = (head - i + SPIN_DOTS) % SPIN_DOTS;
    uint32_t bright = 255 - (uint32_t)(d * 230 / SPIN_DOTS);
    if (bright < 25)
      bright = 25;
    draw_spinner_dot(g_spin_cx + k_spin_dx[i], g_spin_cy + k_spin_dy[i],
                     SPIN_DOT_R, COL_SPINNER, (uint8_t)bright);
  }
  sh_blit(g_spin_cx - SPIN_PAD, g_spin_cy - SPIN_PAD, 2 * SPIN_PAD,
          2 * SPIN_PAD);
}

void boot_splash_tick(void) {
  if (!g_active || g_phase == 0 || g_phase >= 3)
    return;
  uint64_t now = rdtsc_now();
  if (g_last_tick && (now - g_last_tick) < ms_to_ticks(16))
    return;
  g_last_tick = now;
  uint64_t ms = elapsed_ms(now, g_start);
  if (g_phase == 1) {
    draw_title_frame(ms);
    if (ms >= TITLE_TOTAL_MS)
      g_phase = 2;
  } else {
    draw_spinner_frame(ms);
  }
}

void boot_splash_task_loop(void) {
  while (g_active && g_phase >= 1 && g_phase < 3) {
    boot_splash_tick();
    uint64_t until = rdtsc_now() + ms_to_ticks(16);
    while (rdtsc_now() < until)
      __asm__ volatile("pause");
    sched_yield();
  }
}

void boot_splash_begin_reveal(boot_splash_render_fn_t render_below) {
  if (!g_active || g_phase >= 3)
    return;
  while (elapsed_ms(rdtsc_now(), g_start) < MIN_SPLASH_MS) {
    boot_splash_tick();
    __asm__ volatile("pause");
  }
  while (g_phase == 1) {
    boot_splash_tick();
    __asm__ volatile("pause");
  }
  g_render_below = render_below;
  if (render_below) {
    size_t fb_bytes = (size_t)g_pitch * g_h * sizeof(uint32_t);
    g_desktop_bm = (uint32_t *)kmalloc(fb_bytes);
    if (g_desktop_bm) {
      render_below(g_shadow, g_w, g_h, g_pitch);
      for (uint32_t y = 0; y < g_h; y++) {
        uint32_t *src = g_shadow + (uint64_t)y * g_pitch;
        uint32_t *dst = g_desktop_bm + (uint64_t)y * g_pitch;
        for (uint32_t x = 0; x < g_pitch; x++) {
          dst[x] = src[x];
          src[x] = COL_BG;
        }
      }
    }
  }
  g_prev_offset = 0;
  g_reveal_start = rdtsc_now();
  g_phase = 3;
  g_active = 0;
}

int boot_splash_tick_reveal(void) {
  if (g_phase != 3)
    return 0;
  uint64_t ms = elapsed_ms(rdtsc_now(), g_reveal_start);
  uint32_t new_offset =
      (ms >= REVEAL_MS) ? g_h : (uint32_t)(((uint64_t)g_h * ms) / REVEAL_MS);
  if (new_offset > g_h)
    new_offset = g_h;
  uint32_t y0 = g_h - new_offset, y1 = g_h - g_prev_offset;
  for (uint32_t y = y0; y < y1; y++) {
    uint32_t *row = g_fb + (uint64_t)y * g_pitch;
    if (g_desktop_bm) {
      const uint32_t *src = g_desktop_bm + (uint64_t)y * g_pitch;
      for (uint32_t x = 0; x < g_w; x++)
        row[x] = src[x];
    } else {
      for (uint32_t x = 0; x < g_w; x++)
        row[x] = COL_DESKTOP;
    }
  }
  if (y1 > y0)
    fb_flush_rect(g_fb, g_pitch, 0, y0, g_w, y1 - y0);
  g_prev_offset = new_offset;
  if (new_offset >= g_h) {
    g_phase = 4;
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
  font_manager_init();
  g_font = font_manager_get(FONT_ID_MAIN_REGULAR);
  if (!g_font)
    g_font = font_manager_get(FONT_ID_MONO);
  if (!g_font)
    return -1;
  size_t shadow_bytes = (size_t)g_pitch * g_h * sizeof(uint32_t);
  g_shadow = (uint32_t *)kmalloc(shadow_bytes);
  if (!g_shadow)
    return -1;
  for (uint32_t y = 0; y < g_h; y++) {
    uint32_t *row = g_shadow + (uint64_t)y * g_pitch;
    for (uint32_t x = 0; x < g_pitch; x++)
      row[x] = COL_BG;
  }

  /* Handoff desde bootloader */
  g_phase = g_boot_splash_initial_phase ? g_boot_splash_initial_phase : 1;
  g_start = rdtsc_now() - ms_to_ticks(g_boot_splash_initial_ms);

  if (g_phase == 1)
    sh_blit(0, 0, (int)g_w, (int)g_h);

  prerender_title();
  if (!g_title_bm)
    return -1;
  g_spin_cx = (int)g_w / 2;
  g_spin_cy = g_title_y + g_tb_h + 18 + SPIN_PAD;
  if (g_phase == 2) {
    g_last_tick = 0;
  } else {
    g_start = rdtsc_now();
  }
  g_active = 1;
  return 0;
}

int boot_splash_active(void) { return g_active; }
void boot_splash_set_tsc_hz(uint64_t hz) {
  if (hz)
    g_tsc_hz = hz;
}
void boot_splash_finish(void) {
  g_phase = 4;
  g_active = 0;
}