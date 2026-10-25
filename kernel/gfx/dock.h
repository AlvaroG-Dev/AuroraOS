// kernel/gfx/dock.h — Dock flotante estilo macOS.
//
// Reutiliza el registro de items de taskbar.c (un item por ventana) pero toda
// la presentación, la geometría y el hit-testing viven aquí. Solo se usa desde
// el hilo del compositor (sin locks).
#pragma once
#include "gfx.h"
#include "taskbar.h"
#include <stdint.h>

#define DOCK_ICON_BASE 56   /* tamaño de reposo del icono */
#define DOCK_ICON_MAX 84    /* tamaño máximo con magnificación (+50 %) */
#define DOCK_ICON_SRC 96    /* lado de la caché de icono (ARGB) */
/* Proporciones: el icono lleva ~2/72 de margen transparente propio, así que
 * el hueco visible entre iconos es DOCK_GAP + ~3 px. El relleno lateral es
 * parecido al superior y el inferior deja sitio al indicador de app abierta. */
#define DOCK_PAD_X 12
#define DOCK_PAD_TOP 8
#define DOCK_PAD_BOTTOM 14
#define DOCK_GAP 6
#define DOCK_MARGIN_BOTTOM 8
#define DOCK_RADIUS 24
#define DOCK_HEIGHT (DOCK_PAD_TOP + DOCK_ICON_BASE + DOCK_PAD_BOTTOM)
#define DOCK_HEADROOM 110    /* espacio sobre el panel: iconos ampliados + tooltip */
#define DOCK_MAX_ITEMS 32
#define DOCK_SEP_GAP 14     /* hueco extra entre lanzadores fijados y ventanas sueltas */

void dock_init(int screen_w, int screen_h);

// Posición del cursor (para magnificación y hover).
void dock_set_cursor(int x, int y);

// Avanza la animación y recalcula la geometría. Devuelve 1 si sigue
// animando (el compositor debe seguir pidiendo frames).
int dock_update(int dt_ms);

// Fuerza recálculo y repintado (item añadido/quitado, icono nuevo, foco).
void dock_invalidate_layout(void);

// Si la geometría cambió desde la última llamada, devuelve 1 y rellena `out`
// con el rectángulo a repintar (unión de lo anterior y lo nuevo).
int dock_take_damage(rect_t *out);

// Rectángulo máximo que el Dock puede ocupar ahora (panel + iconos ampliados
// + tooltip). El compositor repinta todo ello de una vez para que el
// desenfoque lea un fondo limpio.
rect_t dock_get_damage_rect(void);

rect_t dock_get_panel_rect(void);

// Paneles de cristal visibles ahora (panel y tooltip). Devuelve cuántos.
int dock_glass_rects(glass_rect_t *out, int max);

// Rectángulo del icono de `item` (para el efecto genie). 0 si no está.
int dock_item_rect(const taskbar_item_t *item, rect_t *out);

// Icono pulsado (se oscurece mientras el botón está abajo). NULL = ninguno.
void dock_set_pressed(taskbar_item_t *item);

// Rebote de "app abriéndose", como macOS al lanzar una aplicación.
void dock_bounce(taskbar_item_t *item);

// Un item va a desaparecer: olvidar cualquier referencia a él.
void dock_forget(taskbar_item_t *item);
void dock_render(uint32_t *dst, int dst_stride, rect_t clip);

// Item bajo (x, y) o NULL. Solo cuenta lo que se ve: el panel y los iconos.
taskbar_item_t *dock_hit_test(int x, int y);

// Alto que el Dock resta a la zona útil del escritorio.
int dock_reserved_height(void);
