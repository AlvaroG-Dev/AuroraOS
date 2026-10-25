#pragma once
#include "../bmp.h"
#include "gfx.h"
#include <stdint.h>

// Taskbar constants (shared with compositor)
#define TASKBAR_BAR_H 48
#define TASKBAR_BAR_PADDING_X 12
#define TASKBAR_BAR_MARGIN_BOTTOM 0

#define TASKBAR_ITEM_H 36
#define TASKBAR_ITEM_MIN_W 40
#define TASKBAR_ITEM_PADDING_X 11
#define TASKBAR_ITEM_GAP 4
#define TASKBAR_ITEM_RADIUS 6

#define TASKBAR_CONTENT_GAP 7
#define TASKBAR_START_ICON_SIZE 16
#define TASKBAR_START_ICON_SQ 7
#define TASKBAR_START_ICON_GAP 2

#define TASKBAR_CLOCK_PADDING_X 14
#define TASKBAR_CLOCK_MIN_W 68
#define TASKBAR_CLOCK_GAP 12
#define TASKBAR_SEPARATOR_W 1

/* Animación de hover */
#define TASKBAR_HOVER_ALPHA 0x28
#define TASKBAR_INACTIVE_ALPHA 0x06
#define TASKBAR_ACTIVE_ALPHA 0x24

typedef enum {
  TASKBAR_ITEM_START_BTN,
  TASKBAR_ITEM_APP_ICON,
  TASKBAR_ITEM_WIDGET,
  TASKBAR_ITEM_TRAY_CLOCK
} taskbar_item_type_t;

typedef struct taskbar_item {
  uint32_t id;
  taskbar_item_type_t type;
  char label[32];
  char icon_symbol[8];
  uint32_t icon_color;
  tar_node_t *icon_bmp_node;
  uint32_t icon_cache[20 * 20];
  int has_icon_cache;
  int is_active;
  int width; // 0 = Automático (calculado por contenido). >0 = Ancho explícito
             // indicado.
  void (*on_click)(struct taskbar_item *item);
  struct taskbar_item *next;

  int hover_t;    // 0..256 (0 = sin hover, 256 = hover completo)
  int is_hovered; // 1 si el ratón está encima

  // Icono del Dock (DOCK_ICON_SRC x DOCK_ICON_SRC, ARGB). Lo crea el Dock
  // bajo demanda desde icon_bmp_node o, si no hay BMP, como baldosa con el
  // símbolo. 0 = sin generar, 1 = listo. Se libera al quitar el item.
  uint32_t *dock_icon;
  int dock_icon_state;

  // Lanzador fijado en el Dock (apps Linux/Wayland): sobrevive a sus ventanas,
  // las ventanas con su app_id cuelgan de él y un clic sin ventanas pide a
  // userspace que lance la app (ver compositor_pop_launch).
  int pinned;
  uint32_t launch_id; /* id que se devuelve a userspace al lanzar */
  char app_id[48];
  int win_count;      /* ventanas que cuelgan de este item */
  int launching;      /* ms restantes de "abriendose" (rebote), 0 = no */
} taskbar_item_t;

// API de abstracción de la Taskbar
void taskbar_init(void);
taskbar_item_t *taskbar_add_item(taskbar_item_type_t type, const char *label,
                                 const char *symbol, uint32_t color,
                                 void (*on_click)(taskbar_item_t *));
taskbar_item_t *taskbar_add_item_custom_width(
    taskbar_item_type_t type, const char *label, const char *symbol,
    uint32_t color, int explicit_width, void (*on_click)(taskbar_item_t *));
void taskbar_remove_item(uint32_t id);
rect_t taskbar_get_bounds(int screen_w, int screen_h);
void taskbar_render(uint32_t *dst, int dst_stride, rect_t clip, int screen_w,
                    int screen_h);
// Actualiza el reloj de la taskbar leyendo del RTC
void taskbar_refresh_clock(void);
// Establece el ítem activo de la taskbar (por puntero)
void taskbar_set_active_item(taskbar_item_t *item);
// Busca el ítem de taskbar bajo las coordenadas (x, y) relativas a la pantalla.
taskbar_item_t *taskbar_hit_test(int screen_x, int screen_y, int screen_w,
                                 int screen_h);
taskbar_item_t *taskbar_add_item_bmp(taskbar_item_type_t type,
                                     const char *label, tar_node_t *bmp_file,
                                     void (*on_click)(taskbar_item_t *));
void taskbar_remove_item_ptr(taskbar_item_t *item);

void taskbar_set_hover(taskbar_item_t *item, int hovered);
int taskbar_tick_hover(int dt_ms); /* cambia void → int */
rect_t taskbar_item_get_bounds(taskbar_item_t *item, int screen_w,
                               int screen_h);
taskbar_item_t *taskbar_get_item_at(int sx, int sy, int sw, int sh);
taskbar_item_t *taskbar_items_head(void);
int taskbar_has_active_hover(void);
// Cadenas del reloj ("HH:MM" y "DD/MM/YYYY"), refrescadas con
// taskbar_refresh_clock(). Las usa la barra de menús.
const char *taskbar_clock_time_str(void);
const char *taskbar_clock_date_str(void);
// Fecha corta de la barra de menús, estilo macOS ("vie 24 oct"). Vacía si el
// RTC no dio una fecha válida.
const char *taskbar_clock_menubar_date_str(void);
