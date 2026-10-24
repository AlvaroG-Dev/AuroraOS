// kernel/gfx/menubar.h — Barra de menús superior estilo macOS.
//
// Versión 1: orbe de Aurora (con menú del sistema), nombre de la app activa
// y fecha/hora. Los menús por aplicación (Archivo, Edición...) requieren un
// protocolo de menús en winsrv y quedan para una segunda iteración.
// Solo se usa desde el hilo del compositor.
#pragma once
#include "gfx.h"
#include <stdint.h>

#define MENUBAR_H AURORA_MENUBAR_H

typedef enum {
  MENU_ACT_NONE = 0,
  MENU_ACT_ABOUT,
  MENU_ACT_MINIMIZE,
  MENU_ACT_CLOSE
} menu_action_t;

void menubar_init(int screen_w, int screen_h);

// Nombre de la app activa (título de la ventana con foco). NULL o "" =>
// "Aurora".
void menubar_set_app_name(const char *name);

// La hora cambió: repintar la zona derecha.
void menubar_clock_changed(void);

// Si hay algo pendiente de repintar, devuelve 1 y rellena `out`.
int menubar_take_damage(rect_t *out);

// Barra (+ menú desplegable si está abierto). El compositor repinta todo
// ello de una vez para que el desenfoque lea un fondo limpio.
rect_t menubar_get_damage_rect(void);

void menubar_render(uint32_t *dst, int dst_stride, rect_t clip);

int menubar_is_open(void);
void menubar_close(void);

// Hover. Devuelve 1 si el cursor está sobre la barra o el menú abierto.
int menubar_mouse_move(int x, int y);

// Pulsación izquierda. Devuelve 1 si la barra/menú la consumió (el clic no
// debe llegar a las ventanas); *act recibe la acción elegida, si la hay.
int menubar_mouse_press(int x, int y, menu_action_t *act);
