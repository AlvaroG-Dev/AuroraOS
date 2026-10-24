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
void compositor_set_icon_bmp(window_t *win, tar_node_t *bmp_file);

void compositor_thread(void);
void compositor_notify_event(void);
void compositor_notify_clock_tick(void);

// Llamada desde time_tick() (CPU 0, cada tick): despierta al compositor a
// ~62 fps mientras haya animaciones, sin busy-wait ni escanear tareas.
void compositor_frame_tick(void);

// Zona útil del escritorio (sin barra de menús ni Dock).
rect_t compositor_work_area(void);
