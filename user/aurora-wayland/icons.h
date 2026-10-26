// user/aurora-wayland/icons.h
//
// Iconos de aplicaciones: busca el icono de una app en los temas instalados
// (Aurora, hicolor, Adwaita; PNG o SVG) y en /usr/share/pixmaps, y lo convierte
// a un BMP 32 bpp con alfa en /tmp/aurora-icons (el formato que el kernel
// carga para el Dock y las ventanas: ASYS_DOCK_PIN / ASYS_WIN_SET_ICON).

#ifndef AW_ICONS_H
#define AW_ICONS_H

#include <stddef.h>
#include <stdint.h>

// Busca el icono por cualquiera de `names` (nombre de tema o ruta absoluta) y
// escribe en `out` la ruta del BMP generado (cacheado por `key`). 1 = hay icono.
int aw_icon_for(const char *key, const char *const *names, int nnames, char *out, size_t n);

// Escribe un BMP con alfa recto (ARGB32 sin premultiplicar, p. ej. _NET_WM_ICON).
int aw_icon_from_argb(const char *key, const uint32_t *argb, int w, int h, char *out, size_t n);

#endif
