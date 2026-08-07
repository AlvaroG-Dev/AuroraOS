// kernel/gfx/window.h
#pragma once
#include <stdint.h>
#include "gfx.h"
#include "taskbar.h"
#include "../tarfs.h" // <<-- 1. Añadido para reconocer tar_node_t

#define WIN_FLAGS_FOCUSED      (1 << 0)
#define WIN_FLAGS_INACTIVE     (1 << 1)
#define WIN_FLAGS_NODECORATION (1 << 2)
#define WIN_FLAGS_MOVABLE      (1 << 3)
#define WIN_FLAGS_HIDDEN       (1 << 4)

#define ALIGN_LEFT       (1 << 0)
#define ALIGN_RIGHT      (1 << 1)
#define ALIGN_CENTER_H   (1 << 2)
#define ALIGN_TOP        (1 << 3)
#define ALIGN_BOTTOM     (1 << 4)
#define ALIGN_CENTER_V   (1 << 5)

typedef enum {
    WIN_BTN_NONE = 0,
    WIN_BTN_CLOSE_PRESSED,
    WIN_BTN_MAXIMIZE_PRESSED,
    WIN_BTN_MINIMIZE_PRESSED
} window_btn_state_t;

typedef struct window {
  int x;
  int y;
  int width;
  int height;
  int content_w;
  int content_h;
  
  // Dimensiones totales del Surface (incluyendo sombras)
  int surface_w;
  int surface_h;
  uint32_t *surface; // Buffer offscreen precacheado
  int dirty;         // 1 = Necesita redibujar su surface privada

  uint32_t flags;
  int z_order;

  char title[64];
  tar_node_t *icon_bmp_node;
  char icon_symbol[8];
  uint32_t icon_bg_color;
  const uint32_t *icon_buffer;
  taskbar_item_t *taskbar_item;

  uint32_t *content_buffer;

  struct window *next;
  struct window *prev;
} window_t;

window_t *window_create(int x, int y, int w, int h, const char *title, uint32_t flags);
void window_destroy(window_t *win);
void window_set_visible(window_t *win, int visible);
int window_is_visible(window_t *win);

// Dibuja la ventana y sus marcos dentro de win->surface
void window_redraw_surface(window_t *win);

// Copia veloz del surface al destino (Backbuffer del Compositor)
void window_render_frame(window_t *win, uint32_t *dst, int dst_stride, rect_t clip);

// <<-- 2. Declaración añadida
void win_set_icon_bmp(window_t *win, tar_node_t *bmp_file);

void win_set_icon_text(window_t *win, const char *symbol, uint32_t bg_color);
void win_set_icon_image(window_t *win, const uint32_t *icon_18x18);
void win_clear(window_t *win, uint32_t color);
void win_draw_rect(window_t *win, int x, int y, int w, int h, uint32_t color);
void win_draw_rounded_rect(window_t *win, int x, int y, int w, int h, int radius, uint32_t color);
void win_draw_string(window_t *win, int x, int y, const char *str, uint32_t color, font_id_t font);
void win_draw_string_aligned(window_t *win, rect_t rect, const char *str, uint32_t color, font_id_t font, uint32_t align);
void win_draw_image(window_t *win, int x, int y, int w, int h, const uint32_t *img_data);
void win_draw_image_scaled(window_t *win, int dst_x, int dst_y, int dst_w, int dst_h, const uint32_t *img_data, int src_w, int src_h);
void win_draw_image_full(window_t *win, const uint32_t *img_data, int src_w, int src_h);
void win_update(window_t *win);