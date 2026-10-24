#include "font_aa.h"

// ---------------------------------------------------------------------------
// Decodificación UTF-8 mínima. Secuencias inválidas -> U+FFFD.
// ---------------------------------------------------------------------------
static uint32_t utf8_next(const char **ps) {
  const unsigned char *s = (const unsigned char *)*ps;
  uint32_t c = *s++;
  if (c < 0x80) {
    *ps = (const char *)s;
    return c;
  }
  int n;
  uint32_t cp;
  if ((c & 0xE0) == 0xC0) {
    n = 1;
    cp = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    n = 2;
    cp = c & 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    n = 3;
    cp = c & 0x07;
  } else {
    *ps = (const char *)s;
    return 0xFFFD;
  }
  for (int i = 0; i < n; i++) {
    if ((*s & 0xC0) != 0x80) {
      *ps = (const char *)s; // no consumir el byte que no era continuación
      return 0xFFFD;
    }
    cp = (cp << 6) | (*s & 0x3F);
    s++;
  }
  *ps = (const char *)s;
  return cp;
}

// ---------------------------------------------------------------------------
// Composición de caracteres Latin-1 a partir de glifos ASCII.
// ---------------------------------------------------------------------------
enum { ACC_NONE = 0, ACC_ACUTE, ACC_GRAVE, ACC_CIRC, ACC_DIAER, ACC_TILDE,
       ACC_FLIP };

typedef struct {
  uint16_t cp;
  char base;
  uint8_t acc;
} cpmap_t;

static const cpmap_t cpmap[] = {
    {0xE1, 'a', ACC_ACUTE}, {0xE9, 'e', ACC_ACUTE}, {0xED, 'i', ACC_ACUTE},
    {0xF3, 'o', ACC_ACUTE}, {0xFA, 'u', ACC_ACUTE}, {0xC1, 'A', ACC_ACUTE},
    {0xC9, 'E', ACC_ACUTE}, {0xCD, 'I', ACC_ACUTE}, {0xD3, 'O', ACC_ACUTE},
    {0xDA, 'U', ACC_ACUTE}, {0xE0, 'a', ACC_GRAVE}, {0xE8, 'e', ACC_GRAVE},
    {0xEC, 'i', ACC_GRAVE}, {0xF2, 'o', ACC_GRAVE}, {0xF9, 'u', ACC_GRAVE},
    {0xC0, 'A', ACC_GRAVE}, {0xC8, 'E', ACC_GRAVE}, {0xCC, 'I', ACC_GRAVE},
    {0xD2, 'O', ACC_GRAVE}, {0xD9, 'U', ACC_GRAVE}, {0xE2, 'a', ACC_CIRC},
    {0xEA, 'e', ACC_CIRC},  {0xEE, 'i', ACC_CIRC},  {0xF4, 'o', ACC_CIRC},
    {0xFB, 'u', ACC_CIRC},  {0xE4, 'a', ACC_DIAER}, {0xEB, 'e', ACC_DIAER},
    {0xEF, 'i', ACC_DIAER}, {0xF6, 'o', ACC_DIAER}, {0xFC, 'u', ACC_DIAER},
    {0xFF, 'y', ACC_DIAER}, {0xC4, 'A', ACC_DIAER}, {0xCB, 'E', ACC_DIAER},
    {0xCF, 'I', ACC_DIAER}, {0xD6, 'O', ACC_DIAER}, {0xDC, 'U', ACC_DIAER},
    {0xF1, 'n', ACC_TILDE}, {0xD1, 'N', ACC_TILDE}, {0xBF, '?', ACC_FLIP},
    {0xA1, '!', ACC_FLIP},  {0xE7, 'c', ACC_NONE},  {0xC7, 'C', ACC_NONE},
    {0x2018, '\'', ACC_NONE}, {0x2019, '\'', ACC_NONE},
    {0x201C, '"', ACC_NONE},  {0x201D, '"', ACC_NONE},
    {0x2013, '-', ACC_NONE},  {0x2014, '-', ACC_NONE},
    {0x00B7, '.', ACC_NONE},  {0x2022, '*', ACC_NONE},
};

static const cpmap_t *cpmap_find(uint32_t cp) {
  for (unsigned i = 0; i < sizeof(cpmap) / sizeof(cpmap[0]); i++)
    if (cpmap[i].cp == cp)
      return &cpmap[i];
  return 0;
}

