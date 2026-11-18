// user/lib/text_aa.h — texto UTF-8 antialiased para las apps nativas.
//
// Las fuentes del sistema (font_system.c) solo traen glifos ASCII. Como el
// kernel (kernel/gfx/font_aa.c), los caracteres Latin-1 del español y otros
// habituales se componen: letra base + acento dibujado (á é í ó ú à è ü ñ...),
// ¿ ¡ como ? ! invertidos, comillas tipográficas y guiones como su ASCII.
// Solo cabecera: funciones static inline, sin estado.
#pragma once
#include "font_aa.h"
#include <stdint.h>

enum { UI_ACC_NONE = 0, UI_ACC_ACUTE, UI_ACC_GRAVE, UI_ACC_CIRC, UI_ACC_DIAER,
       UI_ACC_TILDE, UI_ACC_FLIP };

// Siguiente codepoint de *ps (avanza el puntero). Secuencias rotas: U+FFFD.
static inline uint32_t ui_utf8_next(const char **ps) {
  const unsigned char *s = (const unsigned char *)*ps;
  uint32_t c = *s++;
  int n = 0;
  if (c < 0x80) {
    *ps = (const char *)s;
    return c;
  } else if ((c & 0xE0) == 0xC0) {
    c &= 0x1F;
    n = 1;
  } else if ((c & 0xF0) == 0xE0) {
    c &= 0x0F;
    n = 2;
  } else if ((c & 0xF8) == 0xF0) {
    c &= 0x07;
    n = 3;
  } else {
    *ps = (const char *)s;
    return 0xFFFD;
  }
  while (n--) {
    if ((*s & 0xC0) != 0x80) {
      *ps = (const char *)s;
      return 0xFFFD;
    }
    c = (c << 6) | (*s++ & 0x3F);
  }
  *ps = (const char *)s;
  return c;
}

// Longitud en bytes de la secuencia UTF-8 que empieza con el byte b.
static inline int ui_utf8_len(unsigned char b) {
  return b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3
                                            : (b & 0xF8) == 0xF0 ? 4 : 1;
}

static inline const glyph_aa_t *ui_resolve_glyph(const font_aa_t *f, uint32_t cp,
                                                 int *acc) {
  static const struct {
    uint16_t cp;
    char base;
    uint8_t acc;
  } map[] = {
      {0xE1, 'a', UI_ACC_ACUTE}, {0xE9, 'e', UI_ACC_ACUTE}, {0xED, 'i', UI_ACC_ACUTE},
      {0xF3, 'o', UI_ACC_ACUTE}, {0xFA, 'u', UI_ACC_ACUTE}, {0xC1, 'A', UI_ACC_ACUTE},
      {0xC9, 'E', UI_ACC_ACUTE}, {0xCD, 'I', UI_ACC_ACUTE}, {0xD3, 'O', UI_ACC_ACUTE},
      {0xDA, 'U', UI_ACC_ACUTE}, {0xE0, 'a', UI_ACC_GRAVE}, {0xE8, 'e', UI_ACC_GRAVE},
      {0xEC, 'i', UI_ACC_GRAVE}, {0xF2, 'o', UI_ACC_GRAVE}, {0xF9, 'u', UI_ACC_GRAVE},
      {0xC0, 'A', UI_ACC_GRAVE}, {0xC8, 'E', UI_ACC_GRAVE}, {0xCC, 'I', UI_ACC_GRAVE},
      {0xD2, 'O', UI_ACC_GRAVE}, {0xD9, 'U', UI_ACC_GRAVE}, {0xE2, 'a', UI_ACC_CIRC},
      {0xEA, 'e', UI_ACC_CIRC},  {0xEE, 'i', UI_ACC_CIRC},  {0xF4, 'o', UI_ACC_CIRC},
      {0xFB, 'u', UI_ACC_CIRC},  {0xE4, 'a', UI_ACC_DIAER}, {0xEB, 'e', UI_ACC_DIAER},
      {0xEF, 'i', UI_ACC_DIAER}, {0xF6, 'o', UI_ACC_DIAER}, {0xFC, 'u', UI_ACC_DIAER},
      {0xFF, 'y', UI_ACC_DIAER}, {0xC4, 'A', UI_ACC_DIAER}, {0xCB, 'E', UI_ACC_DIAER},
      {0xCF, 'I', UI_ACC_DIAER}, {0xD6, 'O', UI_ACC_DIAER}, {0xDC, 'U', UI_ACC_DIAER},
      {0xF1, 'n', UI_ACC_TILDE}, {0xD1, 'N', UI_ACC_TILDE}, {0xBF, '?', UI_ACC_FLIP},
      {0xA1, '!', UI_ACC_FLIP},  {0xE7, 'c', UI_ACC_NONE},  {0xC7, 'C', UI_ACC_NONE},
      {0xBA, 'o', UI_ACC_NONE},  {0xAA, 'a', UI_ACC_NONE},  {0x20AC, 'E', UI_ACC_NONE},
      {0x2018, '\'', UI_ACC_NONE}, {0x2019, '\'', UI_ACC_NONE},
      {0x201C, '"', UI_ACC_NONE},  {0x201D, '"', UI_ACC_NONE},
      {0x2013, '-', UI_ACC_NONE},  {0x2014, '-', UI_ACC_NONE},
      {0x00B7, '.', UI_ACC_NONE},  {0x2022, '*', UI_ACC_NONE},
  };
  *acc = UI_ACC_NONE;
  if (cp < 128)
    return &f->glyphs[cp];
  for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if (map[i].cp == cp) {
      *acc = map[i].acc;
      return &f->glyphs[(unsigned char)map[i].base];
    }
  return &f->glyphs[(unsigned char)'?'];
}

