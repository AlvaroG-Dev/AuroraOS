// kernel/boot_console.c
#include "boot_console.h"
#include "gfx/font_aa.h"
#include "gfx/font_manager.h"
#include "gfx/gfx.h"
#include "heap.h"
#include "klog.h"
#include <stddef.h>

#define COL_BG 0xFF101828u
#define COL_TS 0xFF6C7A8Au
#define COL_OK 0xFF50E880u
#define COL_PANIC 0xFFFF4040u
#define COL_ERR 0xFFFF7070u
#define COL_WARN 0xFFFFC860u
#define COL_INFO 0xFFC0C8D0u
#define COL_DEBUG 0xFF9099A8u
#define COL_TRACE 0xFF707888u

// Tamaño máximo del área de consola. No hace falta que ocupe toda la
// pantalla: 120 columnas × 60 filas son 1200×840 px = ~4 MB de sombra,
// suficiente para cualquier log de boot.
#define CONSOLE_MAX_COLS 120
#define CONSOLE_MAX_ROWS 60

static uint32_t *g_fb = NULL;
static uint32_t g_fb_pitch = 0; // en dwords
static uint32_t g_fb_w = 0, g_fb_h = 0;
extern void fb_flush_rect(const uint32_t *src, uint32_t src_pitch, uint32_t x,
                          uint32_t y, uint32_t w, uint32_t h);

static uint32_t *g_shadow = NULL; // buffer en RAM cacheable
static uint32_t g_shadow_w = 0;   // stride en px (= g_cols * cell_w)
static uint32_t g_shadow_h = 0;   // alto en px (= g_rows * cell_h)

static uint32_t g_cols = 0, g_rows = 0;
static uint32_t g_cell_w = 9, g_cell_h = 20;
static uint32_t g_col = 0, g_row = 0;
static const font_aa_t *g_font = NULL;

// ---------------------------------------------------------------------------
// Sombra: todo el dibujado vive aquí, en RAM cacheable.
// ---------------------------------------------------------------------------
static void shadow_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        uint32_t c) {
  if (!g_shadow)
    return;
  if (x >= g_shadow_w || y >= g_shadow_h)
    return;
  if (x + w > g_shadow_w)
    w = g_shadow_w - x;
  if (y + h > g_shadow_h)
    h = g_shadow_h - y;
  for (uint32_t yy = 0; yy < h; yy++) {
    uint32_t *row = g_shadow + (uint64_t)(y + yy) * g_shadow_w + x;
    for (uint32_t xx = 0; xx < w; xx++)
      row[xx] = c;
  }
}

// Copia filas de la sombra al framebuffer. El FB es WC: las escrituras
// son rápidas porque son secuenciales y NUNCA leemos del FB.
static void blit_rows_to_fb(uint32_t src_y, uint32_t dst_y, uint32_t rows) {
  if (!g_fb || !g_shadow)
    return;
  for (uint32_t yy = 0; yy < rows; yy++) {
    uint32_t *dst = g_fb + (uint64_t)(dst_y + yy) * g_fb_pitch;
    uint32_t *src = g_shadow + (uint64_t)(src_y + yy) * g_shadow_w;
    for (uint32_t xx = 0; xx < g_shadow_w; xx++)
      dst[xx] = src[xx];
  }
  fb_flush_rect(g_fb, g_fb_pitch, 0, dst_y, g_shadow_w, rows);
}

static void blit_cell_to_fb(uint32_t col, uint32_t row) {
  if (!g_fb || !g_shadow)
    return;
  uint32_t x = col * g_cell_w;
  uint32_t y = row * g_cell_h;
  for (uint32_t yy = 0; yy < g_cell_h; yy++) {
    uint32_t *dst = g_fb + (uint64_t)(y + yy) * g_fb_pitch + x;
    uint32_t *src = g_shadow + (uint64_t)(y + yy) * g_shadow_w + x;
    for (uint32_t xx = 0; xx < g_cell_w; xx++)
      dst[xx] = src[xx];
  }
  fb_flush_rect(g_fb, g_fb_pitch, x, y, g_cell_w, g_cell_h);
}

// Scroll en RAM (memmove cacheable) + un único blit del área completa
// al FB. Coste: ~1 ms el memmove + ~5 ms el blit, en lugar de 2 s.
static void scroll_one_row(void) {
  if (!g_shadow)
    return;
  uint32_t move_rows = g_shadow_h - g_cell_h;
  uint32_t *src = g_shadow + (uint64_t)g_cell_h * g_shadow_w;
  __builtin_memmove(g_shadow, src,
                    (size_t)move_rows * g_shadow_w * sizeof(uint32_t));
  shadow_fill(0, g_shadow_h - g_cell_h, g_shadow_w, g_cell_h, COL_BG);
  blit_rows_to_fb(0, 0, g_shadow_h);
}

