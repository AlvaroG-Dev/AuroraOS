// kernel/gfx/window.h
#pragma once
#include "../spinlock.h"
#include "../tarfs.h"
#include "gfx.h"
#include "taskbar.h"
#include <stdint.h>

#define WIN_FLAGS_FOCUSED (1 << 0)
#define WIN_FLAGS_INACTIVE (1 << 1)
#define WIN_FLAGS_NODECORATION (1 << 2)
#define WIN_FLAGS_MOVABLE (1 << 3)
#define WIN_FLAGS_HIDDEN (1 << 4)

#define ALIGN_LEFT (1 << 0)
#define ALIGN_RIGHT (1 << 1)
#define ALIGN_CENTER_H (1 << 2)
#define ALIGN_TOP (1 << 3)
#define ALIGN_BOTTOM (1 << 4)
#define ALIGN_CENTER_V (1 << 5)

#define WIN_ICON_SIZE 18

/* Geometría de los semáforos (relativa a la esquina sup-izq. de la ventana).
 * Toda la geometría de la decoración vive aquí y en window_hit_test(): el
 * compositor ya no contiene números mágicos de la barra de título. */
#define WIN_TL_DIAM 12  /* diámetro del círculo */
#define WIN_TL_PITCH 20 /* distancia entre centros... de borde a borde */
#define WIN_TL_X 14     /* x del borde izquierdo del primer círculo */
#define WIN_TL_HIT 18   /* lado de la zona de clic de cada botón */
#define WIN_RESIZE_BORDER 6

typedef enum {
  WIN_BTN_NONE = 0,
  WIN_BTN_CLOSE_PRESSED,
  WIN_BTN_MAXIMIZE_PRESSED,
  WIN_BTN_MINIMIZE_PRESSED
} window_btn_state_t;

typedef enum {
  WIN_HIT_NONE = 0, /* fuera de la ventana */
  WIN_HIT_CLIENT,
  WIN_HIT_TITLEBAR,
  WIN_HIT_CLOSE,
  WIN_HIT_MINIMIZE,
  WIN_HIT_MAXIMIZE,
  WIN_HIT_RESIZE_R,
  WIN_HIT_RESIZE_B,
  WIN_HIT_RESIZE_BR
} win_hit_t;

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
  int dirty;         // 1 = hay que redibujar TODO (sombra, chrome, contenido)
  int content_dirty; // 1 = solo cambió el contenido (camino barato)

  uint32_t flags;
  int z_order;

  char title[64];
  tar_node_t *icon_bmp_node;
  uint32_t icon_cache[WIN_ICON_SIZE * WIN_ICON_SIZE]; /* legado, sin usar */
  int has_icon_cache; /* 1 = la ventana tiene icono BMP asignado */
  char icon_symbol[8];
  uint32_t icon_bg_color;
  const uint32_t *icon_buffer;
  taskbar_item_t *taskbar_item;

  uint32_t *content_buffer;
  // Protege el intercambio de content_buffer/content_w/h en window_resize
  // frente a winsrv_blit (que corre en otra CPU). Orden de locks:
  // winsrv_lock -> content_lock.
  spinlock_t content_lock;

  struct window *next;
  struct window *prev;

  /* Estado de animación */
  int anim_state;      // 0=none, 1=opening, 2=closing
  int anim_t;          // 0..256 (progreso)
  int anim_alpha;      // 0..255
  int anim_dy;         // offset vertical durante animación
  int pending_destroy; // 1 = al terminar anim, destruir

  /* Hover sobre los semáforos. hover_controls = el cursor está sobre el
   * grupo (muestra los glifos). hover_btn: 0 ninguno, 1 min, 2 max, 3 cerrar */
  int hover_btn;
  int hover_controls;

  /* Maximizado / restauración */
  int maximized;
  int restore_x, restore_y, restore_w, restore_h;
} window_t;

window_t *window_create(int x, int y, int w, int h, const char *title,
                        uint32_t flags);
void window_destroy(window_t *win);
void window_set_visible(window_t *win, int visible);
int window_is_visible(window_t *win);

// Alto de la barra de título (0 si NODECORATION).
int window_titlebar_h(const window_t *win);
// Zona de la ventana bajo (sx, sy) en coordenadas de pantalla.
win_hit_t window_hit_test(const window_t *win, int sx, int sy);
// 1 si (sx, sy) está sobre el grupo de semáforos.
int window_in_controls(const window_t *win, int sx, int sy);

// Dibuja la ventana y sus marcos dentro de win->surface
void window_redraw_surface(window_t *win);

// Copia veloz del surface al destino (Backbuffer del Compositor)
void window_render_frame(window_t *win, uint32_t *dst, int dst_stride,
                         rect_t clip);

void win_set_icon_bmp(window_t *win, tar_node_t *bmp_file);
void win_set_icon_text(window_t *win, const char *symbol, uint32_t bg_color);
void win_set_icon_image(window_t *win, const uint32_t *icon_18x18);
void win_clear(window_t *win, uint32_t color);
void win_draw_rect(window_t *win, int x, int y, int w, int h, uint32_t color);
void win_draw_rounded_rect(window_t *win, int x, int y, int w, int h,
                           int radius, uint32_t color);
void win_draw_string(window_t *win, int x, int y, const char *str,
                     uint32_t color, font_id_t font);
void win_draw_string_aligned(window_t *win, rect_t rect, const char *str,
                             uint32_t color, font_id_t font, uint32_t align);
void win_draw_image(window_t *win, int x, int y, int w, int h,
                    const uint32_t *img_data);
void win_draw_image_scaled(window_t *win, int dst_x, int dst_y, int dst_w,
                           int dst_h, const uint32_t *img_data, int src_w,
                           int src_h);
void win_draw_image_full(window_t *win, const uint32_t *img_data, int src_w,
                         int src_h);
void win_update(window_t *win);

/* Animaciones */
#define WIN_ANIM_NONE 0
#define WIN_ANIM_OPENING 1
#define WIN_ANIM_CLOSING 2
#define WIN_ANIM_MINIMIZING 3

void window_start_open_animation(window_t *win);
void window_start_close_animation(window_t *win);
void window_advance_animation(window_t *win, int dt_ms);
int window_is_animating(window_t *win);
void window_start_minimize_animation(window_t *win);

// Resize de la ventana. Realloca content_buffer y surface. Solo debe
// llamarse desde el hilo del compositor. No emite WINSRV_EV_RESIZE.
void window_resize(window_t *win, int new_w, int new_h);
