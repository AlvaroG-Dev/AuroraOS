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
#define WINSRV_EV_RESIZE 9
// Puntero "crudo" (solo ventanas con WIN_FLAGS_RAW_INPUT, ver
// winsrv_set_raw_input): x,y = posicion en el contenido de la ventana,
// data = botones pulsados (bit0 izq, bit1 der, bit2 medio). Se emite con
// cada movimiento o cambio de botones sobre el area de cliente (y mientras
// haya un boton pulsado, aunque el cursor salga). PTR_LEAVE: el cursor dejo
// la ventana.
#define WINSRV_EV_PTR 10
#define WINSRV_EV_PTR_LEAVE 11
// Cambio de estado de la ventana hecho por Aurora: x = 1 maximizada, 0 no.
#define WINSRV_EV_STATE 12

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
// Variante con flags (WIN_FLAGS_NODECORATION/FIXED_SIZE/POPUP/RAW_INPUT). Con
// NODECORATION, w x h es el tamano del contenido (sin barra de titulo).
int winsrv_create_window_ex(task_t *owner, int x, int y, int w, int h,
                            const char *title, uint32_t flags);
int winsrv_destroy_window(task_t *owner, int win_id);
int winsrv_blit(task_t *owner, int win_id, int x, int y, int w, int h,
                int src_x, int src_y, int src_stride,
                const uint32_t *user_pixels);
int winsrv_poll_event(task_t *owner, int win_id, winsrv_event_t *out_user_ev,
                      int blocking);
int winsrv_register_console(task_t *owner, int win_id);
// Pide recibir WINSRV_EV_PTR (movimiento/botones sin filtrar) en esta ventana.
int winsrv_set_raw_input(task_t *owner, int win_id, int enable);

void winsrv_post_event(window_t *win, uint32_t type, int32_t x, int32_t y,
                       uint32_t data);
void winsrv_console_output(char c);

int winsrv_has_window(window_t *win);

void winsrv_cleanup_task(task_t *owner);

int winsrv_set_icon(task_t *owner, int win_id, const char *path);
// `title` es un puntero de KERNEL (el syscall ya lo copio).
int winsrv_set_title(task_t *owner, int win_id, const char *title);
// Estado pedido por el dueno de la ventana (ASYS_WIN_SET_STATE).
#define WINSRV_STATE_MINIMIZE 1
#define WINSRV_STATE_MAXIMIZE 2
#define WINSRV_STATE_RESTORE 3
int winsrv_set_state(task_t *owner, int win_id, int state);
// app_id de la ventana (ASYS_WIN_SET_APP_ID): la agrupa en el Dock.
int winsrv_set_app_id(task_t *owner, int win_id, const char *app_id);
// Forma del cursor (cursor_shape_t) sobre el area de cliente de la ventana
// (ASYS_WIN_SET_CURSOR). Formas desconocidas -> -1.
int winsrv_set_cursor(task_t *owner, int win_id, int shape);
// Inicia un arrastre (edges=0) o redimension (bits RESIZE_EDGE_*: 1 der, 2 inf,
// 4 izq, 8 sup) interactivo; solo vale con el boton izquierdo pulsado.
int winsrv_begin_grab(task_t *owner, int win_id, int edges);
// Pide un tamano exterior w x h (ASYS_WIN_RESIZE); el cliente recibe WINSRV_EV_RESIZE.
int winsrv_request_resize(task_t *owner, int win_id, int w, int h);
// Carga un BMP del VFS como nodo permanente (icono del Dock); NULL si falla.
tar_node_t *winsrv_load_icon_node(const char *path);

// Postea un evento a la ventana que tiene el foco. Si no hay ninguna
// enfocada, no hace nada. Se usa para input de teclado: el TTY recibe
// los bytes del PS/2 y los enruta al terminal que tiene el foco.
void winsrv_post_to_focused(uint32_t type, int32_t x, int32_t y, uint32_t data);

#endif