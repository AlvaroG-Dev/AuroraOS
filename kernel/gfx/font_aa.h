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
  uint8_t height; // En la práctica: distancia de la línea superior a la
                  // línea base (ascent). baseline = y + height.
  glyph_aa_t glyphs[128]; // Mapeo ASCII básico
} font_aa_t;

// Dibuja una cadena UTF-8. Los caracteres Latin-1 habituales en español
// (á é í ó ú ü ñ ¿ ¡ y variantes) se componen en tiempo de ejecución a
// partir de glifos ASCII hasta que los headers de fuente se regeneren con
// el rango U+00A0..U+00FF.
void gfx_draw_string_aa(uint32_t *dst, int stride, rect_t clip, int x, int y,
                        const char *str, uint32_t color, const font_aa_t *font);

// Ancho en píxeles de una cadena UTF-8 (suma de avances).
int font_aa_text_width(const font_aa_t *font, const char *str);

// Altura de las mayúsculas (bearing_y de 'H').
int font_aa_cap_height(const font_aa_t *font);
