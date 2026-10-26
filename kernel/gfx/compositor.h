// kernel/gfx/compositor.h
#pragma once
#include "gfx.h"
#include "tarfs.h"
#include "window.h"
#include <stdint.h>

void compositor_init(void);

// Seguras desde cualquier contexto/CPU.
void compositor_invalidate_rect(rect_t damage);
void compositor_invalidate_dock(void);

// Modelo de propiedad: SOLO el hilo del compositor modifica la lista de
// ventanas y los items de la taskbar/Dock. Las funciones siguientes pueden
// llamarse desde cualquier tarea: encolan un comando que el compositor
// ejecuta en orden (FIFO). NO llamarlas desde el propio hilo del compositor.
window_t *compositor_create_window(int x, int y, int w, int h,
                                   const char *title, uint32_t flags);
void compositor_close_window(window_t *win);
void compositor_minimize_window(window_t *win);
void compositor_focus_window(window_t *win);
// Maximiza (1) o restaura (0) la ventana; no-op si ya esta en ese estado.
void compositor_set_maximized(window_t *win, int maximized);
void compositor_set_icon_bmp(window_t *win, tar_node_t *bmp_file);
// Cambia el titulo de la ventana (barra de titulo, Dock y menubar). Asincrono.
void compositor_set_title(window_t *win, const char *title);

// Dock: fija un lanzador (app Linux). Al hacer clic sin ventanas abiertas se
// encola `launch_id` (ver compositor_pop_launch). Asincrono.
void compositor_dock_pin(const char *name, const char *app_id,
                         const char *symbol, uint32_t color, uint32_t launch_id,
                         tar_node_t *icon);
// La ventana pertenece a `app_id`: cuelga del lanzador fijado de esa app.
void compositor_set_app_id(window_t *win, const char *app_id);
// Cursor (cursor_shape_t) que se muestra sobre el area de cliente de `win`.
void compositor_set_cursor(window_t *win, int shape);
/* El cliente pide mover (edges=0) o redimensionar (RESIZE_EDGE_*) su ventana. */
void compositor_begin_grab(window_t *win, int edges);
/* El cliente pide un tamano exterior (con barra de titulo) distinto. */
void compositor_request_resize(window_t *win, int w, int h);
// Siguiente peticion de lanzamiento pendiente, o 0 si no hay.
uint32_t compositor_pop_launch(void);

void compositor_thread(void);
void compositor_notify_event(void);
void compositor_notify_clock_tick(void);

// Llamada desde time_tick() (CPU 0, cada tick): despierta al compositor a
// ~62 fps mientras haya animaciones, sin busy-wait ni escanear tareas.
void compositor_frame_tick(void);

// Zona útil del escritorio (sin barra de menús ni Dock).
rect_t compositor_work_area(void);
