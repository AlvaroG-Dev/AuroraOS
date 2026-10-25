// kernel/gfx/about.h — Ventana "Acerca de AuroraOS" (estilo "Acerca de este
// Mac"): logo, nombre y los datos reales del equipo. La crea y gestiona el
// hilo del compositor; esto solo define su tamaño y pinta su contenido.
#pragma once
#include "window.h"

#define ABOUT_W 340
#define ABOUT_H 430 /* incluida la barra de título */

// Pinta el contenido de la ventana (content_buffer) y lo marca como cambiado.
void about_paint(window_t *win);
