// kernel/gfx/about.c
#include "about.h"
#include "../cpu.h"
#include "../smp_boot.h"
#include "../string.h"
#include "font_manager.h"
#include "gfx_filter.h"
#include "theme.h"

extern uint64_t pmm_total_usable_pages(void);
extern uint32_t fb_width, fb_height;

#define LABEL_X 128  /* borde derecho de las etiquetas */
#define VALUE_X 140  /* borde izquierdo de los valores */
#define ROW_H 22

static inline uint32_t lerp_rgb(uint32_t a, uint32_t b, int t /*0..256*/) {
  uint32_t r = (((a >> 16) & 0xFF) * (256 - t) + ((b >> 16) & 0xFF) * t) >> 8;
  uint32_t g = (((a >> 8) & 0xFF) * (256 - t) + ((b >> 8) & 0xFF) * t) >> 8;
  uint32_t bl = ((a & 0xFF) * (256 - t) + (b & 0xFF) * t) >> 8;
  return 0xFF000000 | (r << 16) | (g << 8) | bl;
}

/* Logo: esfera con degradado cian -> azul -> índigo, brillo superior y tres
 * cintas de luz (la "aurora"). */
static void draw_logo(uint32_t *buf, int stride, rect_t clip, float cx,
                      float cy, float r) {
  int y0 = (int)(cy - r) - 1, y1 = (int)(cy + r) + 2;
  for (int y = y0; y < y1; y++) {
    int t = (int)(((float)y - (cy - r)) / (2.0f * r) * 512.0f);
    t = t < 0 ? 0 : (t > 512 ? 512 : t);
    uint32_t c = t < 256 ? lerp_rgb(0xFF64D2FF, 0xFF0A84FF, t)
                         : lerp_rgb(0xFF0A84FF, 0xFF5E5CE6, t - 256);
    rect_t row = rect_clip(clip, (rect_t){clip.x, y, clip.w, 1});
    if (row.w > 0)
      gfx_fill_circle(buf, stride, row, cx, cy, r, c);
  }
  gfx_fill_circle(buf, stride, clip, cx, cy - r * 0.34f, r * 0.64f,
                  0x1EFFFFFF);

  /* Cintas: misma curva desplazada, cada una más fina y tenue. */
  static const float k_pts[5][2] = {
      {-0.62f, 0.26f}, {-0.30f, -0.02f}, {0.02f, 0.12f},
      {0.32f, -0.06f}, {0.60f, -0.30f}};
  static const struct {
    float dy, hw;
    uint32_t col;
  } k_ribbon[3] = {{0.0f, 2.6f, 0xD8FFFFFF},
                   {0.18f, 1.7f, 0x90FFFFFF},
                   {0.33f, 1.1f, 0x58FFFFFF}};
  for (int k = 0; k < 3; k++) {
    for (int i = 0; i < 4; i++) {
      gfx_draw_capsule(
          buf, stride, clip, cx + k_pts[i][0] * r,
          cy + (k_pts[i][1] + k_ribbon[k].dy) * r, cx + k_pts[i + 1][0] * r,
          cy + (k_pts[i + 1][1] + k_ribbon[k].dy) * r, k_ribbon[k].hw,
          k_ribbon[k].col);
    }
  }
}

/* Texto ampliado `scale256`/256 veces (la fuente del sistema es de 14 px). */
static void draw_big_text(uint32_t *buf, int stride, rect_t clip, int cx,
                          int y, const char *s, uint32_t color,
                          int scale256) {
  static uint32_t tmp[200 * 24]; /* solo hilo del compositor */
  const int TW = 200, TH = 24;
  int tw = gfx_text_width(s, FONT_ID_MAIN_BOLD) + 2;
  if (tw > TW)
    tw = TW;
  for (int i = 0; i < TW * TH; i++)
    tmp[i] = color & 0x00FFFFFF; /* transparente del mismo color: sin halo */
  gfx_draw_string(tmp, TW, (rect_t){0, 0, tw, TH}, 1,
                  gfx_text_center_y(FONT_ID_MAIN_BOLD, 0, TH), s, color,
                  FONT_ID_MAIN_BOLD);
  /* Pasar a una imagen compacta tw x TH para el escalado. */
  for (int y = 0; y < TH; y++)
    for (int x = 0; x < tw; x++)
      tmp[y * tw + x] = tmp[y * TW + x];
  int dw = tw * scale256 / 256, dh = TH * scale256 / 256;
  gfx_blit_scaled(buf, stride, clip, cx - dw / 2, y, dw, dh, tmp, tw, TH,
                  GFX_FILTER_BILINEAR);
}

static void draw_centered(uint32_t *buf, int stride, rect_t clip, int cx,
                          int top, int h, const char *s, uint32_t color,
                          font_id_t f) {
  int w = gfx_text_width(s, f);
  gfx_draw_string(buf, stride, clip, cx - w / 2, gfx_text_center_y(f, top, h),
                  s, color, f);
}

