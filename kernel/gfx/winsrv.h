// kernel/gfx/winsrv.h
#ifndef KERNEL_GFX_WINSRV_H
#define KERNEL_GFX_WINSRV_H

#include "window.h"
#include "../sched.h"
#include "../wait.h"
#include <stdint.h>

// Window Server: capa intermedia entre las apps de usuario y el
// compositor. Las apps piden ventanas, hacen blit y reciben eventos.
//
// Un win_id es un índice en la tabla g_windows[]. El puntero a
// window_t vive en el kernel y nunca se expone al usuario.

#define WINSRV_MAX_WINDOWS 32
#define WINSRV_EVENT_QUEUE 64

// Tipos de evento.
#define WINSRV_EV_NONE      0
#define WINSRV_EV_CLOSE     1   // el compositor cerró la ventana
#define WINSRV_EV_FOCUS     2   // la ventana ganó foco
#define WINSRV_EV_BLUR      3   // la ventana perdió foco
#define WINSRV_EV_MOVE      4   // la ventana se movió (x,y nuevas)
#define WINSRV_EV_KEY       5   // tecla con foco: x=scancode, y=pressed
#define WINSRV_EV_MOUSE     6   // click/move en la ventana: x,y locales
#define WINSRV_EV_OUTPUT    7   // byte escrito a stdout: x=byte
#define WINSRV_EV_TTY_INPUT 8   // byte del teclado (ASCII): x=byte

typedef struct winsrv_event {
    uint32_t type;
    int32_t  x;
    int32_t  y;
    uint32_t data;
} winsrv_event_t;

// Struct compartida entre kernel y userland para SYS_WIN_BLIT.
typedef struct {
    int32_t  win_id;
    int32_t  x, y, w, h;
    uint32_t *pixels;
} winsrv_blit_args_t;

void winsrv_init(void);

int winsrv_create_window(task_t *owner, int x, int y, int w, int h,
                         const char *title);
int winsrv_destroy_window(task_t *owner, int win_id);
int winsrv_blit(task_t *owner, int win_id, int x, int y, int w, int h,
                const uint32_t *user_pixels);
int winsrv_poll_event(task_t *owner, int win_id, winsrv_event_t *out_user_ev,
                      int blocking);
int winsrv_register_console(task_t *owner, int win_id);

void winsrv_post_event(window_t *win, uint32_t type, int32_t x, int32_t y,
                       uint32_t data);
void winsrv_console_output(char c);

#endif
