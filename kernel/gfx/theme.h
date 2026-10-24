// kernel/gfx/theme.h
#pragma once
#include <stdint.h>

// Colors format: 0xAARRGGBB

// ============================================================================
// AURORA OS — identidad visual (escritorio estilo macOS, modo oscuro)
// ----------------------------------------------------------------------------
// Base de grises neutros + UN acento (índigo). Los únicos colores "vivos" de
// la interfaz son los semáforos de las ventanas y el acento de selección.
// ============================================================================

// --- Acento único ---
#define AURORA_ACCENT 0xFF6366F1
#define AURORA_ACCENT_HOVER 0xFF818CF8
#define AURORA_ACCENT_DIM 0x556366F1

// --- Escala de grises ---
#define AURORA_BLACK 0xFF0A0A0A
#define AURORA_GRAY_950 0xFF141414
#define AURORA_GRAY_900 0xFF1A1A1A
#define AURORA_GRAY_850 0xFF1F1F1F
#define AURORA_GRAY_800 0xFF242424
#define AURORA_GRAY_700 0xFF2E2E2E
#define AURORA_GRAY_600 0xFF3A3A3A
#define AURORA_GRAY_500 0xFF525252
#define AURORA_GRAY_400 0xFF737373
#define AURORA_GRAY_300 0xFFA3A3A3
#define AURORA_GRAY_200 0xFFD4D4D4
#define AURORA_GRAY_100 0xFFE8E8E8
#define AURORA_WHITE 0xFFFFFFFF

// --- Superficies ---
#define AURORA_BG_DARK AURORA_GRAY_950
#define AURORA_MICA_BASE AURORA_GRAY_900
#define AURORA_SURFACE_CARD 0xFF1E1E1E
#define AURORA_WIN_BG 0xFF1E1E1E /* cuerpo de la ventana, opaco */

// --- Ventanas ---
#define AURORA_TITLEBAR_ACTIVE 0xFF2C2C2E
#define AURORA_TITLEBAR_INACTIVE 0xFF252527
#define AURORA_BORDER_ACTIVE 0x33FFFFFF
#define AURORA_BORDER_INACTIVE 0x1AFFFFFF
#define AURORA_TITLEBAR_SEP 0x38000000

// --- Semáforos (izquierda de la barra de título) ---
#define AURORA_TL_CLOSE 0xFFFF5F57
#define AURORA_TL_MINIMIZE 0xFFFEBC2E
#define AURORA_TL_MAXIMIZE 0xFF28C840
#define AURORA_TL_INACTIVE 0xFF4D4D50
#define AURORA_TL_RIM 0x40000000   /* borde fino oscuro */
#define AURORA_TL_GLYPH 0xB0000000 /* ×, −, + al pasar el ratón */

// --- Texto ---
#define AURORA_TEXT_PRIMARY AURORA_WHITE
#define AURORA_TEXT_SECONDARY 0xCCFFFFFF
#define AURORA_TEXT_TERTIARY 0x99FFFFFF
#define AURORA_TEXT_DISABLED 0x55FFFFFF

// --- Iconos ---
#define AURORA_ICON_DEFAULT 0xFFE8E8F0
#define AURORA_ICON_INACTIVE 0xFF808090
#define AURORA_ICON_MUTED 0xFF5A5A68

// --- Barra de menús y Dock (translucidez sobre fondo desenfocado) ---
#define AURORA_MENUBAR_H 28
#define AURORA_MENUBAR_TINT 0x7A141418
#define AURORA_MENUBAR_LINE 0x22FFFFFF
#define AURORA_DOCK_TINT 0x3CFFFFFF
#define AURORA_DOCK_BORDER 0x40FFFFFF
#define AURORA_DOCK_TOPLIGHT 0x30FFFFFF
#define AURORA_MENU_BG 0xB0202024
#define AURORA_MENU_HILITE AURORA_ACCENT

// --- Taskbar (legado: ya no se dibuja; se mantiene el modelo de items) ---
#define AURORA_TASKBAR_BG 0xE81A1A1A
#define AURORA_TASKBAR_TOPLIGHT 0x28FFFFFF
#define AURORA_TASKBAR_ITEM_HOVER 0x18FFFFFF
#define AURORA_TASKBAR_ITEM_ACTIVE 0x14FFFFFF
#define AURORA_TASKBAR_INDICATOR AURORA_ACCENT

// --- Geometría ---
#define AURORA_CORNER_RADIUS 10
#define AURORA_CORNER_RADIUS_SM 6
#define AURORA_CORNER_RADIUS_LG 14
#define AURORA_TITLEBAR_HEIGHT 32

// Sombra: la superficie de la ventana reserva BLUR + OFFSET_Y px por lado.
#define AURORA_SHADOW_BLUR 18
#define AURORA_SHADOW_OFFSET_Y 6
#define AURORA_SHADOW_SIZE (AURORA_SHADOW_BLUR + AURORA_SHADOW_OFFSET_Y)

// --- Animaciones ---
#define WIN_ANIM_OPEN_MS 220
#define WIN_ANIM_CLOSE_MS 160
#define WIN_ANIM_OPEN_DY (-12)
#define WIN_ANIM_CLOSE_DY (-6)

// ============================================================================
// Aliases legacy (código que aún usa WIN11_*; migrar y borrar)
// ============================================================================
#define WIN11_BG_DARK AURORA_BG_DARK
#define WIN11_MICA_BASE AURORA_MICA_BASE
#define WIN11_SURFACE_CARD AURORA_SURFACE_CARD
#define WIN11_WIN_BG AURORA_WIN_BG
#define WIN11_TITLEBAR_ACTIVE AURORA_TITLEBAR_ACTIVE
#define WIN11_TITLEBAR_INACTIVE AURORA_TITLEBAR_INACTIVE
#define WIN11_BORDER_ACTIVE AURORA_BORDER_ACTIVE
#define WIN11_BORDER_INACTIVE AURORA_BORDER_INACTIVE
#define WIN11_TEXT_PRIMARY AURORA_TEXT_PRIMARY
#define WIN11_TEXT_SECONDARY AURORA_TEXT_SECONDARY
#define WIN11_ACCENT AURORA_ACCENT
#define WIN11_CORNER_RADIUS AURORA_CORNER_RADIUS
#define WIN11_SHADOW_SIZE AURORA_SHADOW_SIZE
#define WIN11_TITLEBAR_HEIGHT AURORA_TITLEBAR_HEIGHT

#define AURORA_CYAN AURORA_ACCENT
#define AURORA_BLUE AURORA_ACCENT
#define AURORA_VIOLET AURORA_ACCENT
#define AURORA_MAGENTA AURORA_ACCENT