static inline void ui_blend_px(uint32_t *buf, int stride, int bw, int bh, int x,
                               int y, uint32_t rgb, uint32_t a) {
  if (x < 0 || y < 0 || x >= bw || y >= bh || !a)
    return;
  uint32_t d = buf[y * stride + x];
  uint32_t r = (((rgb >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
  uint32_t g = (((rgb >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
  uint32_t b = ((rgb & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
  buf[y * stride + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline int ui_text_width(const font_aa_t *f, const char *s) {
  int w = 0, acc;
  while (*s)
    w += ui_resolve_glyph(f, ui_utf8_next(&s), &acc)->advance;
  return w;
}

// Dibuja `s` (UTF-8, `len` bytes o hasta el NUL si len < 0) con la esquina
// superior izquierda de la celda en (x, y). Devuelve la x final.
static inline int ui_draw_text_n(uint32_t *buf, int stride, int bw, int bh, int x,
                                 int y, const char *s, int len, uint32_t rgb,
                                 const font_aa_t *f) {
  const char *end = len < 0 ? 0 : s + len;
  while (*s && (!end || s < end)) {
    int acc;
    const glyph_aa_t *g = ui_resolve_glyph(f, ui_utf8_next(&s), &acc);
    if (g->bitmap && g->width && g->height) {
      int flip = acc == UI_ACC_FLIP;
      int dx = x + g->bearing_x;
      int dy = y + (f->height - g->bearing_y) + (flip ? 2 : 0);
      for (int j = 0; j < g->height; j++) {
        int sr = flip ? g->height - 1 - j : j;
        for (int i = 0; i < g->width; i++)
          ui_blend_px(buf, stride, bw, bh, dx + i, dy + j, rgb,
                      g->bitmap[sr * g->width + i]);
      }
      if (acc != UI_ACC_NONE && !flip) {
        int cx = dx + g->width / 2, t = dy;
        uint32_t a = 230;
#define UIP(px, py) ui_blend_px(buf, stride, bw, bh, cx + (px), t + (py), rgb, a)
        switch (acc) {
        case UI_ACC_ACUTE:
          UIP(1, -4); UIP(2, -4); UIP(0, -3); UIP(1, -3); UIP(-1, -2); UIP(0, -2);
          break;
        case UI_ACC_GRAVE:
          UIP(-1, -4); UIP(0, -4); UIP(0, -3); UIP(1, -3); UIP(1, -2); UIP(2, -2);
          break;
        case UI_ACC_CIRC:
          UIP(0, -4); UIP(-1, -3); UIP(1, -3); UIP(-2, -2); UIP(2, -2);
          break;
        case UI_ACC_DIAER:
          UIP(-2, -3); UIP(-1, -3); UIP(-2, -2); UIP(-1, -2);
          UIP(1, -3); UIP(2, -3); UIP(1, -2); UIP(2, -2);
          break;
        case UI_ACC_TILDE:
          UIP(-3, -2); UIP(-2, -3); UIP(-1, -3); UIP(0, -2); UIP(1, -3); UIP(2, -3);
          UIP(3, -2);
          break;
        }
#undef UIP
      }
    }
    x += g->advance;
  }
  return x;
}

static inline int ui_draw_text(uint32_t *buf, int stride, int bw, int bh, int x,
                               int y, const char *s, uint32_t rgb,
                               const font_aa_t *f) {
  return ui_draw_text_n(buf, stride, bw, bh, x, y, s, -1, rgb, f);
}
