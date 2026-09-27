// user/lib/font_aa.h
#pragma once
#include <stdint.h>

// Glifo antialiased: 8 bits por píxel (0 = transparente, 255 = opaco).
// `bearing_x` es el desplazamiento horizontal desde el origen del
// cursor. `bearing_y` es la distancia desde el FONDO de la celda hasta
// el FONDO del glyph (cuenta hacia arriba: para una 'g' con descendente
// vale menos que para una 'A' que apoya en la baseline).
typedef struct {
  uint8_t width;
  uint8_t height;
  int8_t bearing_x;
  int8_t bearing_y;
  uint8_t advance;
  const uint8_t *bitmap;
} glyph_aa_t;

typedef struct {
  uint8_t height;
  glyph_aa_t glyphs[128];
} font_aa_t;