static uint32_t color_for(klog_level_t level, klog_fb_style_t style) {
  switch (style) {
  case KLOG_FB_TS:
    return COL_TS;
  case KLOG_FB_OK:
    return COL_OK;
  default:
    switch (level) {
    case KLOG_PANIC:
      return COL_PANIC;
    case KLOG_ERR:
      return COL_ERR;
    case KLOG_WARN:
      return COL_WARN;
    case KLOG_INFO:
      return COL_INFO;
    case KLOG_DEBUG:
      return COL_DEBUG;
    case KLOG_TRACE:
      return COL_TRACE;
    default:
      return COL_INFO;
    }
  }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
void boot_console_init(uint32_t *fb, uint32_t width, uint32_t height,
                       uint32_t pitch_px) {
  g_fb = fb;
  g_fb_w = width;
  g_fb_h = height;
  g_fb_pitch = pitch_px;
  g_col = 0;
  g_row = 0;

  font_manager_init();
  g_font = font_manager_get(FONT_ID_MONO);
  if (!g_font)
    g_font = font_manager_get(FONT_ID_MAIN_REGULAR);

  if (g_font) {
    const glyph_aa_t *M = &g_font->glyphs[(unsigned char)'M'];
    g_cell_w = M->advance ? M->advance : 9;
    g_cell_h = (uint32_t)g_font->height + 6;
  } else {
    g_cell_w = 9;
    g_cell_h = 20;
  }

  // Cuántas celdas caben en pantalla, con techo por si la pantalla es
  // enorme (no queremos reservar 8 MB de sombra cuando no hace falta).
  g_cols = width / g_cell_w;
  if (g_cols > CONSOLE_MAX_COLS)
    g_cols = CONSOLE_MAX_COLS;
  g_rows = height / g_cell_h;
  if (g_rows > CONSOLE_MAX_ROWS)
    g_rows = CONSOLE_MAX_ROWS;

  g_shadow_w = g_cols * g_cell_w;
  g_shadow_h = g_rows * g_cell_h;

  g_shadow =
      (uint32_t *)kmalloc((size_t)g_shadow_w * g_shadow_h * sizeof(uint32_t));
  if (!g_shadow)
    return;

  shadow_fill(0, 0, g_shadow_w, g_shadow_h, COL_BG);
  blit_rows_to_fb(0, 0, g_shadow_h);
}

void boot_console_clear(void) {
  if (!g_shadow)
    return;
  shadow_fill(0, 0, g_shadow_w, g_shadow_h, COL_BG);
  blit_rows_to_fb(0, 0, g_shadow_h);
  g_col = 0;
  g_row = 0;
}

void boot_console_putchar(char c, klog_level_t level, klog_fb_style_t style) {
  if (!g_shadow)
    return;

  if (c == '\r') {
    g_col = 0;
    return;
  }
  if (c == '\n') {
    g_col = 0;
    g_row++;
    if (g_row >= g_rows) {
      scroll_one_row();
      g_row = g_rows - 1;
    }
    return;
  }
  if (c == '\t') {
    g_col = (g_col + 4) & ~3u;
    if (g_col >= g_cols)
      g_col = g_cols - 1;
    return;
  }
  if ((unsigned char)c < 0x20)
    return;

  if (g_col >= g_cols) {
    g_col = 0;
    g_row++;
    if (g_row >= g_rows) {
      scroll_one_row();
      g_row = g_rows - 1;
    }
  }

  // Dibujar sólo en la sombra.
  uint32_t px = g_col * g_cell_w;
  uint32_t py = g_row * g_cell_h;
  shadow_fill(px, py, g_cell_w, g_cell_h, COL_BG);

  if (g_font) {
    uint32_t fg = color_for(level, style);
    char buf[2] = {c, 0};
    rect_t clip = {0, 0, (int)g_shadow_w, (int)g_shadow_h};
    gfx_draw_string_aa(g_shadow, (int)g_shadow_w, clip, (int)px, (int)py + 2,
                       buf, fg, g_font);
  }

  // Sólo esta celda al FB (140 dwords = 560 bytes).
  blit_cell_to_fb(g_col, g_row);
  g_col++;
}

void boot_console_hook_install(void) { klog_fb_hook = boot_console_putchar; }

void boot_console_hook_remove(void) {
  klog_fb_hook = NULL;
  // No liberamos g_shadow: liberarla aquí podría dar UAF si algún CPU
  // aún está dentro del hook desde otro mensaje. Son ~4 MB y sólo se
  // asignan una vez por boot. Aceptable.
}

// ===========================================================================
// ===========================================================================
// Overlay de diagnóstico. Se rasteriza en RAM cacheable y se copia al
// backbuffer SOLO en la región dañada. Nunca se lee la VRAM WC para mezclar.
// El fondo es opaco deliberadamente: un overlay de diagnóstico debe ser
// legible y no merece una lectura lenta del framebuffer para lograr alpha.
// ===========================================================================
#define OVERLAY_LINES 28
#define OVERLAY_SCALE 1
#define OVERLAY_MAX_COLS 100
#define OVERLAY_PAD_X 16
#define OVERLAY_PAD_Y 12
#define OVERLAY_BG 0xFF101828u

static int g_overlay_enabled = 0;
static uint32_t *g_overlay_layer = NULL;
static uint32_t g_overlay_w = 0, g_overlay_h = 0;
static uint32_t g_overlay_x = 0, g_overlay_y = 0;
static uint64_t g_overlay_last_line_count = 0;
static int g_overlay_cache_valid = 0;

void boot_console_overlay_enable(int on) {
  g_overlay_enabled = !!on;
  if (on)
    g_overlay_cache_valid = 0;
}
int boot_console_overlay_enabled(void) { return g_overlay_enabled; }

// Rasteriza el glifo en la capa cacheable. Las coordenadas son locales a ella.
static void draw_char_scaled_layer(uint32_t *target, uint32_t stride,
                                   uint32_t width, uint32_t height, int x,
                                   int y, char c, uint32_t rgb,
                                   const font_aa_t *font, int scale) {
  if (!target || !font || (unsigned char)c >= 128)
    return;
  const glyph_aa_t *g = &font->glyphs[(unsigned char)c];
  if (!g->bitmap || g->width == 0 || g->height == 0)
    return;

  int draw_x = x + g->bearing_x * scale;
  int draw_y = y + (font->height - g->bearing_y) * scale;
  for (int gy = 0; gy < g->height; gy++) {
    for (int gx = 0; gx < g->width; gx++) {
      uint8_t a = g->bitmap[gy * g->width + gx];
      if (a == 0)
        continue;
      uint32_t fg = ((uint32_t)a << 24) | (rgb & 0x00FFFFFF);
      int px = draw_x + gx * scale;
      int py = draw_y + gy * scale;
      for (int sy = 0; sy < scale; sy++) {
        int fy = py + sy;
        if (fy < 0 || fy >= (int)height)
          continue;
        uint32_t *row = target + (uint64_t)fy * stride;
        for (int sx = 0; sx < scale; sx++) {
          int fx = px + sx;
          if (fx < 0 || fx >= (int)width)
            continue;
          row[fx] = blend_pixel_fast(fg, row[fx]);
        }
      }
    }
  }
}

// Detecta el nivel de log al principio de la línea y devuelve el color.
static uint32_t overlay_level_color(const char *p, size_t pl) {
  if (pl >= 5) {
    if (p[0] == 'W' && p[1] == 'A' && p[2] == 'R' && p[3] == 'N')
      return 0xFFFFC860u;
    if (p[0] == 'E' && p[1] == 'R' && p[2] == 'R' && p[3] == 'O' &&
        p[4] == 'R')
      return 0xFFFF7070u;
    if (p[0] == 'P' && p[1] == 'A' && p[2] == 'N' && p[3] == 'I' &&
        p[4] == 'C')
      return 0xFFFF4040u;
    if (p[0] == 'D' && p[1] == 'E' && p[2] == 'B' && p[3] == 'U')
      return 0xFF9099A8u;
  }
  return 0xFFE0E0E0u;
}

static int overlay_rebuild_cache(void) {
  if (!g_fb || !g_font || g_fb_w <= 40 || g_cell_w == 0 || g_cell_h == 0)
    return -1;

  extern uint64_t klog_line_count(void);
  extern size_t klog_snapshot(char *out, size_t max);
  static char snap[16384];

  uint64_t cur = klog_line_count();
  if (g_overlay_cache_valid && cur == g_overlay_last_line_count)
    return 0;

  size_t snap_len = klog_snapshot(snap, sizeof(snap));
  if (snap_len == 0)
    return -1;

  const uint32_t cell_w = g_cell_w * OVERLAY_SCALE;
  const uint32_t cell_h = g_cell_h * OVERLAY_SCALE;
  uint32_t cols = OVERLAY_MAX_COLS;
  uint32_t box_w = cols * cell_w + 2 * OVERLAY_PAD_X;
  if (box_w > g_fb_w - 40) {
    box_w = g_fb_w - 40;
    cols = (box_w - 2 * OVERLAY_PAD_X) / cell_w;
  }
  uint32_t box_h = OVERLAY_LINES * cell_h + 2 * OVERLAY_PAD_Y;
  if (box_w == 0 || box_h == 0)
    return -1;

  if (!g_overlay_layer || box_w != g_overlay_w || box_h != g_overlay_h) {
    uint32_t *new_layer =
        (uint32_t *)kmalloc((size_t)box_w * box_h * sizeof(uint32_t));
    if (!new_layer)
      return -1;
    if (g_overlay_layer)
      kfree(g_overlay_layer);
    g_overlay_layer = new_layer;
    g_overlay_w = box_w;
    g_overlay_h = box_h;
  }

  g_overlay_x = (g_fb_w - box_w) / 2;
  g_overlay_y = (g_fb_h > box_h) ? (g_fb_h - box_h) / 2 : 0;

  // Construir una tarjeta opaca en RAM normal; ninguna lectura de VRAM.
  const size_t pixels = (size_t)g_overlay_w * g_overlay_h;
  for (size_t p = 0; p < pixels; p++)
    g_overlay_layer[p] = OVERLAY_BG;

  const uint32_t border = 0xFF3A4A6Au;
  for (uint32_t x = 0; x < g_overlay_w; x++) {
    g_overlay_layer[x] = border;
    g_overlay_layer[(size_t)(g_overlay_h - 1) * g_overlay_w + x] = border;
  }
  for (uint32_t y = 0; y < g_overlay_h; y++) {
    g_overlay_layer[(size_t)y * g_overlay_w] = border;
    g_overlay_layer[(size_t)y * g_overlay_w + g_overlay_w - 1] = border;
  }

  int total = 0;
  for (size_t k = 0; k < snap_len; k++)
    if (snap[k] == '\n')
      total++;
  int skip = total > OVERLAY_LINES ? total - OVERLAY_LINES : 0;

  size_t i = 0;
  int line = 0;
  while (i < snap_len && line < skip) {
    while (i < snap_len && snap[i] != '\n')
      i++;
    if (i < snap_len)
      i++;
    line++;
  }

  uint32_t row = 0;
  while (i < snap_len && row < OVERLAY_LINES) {
    size_t ls = i;
    while (i < snap_len && snap[i] != '\n')
      i++;
    size_t ll = i - ls;
    if (i < snap_len)
      i++;

    const char *p = snap + ls;
    size_t pl = ll;
    if (pl > 2 && p[0] == '[') {
      size_t k = 1;
      while (k < pl && p[k] != ']')
        k++;
      if (k < pl) {
        k++;
        while (k < pl && p[k] == ' ')
          k++;
      }
      p += k;
      pl -= k;
    }
    if (pl > cols)
      pl = cols;

    int tx = (int)OVERLAY_PAD_X;
    int ty = (int)OVERLAY_PAD_Y + (int)row * (int)cell_h;
    uint32_t color = overlay_level_color(p, pl);
    for (size_t col = 0; col < pl; col++) {
      int px = tx + (int)col * (int)cell_w;
      if (px + (int)cell_w > (int)g_overlay_w - (int)OVERLAY_PAD_X)
        break;
      draw_char_scaled_layer(g_overlay_layer, g_overlay_w, g_overlay_w,
                             g_overlay_h, px, ty, p[col], color, g_font,
                             OVERLAY_SCALE);
    }
    row++;
  }

  g_overlay_last_line_count = cur;
  g_overlay_cache_valid = 1;
  return 0;
}

/*
 * Se llama antes del blit, con el backbuffer cacheable como destino. La capa
 * se regenera solo cuando cambia el log, pero se vuelve a copiar sobre cada
 * región dañada que la intersecte: así no desaparece al repintarse el fondo.
 */
void boot_console_draw_overlay(uint32_t *dst, uint32_t dst_pitch, int clip_x,
                               int clip_y, int clip_w, int clip_h) {
  if (!g_overlay_enabled || !dst || !g_fb || clip_w <= 0 || clip_h <= 0)
    return;
  if (overlay_rebuild_cache() != 0 || !g_overlay_layer)
    return;

  int x0 = clip_x > (int)g_overlay_x ? clip_x : (int)g_overlay_x;
  int y0 = clip_y > (int)g_overlay_y ? clip_y : (int)g_overlay_y;
  int clip_x1 = clip_x + clip_w;
  int clip_y1 = clip_y + clip_h;
  int box_x1 = (int)g_overlay_x + (int)g_overlay_w;
  int box_y1 = (int)g_overlay_y + (int)g_overlay_h;
  int x1 = clip_x1 < box_x1 ? clip_x1 : box_x1;
  int y1 = clip_y1 < box_y1 ? clip_y1 : box_y1;
  if (x1 <= x0 || y1 <= y0)
    return;

  size_t row_bytes = (size_t)(x1 - x0) * sizeof(uint32_t);
  for (int y = y0; y < y1; y++) {
    const uint32_t *src =
        g_overlay_layer + (size_t)(y - (int)g_overlay_y) * g_overlay_w +
        (size_t)(x0 - (int)g_overlay_x);
    uint32_t *out = dst + (size_t)y * dst_pitch + x0;
    __builtin_memcpy(out, src, row_bytes);
  }
}
