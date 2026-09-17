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
#define WINSRV_EVENT_QUEUE 32

// Tipos de evento.
#define WINSRV_EV_NONE    0
#define WINSRV_EV_CLOSE   1   // el compositor cerró la ventana
#define WINSRV_EV_FOCUS   2   // la ventana ganó foco
#define WINSRV_EV_BLUR    3   // la ventana perdió foco
#define WINSRV_EV_MOVE    4   // la ventana se movió (x,y nuevas)
#define WINSRV_EV_KEY     5   // tecla con foco: x=scancode, y=pressed
#define WINSRV_EV_MOUSE   6   // click/move en la ventana: x,y locales
#define WINSRV_EV_OUTPUT  7   // byte escrito a stdout: x=byte

typedef struct winsrv_event {
    uint32_t type;
    int32_t  x;
    int32_t  y;
    uint32_t data;
} winsrv_event_t;

// Inicializa el winsrv. Llamar desde compositor_init tras crear el
// backbuffer.
void winsrv_init(void);

// --- API para las syscalls ---

// Crea una ventana propiedad de 'owner'. Devuelve win_id >= 0, o -1.
int winsrv_create_window(task_t *owner, int x, int y, int w, int h,
                         const char *title);

// Destruye la ventana. Si era la consola registrada, se desregistra.
// Devuelve 0 si OK, -1 si no existe o no es del owner.
int winsrv_destroy_window(task_t *owner, int win_id);

// Copia píxeles al content_buffer de la ventana. 'pixels' es un buffer
// de userland (w*h uint32 en formato 0xAARRGGBB, fila a fila).
int winsrv_blit(task_t *owner, int win_id, int x, int y, int w, int h,
                const uint32_t *user_pixels);

// Espera un evento. Si blocking=0 y no hay eventos, devuelve 0.
// Si hay evento, rellena *out_user_ev y devuelve 1.
// Si error, devuelve -1.
int winsrv_poll_event(task_t *owner, int win_id, winsrv_event_t *out_user_ev,
                      int blocking);

// Registra la ventana como consola del sistema. Solo una a la vez.
// Devuelve 0 si OK, -1 si error.
int winsrv_register_console(task_t *owner, int win_id);

// --- API para el resto del kernel ---

// Publica un evento a la ventana. Se llama desde el compositor y desde
// el VFS (para output). No bloquea.
void winsrv_post_event(window_t *win, uint32_t type, int32_t x, int32_t y,
                       uint32_t data);

// Envía un byte de stdout a la consola registrada (si hay). Llamado
// desde console_vfs_write en vfs.c.
void winsrv_console_output(char c);

#endif
