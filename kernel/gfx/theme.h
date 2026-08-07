// kernel/gfx/theme.h
#pragma once
#include <stdint.h>

// Colors format: 0xAARRGGBB

// Desktop & Background
#define WIN11_BG_DARK 0xFF0A0A10
#define WIN11_MICA_BASE 0xFF202020
#define WIN11_SURFACE_CARD 0xFF2C2C2C

// Windows & Borders
#define WIN11_WIN_BG 0xF01C1C1C // Translucidez tipo Acrylic/Mica
#define WIN11_TITLEBAR_ACTIVE 0xFF202020
#define WIN11_TITLEBAR_INACTIVE 0xFF181818
#define WIN11_BORDER_ACTIVE 0x40FFFFFF // Borde blanco sutil con alpha
#define WIN11_BORDER_INACTIVE 0x1AFFFFFF

// Controls
#define WIN11_BTN_HOVER 0x1AFFFFFF
#define WIN11_BTN_CLOSE_HOVER 0xE6E81123 // Rojo característico de cerrar
#define WIN11_TEXT_PRIMARY 0xFFFFFFFF
#define WIN11_TEXT_SECONDARY 0x99FFFFFF
#define WIN11_ACCENT 0xFF0078D4 // Azul Windows 11 Accent

// Geometry
#define WIN11_CORNER_RADIUS 8
#define WIN11_SHADOW_SIZE 12
#define WIN11_TITLEBAR_HEIGHT 32