// Resuelve un codepoint a (glifo base, acento).
static const glyph_aa_t *resolve_glyph(const font_aa_t *f, uint32_t cp,
                                       int *acc) {
  *acc = ACC_NONE;
  if (cp < 128)
    return &f->glyphs[cp];
  const cpmap_t *m = cpmap_find(cp);
  if (m) {
    *acc = m->acc;
    return &f->glyphs[(unsigned char)m->base];
  }
  return &f->glyphs[(unsigned char)'?'];
}

static inline void put_px(uint32_t *dst, int stride, rect_t clip, int x, int y,
                          uint32_t rgb, uint32_t alpha) {
  if (x < clip.x || x >= clip.x + clip.w || y < clip.y || y >= clip.y + clip.h)
    return;
  uint32_t *p = &dst[y * stride + x];
  *p = blend_pixel_fast((alpha << 24) | rgb, *p);
}

// Marcas diacríticas de 2 px de grosor. `top` es la y del extremo superior
// del glifo base; las marcas se dibujan por encima, con 1 px de separación.
static void draw_accent(uint32_t *dst, int stride, rect_t clip, int cx, int top,
                        int acc, uint32_t rgb, uint32_t a) {
#define P(dx, dy) put_px(dst, stride, clip, cx + (dx), top + (dy), rgb, a)
  switch (acc) {
  case ACC_ACUTE:
    P(1, -4); P(2, -4); P(0, -3); P(1, -3); P(-1, -2); P(0, -2);
    break;
  case ACC_GRAVE:
    P(-1, -4); P(0, -4); P(0, -3); P(1, -3); P(1, -2); P(2, -2);
    break;
  case ACC_CIRC:
    P(0, -4); P(-1, -3); P(1, -3); P(-2, -2); P(2, -2);
    break;
  case ACC_DIAER:
    P(-2, -3); P(-1, -3); P(-2, -2); P(-1, -2);
    P(1, -3); P(2, -3); P(1, -2); P(2, -2);
    break;
  case ACC_TILDE:
    P(-3, -2); P(-2, -3); P(-1, -3); P(0, -2); P(1, -3); P(2, -3); P(3, -2);
    break;
  default:
    break;
  }
#undef P
}

void gfx_draw_string_aa(uint32_t *dst, int stride, rect_t clip, int x, int y,
                        const char *str, uint32_t color,
                        const font_aa_t *font) {
  if (!font || !str)
    return;

  int cur_x = x;
  uint32_t base_alpha = (color >> 24) & 0xFF;
  uint32_t rgb = color & 0x00FFFFFF;

  const char *p = str;
  while (*p) {
    int acc;
    uint32_t cp = utf8_next(&p);
    const glyph_aa_t *g = resolve_glyph(font, cp, &acc);

    if (g->bitmap) {
      int flip = (acc == ACC_FLIP);
      int draw_x = cur_x + g->bearing_x;
      int draw_y = y + (font->height - g->bearing_y) + (flip ? 2 : 0);

      for (int gy = 0; gy < g->height; gy++) {
        int py = draw_y + gy;
        if (py < clip.y || py >= clip.y + clip.h)
          continue;
        int src_row = flip ? (g->height - 1 - gy) : gy;
        const uint8_t *src = &g->bitmap[src_row * g->width];

        for (int gx = 0; gx < g->width; gx++) {
          int px = draw_x + gx;
          if (px < clip.x || px >= clip.x + clip.w)
            continue;
          uint8_t fa = src[gx];
          if (fa > 0) {
            uint32_t final_alpha = (base_alpha * fa) / 255;
            uint32_t *pixel = &dst[py * stride + px];
            *pixel = blend_pixel_fast((final_alpha << 24) | rgb, *pixel);
          }
        }
      }

      if (acc != ACC_NONE && acc != ACC_FLIP)
        draw_accent(dst, stride, clip, draw_x + g->width / 2, draw_y, acc, rgb,
                    (base_alpha * 230) / 255);
    }
    cur_x += g->advance;
  }
}

int font_aa_text_width(const font_aa_t *font, const char *str) {
  if (!font || !str)
    return 0;
  int w = 0;
  const char *p = str;
  while (*p) {
    int acc;
    uint32_t cp = utf8_next(&p);
    w += resolve_glyph(font, cp, &acc)->advance;
  }
  return w;
}

int font_aa_cap_height(const font_aa_t *font) {
  if (!font)
    return 0;
  const glyph_aa_t *h = &font->glyphs[(unsigned char)'H'];
  if (h->bitmap && h->bearing_y > 0)
    return h->bearing_y;
  return (font->height * 3) / 4;
}
