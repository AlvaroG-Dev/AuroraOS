#pragma once
#include <stdint.h>
#include "gfx.h"
#include "../bmp.h"

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
    int is_active;
    int width; // 0 = Automático (calculado por contenido). >0 = Ancho explícito indicado.
    void (*on_click)(struct taskbar_item *item);
    struct taskbar_item *next;
} taskbar_item_t;

// API de abstracción de la Taskbar
void taskbar_init(void);
taskbar_item_t *taskbar_add_item(taskbar_item_type_t type, const char *label, const char *symbol, uint32_t color, void (*on_click)(taskbar_item_t*));
taskbar_item_t *taskbar_add_item_custom_width(taskbar_item_type_t type, const char *label, const char *symbol, uint32_t color, int explicit_width, void (*on_click)(taskbar_item_t*));
void taskbar_remove_item(uint32_t id);
rect_t taskbar_get_bounds(int screen_w, int screen_h);
void taskbar_render(uint32_t *dst, int dst_stride, rect_t clip, int screen_w, int screen_h);
// Actualiza el reloj de la taskbar leyendo del RTC
void taskbar_refresh_clock(void);
// Establece el ítem activo de la taskbar (por puntero)
void taskbar_set_active_item(taskbar_item_t *item);
// Busca el ítem de taskbar bajo las coordenadas (x, y) relativas a la pantalla.
taskbar_item_t *taskbar_hit_test(int screen_x, int screen_y, int screen_w, int screen_h);
taskbar_item_t *taskbar_add_item_bmp(taskbar_item_type_t type, const char *label, tar_node_t *bmp_file, void (*on_click)(taskbar_item_t *));