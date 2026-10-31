// kernel/gfx/brand.h — emblema de AuroraOS embebido (sin depender del initrd)
#pragma once
#include "gfx.h"

/* Dibuja el emblema centrado en (cx, cy) con altura `h` px. */
void brand_draw_emblem(uint32_t *dst, int stride, rect_t clip, int cx, int cy,
                       int h);
/* Proporción ancho/alto del emblema, en 1/256. */
int brand_emblem_aspect256(void);

/* Wordmark "aurora OS" (blanco) centrado en cx, con arriba en `top` y ancho w. */
void brand_draw_wordmark(uint32_t *dst, int stride, rect_t clip, int cx, int top,
                         int w);
