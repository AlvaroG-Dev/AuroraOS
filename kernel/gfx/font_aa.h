#pragma once
#include "gfx.h"
#include <stdint.h>


typedef struct {
  uint8_t width;    // Ancho del glifo
  uint8_t height;   // Alto del glifo
  int8_t bearing_x; // Desplazamiento horizontal
  int8_t bearing_y; // Desplazamiento vertical
  uint8_t advance;  // Espaciado hacia el siguiente carácter
  const uint8_t
      *bitmap; // Mapa de opacidad de 8 bits (0 = transparente, 255 = opaco)
} glyph_aa_t;

typedef struct {
  uint8_t height;
  glyph_aa_t glyphs[128]; // Mapeo ASCII básico
} font_aa_t;

void gfx_draw_string_aa(uint32_t *dst, int stride, rect_t clip, int x, int y,
                        const char *str, uint32_t color, const font_aa_t *font);