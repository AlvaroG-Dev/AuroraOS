// kernel/gfx/menubar.h — Barra de menús superior estilo macOS Tahoe.
//
// Transparente (solo texto sobre el fondo, con sombra). Menús: Aurora, el de
// la app activa, Archivo, Ventana (con la lista de ventanas) y Ayuda. Todas
// las entradas hacen algo real; Edición/Ver llegarán con el protocolo de
// menús por aplicación. Solo se usa desde el hilo del compositor.
#pragma once
#include "gfx.h"
#include <stdint.h>

#define MENUBAR_H AURORA_MENUBAR_H

typedef enum {
  MENU_ACT_NONE = 0,
  MENU_ACT_ABOUT,
  MENU_ACT_MINIMIZE,
  MENU_ACT_ZOOM,
  MENU_ACT_CLOSE,
  MENU_ACT_FOCUS_BASE = 100 /* + índice de ventana */
} menu_action_t;

// Datos dinámicos que el compositor aporta (ventanas abiertas y foco).
typedef struct {
  int (*window_count)(void);
  const char *(*window_title)(int i);
  int (*window_is_focused)(int i);
  int (*has_focused)(void);
} menubar_provider_t;

void menubar_init(int screen_w, int screen_h);
void menubar_set_provider(const menubar_provider_t *p);

// Nombre de la app activa (título de la ventana con foco). NULL o "" =>
// "Aurora".
void menubar_set_app_name(const char *name);

// La hora cambió: repintar la barra.
void menubar_clock_changed(void);

// Si hay algo pendiente de repintar, devuelve 1 y rellena `out`.
int menubar_take_damage(rect_t *out);

// Barra (+ menú desplegable si está abierto). El compositor repinta todo
// ello de una vez para que el desenfoque lea un fondo limpio.
rect_t menubar_get_damage_rect(void);

void menubar_render(uint32_t *dst, int dst_stride, rect_t clip);

// Paneles de cristal visibles (el menú desplegable abierto). Devuelve cuántos.
int menubar_glass_rects(glass_rect_t *out, int max);

int menubar_is_open(void);
void menubar_close(void);

// Hover. Devuelve 1 si el cursor está sobre la barra o el menú abierto.
int menubar_mouse_move(int x, int y);

// Pulsación izquierda. Devuelve 1 si la barra/menú la consumió (el clic no
// debe llegar a las ventanas); *act recibe la acción elegida, si la hay.
int menubar_mouse_press(int x, int y, menu_action_t *act);
