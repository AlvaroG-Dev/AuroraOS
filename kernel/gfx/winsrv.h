// kernel/gfx/winsrv.h
#ifndef KERNEL_GFX_WINSRV_H
#define KERNEL_GFX_WINSRV_H

#include "../sched.h"
#include "../wait.h"
#include "window.h"
#include <stdint.h>

#define WINSRV_MAX_WINDOWS 32
#define WINSRV_EVENT_QUEUE 2048

// Tipos de evento.
#define WINSRV_EV_NONE 0
#define WINSRV_EV_CLOSE 1
#define WINSRV_EV_FOCUS 2
#define WINSRV_EV_BLUR 3
#define WINSRV_EV_MOVE 4
#define WINSRV_EV_KEY 5
#define WINSRV_EV_MOUSE 6
#define WINSRV_EV_OUTPUT 7
#define WINSRV_EV_TTY_INPUT 8

#define WINSRV_USER_LIMIT 0x0000800000000000ULL

typedef struct winsrv_event {
  uint32_t type;
  int32_t x;
  int32_t y;
  uint32_t data;
} winsrv_event_t;

// Struct compartida entre kernel y userland para SYS_WIN_BLIT.
//
// Semántica:
//   - x, y, w, h   : rectángulo DESTINO dentro de la ventana.
//   - src_x, src_y : offset dentro del buffer fuente donde empieza el
//                    rectángulo a copiar. Normalmente coincide con x,y
//                    si el buffer fuente es la ventana entera.
//   - src_stride   : ancho del buffer fuente en píxeles (para calcular
//                    filas del buffer).
//   - pixels       : puntero al INICIO del buffer fuente (NO al
//                    principio del rectángulo).
typedef struct {
  int32_t win_id;
  int32_t x, y, w, h;
  int32_t src_x, src_y, src_stride;
  uint32_t *pixels;
} winsrv_blit_args_t;

void winsrv_init(void);

int winsrv_create_window(task_t *owner, int x, int y, int w, int h,
                         const char *title);
int winsrv_destroy_window(task_t *owner, int win_id);
int winsrv_blit(task_t *owner, int win_id, int x, int y, int w, int h,
                int src_x, int src_y, int src_stride,
                const uint32_t *user_pixels);
int winsrv_poll_event(task_t *owner, int win_id, winsrv_event_t *out_user_ev,
                      int blocking);
int winsrv_register_console(task_t *owner, int win_id);

void winsrv_post_event(window_t *win, uint32_t type, int32_t x, int32_t y,
                       uint32_t data);
void winsrv_console_output(char c);

int winsrv_has_window(window_t *win);

void winsrv_cleanup_task(task_t *owner);

#endif