/* Recorta s con "..." para que quepa en max_w. */
static void fit(char *s, int max_w, font_id_t f) {
  size_t n = strlen(s);
  if (gfx_text_width(s, f) <= max_w)
    return;
  while (n > 0) {
    s[--n] = '\0';
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
      s[--n] = '\0';
    char t[96];
    snprintf(t, sizeof(t), "%s...", s);
    if (gfx_text_width(t, f) <= max_w) {
      snprintf(s, 96, "%s", t);
      return;
    }
  }
}

/* Nombre comercial de la CPU (CPUID 0x80000002..4), sin espacios de más. */
static void cpu_brand(char *out, size_t cap) {
  uint32_t a, b, c, d;
  cpuid(0x80000000, 0, &a, &b, &c, &d);
  if (a < 0x80000004) {
    snprintf(out, cap, "x86_64");
    return;
  }
  char raw[49];
  uint32_t *w = (uint32_t *)raw;
  for (uint32_t leaf = 0; leaf < 3; leaf++)
    cpuid(0x80000002 + leaf, 0, &w[leaf * 4], &w[leaf * 4 + 1],
          &w[leaf * 4 + 2], &w[leaf * 4 + 3]);
  raw[48] = '\0';
  size_t n = 0;
  for (const char *p = raw; *p && n + 1 < cap; p++) {
    if (*p == ' ' && (n == 0 || out[n - 1] == ' '))
      continue;
    out[n++] = *p;
  }
  while (n > 0 && out[n - 1] == ' ')
    n--;
  out[n] = '\0';
  if (n == 0)
    snprintf(out, cap, "x86_64");
}

static void memory_str(char *out, size_t cap) {
  /* Memoria utilizable por el kernel (sin lo que reserva el firmware): es la
   * cifra real que conocemos, no la instalada. */
  uint64_t mb = pmm_total_usable_pages() * 4096ULL / (1024 * 1024);
  if (mb >= 1024) {
    uint64_t tenths = (mb * 10 + 512) / 1024;
    if (tenths % 10 == 0)
      snprintf(out, cap, "%u GB", (unsigned)(tenths / 10));
    else
      snprintf(out, cap, "%u,%u GB", (unsigned)(tenths / 10),
               (unsigned)(tenths % 10));
  } else {
    snprintf(out, cap, "%u MB", (unsigned)mb);
  }
}

void about_paint(window_t *win) {
  if (!win || !win->content_buffer)
    return;
  uint32_t *buf = win->content_buffer;
  int W = win->content_w, H = win->content_h;
  rect_t clip = {0, 0, W, H};
  int cx = W / 2;

  for (int i = 0; i < W * H; i++)
    buf[i] = AURORA_WIN_BG;

  draw_logo(buf, W, clip, (float)cx, 84.0f, 54.0f);
  draw_big_text(buf, W, clip, cx, 154, "AuroraOS", AURORA_TEXT_PRIMARY, 448);
  draw_centered(buf, W, clip, cx, 196, 20, "Sistema operativo de 64 bits",
                AURORA_TEXT_TERTIARY, FONT_ID_MAIN_REGULAR);

  char cpu[96], cores[16], mem[24], screen[32], built[32];
  cpu_brand(cpu, sizeof(cpu));
  fit(cpu, W - VALUE_X - 16, FONT_ID_MAIN_REGULAR);
  int n = smp_online_cpus();
  snprintf(cores, sizeof(cores), "%d", n);
  memory_str(mem, sizeof(mem));
  snprintf(screen, sizeof(screen), "%u x %u", (unsigned)fb_width,
           (unsigned)fb_height);
  snprintf(built, sizeof(built), "%s", __DATE__);

  const char *labels[] = {"Procesador", "Núcleos", "Memoria útil", "Pantalla",
                          "Compilado"};
  const char *values[] = {cpu, cores, mem, screen, built};
  int y = 244;
  gfx_blend_rect(buf, W, clip, (rect_t){28, y - 12, W - 56, 1}, 0x1AFFFFFF);
  for (int i = 0; i < 5; i++, y += ROW_H) {
    int lw = gfx_text_width(labels[i], FONT_ID_MAIN_BOLD);
    int ty = gfx_text_center_y(FONT_ID_MAIN_REGULAR, y, ROW_H);
    gfx_draw_string(buf, W, clip, LABEL_X - lw, ty, labels[i],
                    AURORA_TEXT_SECONDARY, FONT_ID_MAIN_BOLD);
    gfx_draw_string(buf, W, clip, VALUE_X, ty, values[i],
                    AURORA_TEXT_TERTIARY, FONT_ID_MAIN_REGULAR);
  }
  gfx_blend_rect(buf, W, clip, (rect_t){28, y + 12, W - 56, 1}, 0x1AFFFFFF);

  win_update(win);
}
