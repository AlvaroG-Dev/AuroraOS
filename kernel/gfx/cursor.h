// kernel/gfx/cursor.h — Cursores estilo macOS (flecha, redimensionado, mano, texto...).
//
// Se generan una vez en cursor_init() a partir de polígonos, con
// antialiasing (supermuestreo 4x4), borde blanco y sombra difusa, como
// imágenes ARGB no premultiplicadas listas para mezclar sobre el backbuffer.
#pragma once
#include <stdint.h>

#define CURSOR_IMG 32 /* lado de la imagen de cada cursor */

typedef enum {
  CURSOR_ARROW = 0,
  CURSOR_RESIZE_EW,   /* ↔ bordes izquierdo/derecho */
  CURSOR_RESIZE_NS,   /* ↕ bordes superior/inferior */
  CURSOR_RESIZE_NWSE, /* ⤡ esquinas sup-izq / inf-der */
  CURSOR_RESIZE_NESW, /* ⤢ esquinas sup-der / inf-izq */
  CURSOR_POINTER,     /* mano con el índice (enlaces, botones) */
  CURSOR_NOT_ALLOWED, /* círculo tachado */
  CURSOR_TEXT,        /* I-beam */
  CURSOR_GRABBING,    /* mano cerrada (arrastre) */
  CURSOR_WAIT,        /* reloj de arena */
  CURSOR_COUNT
} cursor_shape_t;

typedef struct {
  int w, h;
  int hot_x, hot_y; /* punto activo dentro de la imagen */
  uint32_t px[CURSOR_IMG * CURSOR_IMG];
} cursor_img_t;

void cursor_init(void);
const cursor_img_t *cursor_get(cursor_shape_t shape);
