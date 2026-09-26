// kernel/gfx/theme.h
#pragma once
#include <stdint.h>

// Colors format: 0xAARRGGBB

// ============================================================================
// AURORA OS — Visual Identity (Windows/macOS style)
// ----------------------------------------------------------------------------
// Base monocroma (grises neutros) + UN solo color de acento (indigo).
// Sin gradientes, sin rainbow, sin tints de color en las superficies.
// ============================================================================

// --- Acento único ---
#define AURORA_ACCENT 0xFF6366F1       // indigo — indicadores, focus, activo
#define AURORA_ACCENT_HOVER 0xFF818CF8 // indigo claro — hover
#define AURORA_ACCENT_DIM 0x556366F1   // indigo translúcido

// --- Escala de grises (base de todo) ---
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

// --- Semánticos para superficies ---
#define AURORA_BG_DARK AURORA_GRAY_950
#define AURORA_MICA_BASE AURORA_GRAY_900
#define AURORA_SURFACE_CARD AURORA_GRAY_800
#define AURORA_WIN_BG 0xF01A1A1A

// --- Ventanas ---
#define AURORA_TITLEBAR_ACTIVE AURORA_GRAY_850
#define AURORA_TITLEBAR_INACTIVE AURORA_GRAY_900
#define AURORA_BORDER_ACTIVE 0x25FFFFFF   /* era 0x5CFFFFFF */
#define AURORA_BORDER_INACTIVE 0x12FFFFFF /* era 0x22FFFFFF */
#define AURORA_TITLEBAR_SEP 0x14FFFFFF

// --- Botones de ventana ---
#define AURORA_BTN_HOVER 0x1AFFFFFF
#define AURORA_BTN_PRESSED 0x0DFFFFFF
#define AURORA_BTN_CLOSE_HOVER 0xFFC42B1C
#define AURORA_BTN_CLOSE_PRESS 0xFFB0241A

// --- Texto ---
#define AURORA_TEXT_PRIMARY AURORA_WHITE
#define AURORA_TEXT_SECONDARY 0xCCFFFFFF
#define AURORA_TEXT_TERTIARY 0x99FFFFFF
#define AURORA_TEXT_DISABLED 0x55FFFFFF

// --- Iconos ---
#define AURORA_ICON_DEFAULT 0xFFE8E8F0
#define AURORA_ICON_INACTIVE 0xFF808090
#define AURORA_ICON_MUTED 0xFF5A5A68

// --- Taskbar ---
#define AURORA_TASKBAR_BG 0xE81A1A1A
#define AURORA_TASKBAR_TOPLIGHT 0x28FFFFFF
#define AURORA_TASKBAR_ITEM_HOVER 0x18FFFFFF
#define AURORA_TASKBAR_ITEM_ACTIVE 0x14FFFFFF
#define AURORA_TASKBAR_INDICATOR AURORA_ACCENT

// --- Geometría ---
#define AURORA_CORNER_RADIUS 8
#define AURORA_CORNER_RADIUS_SM 6
#define AURORA_CORNER_RADIUS_LG 12
#define AURORA_SHADOW_SIZE 8
#define AURORA_TITLEBAR_HEIGHT 32

// --- Animaciones ---
#define WIN_ANIM_OPEN_MS 220
#define WIN_ANIM_CLOSE_MS 160
#define WIN_ANIM_OPEN_DY (-12)
#define WIN_ANIM_CLOSE_DY (-6)

#define TASKBAR_HOVER_ALPHA 0x28
#define TASKBAR_INACTIVE_ALPHA 0x06
#define TASKBAR_ACTIVE_ALPHA 0x14

// ============================================================================
// Aliases legacy
// ============================================================================
#define WIN11_BG_DARK AURORA_BG_DARK
#define WIN11_MICA_BASE AURORA_MICA_BASE
#define WIN11_SURFACE_CARD AURORA_SURFACE_CARD
#define WIN11_WIN_BG AURORA_WIN_BG
#define WIN11_TITLEBAR_ACTIVE AURORA_TITLEBAR_ACTIVE
#define WIN11_TITLEBAR_INACTIVE AURORA_TITLEBAR_INACTIVE
#define WIN11_BORDER_ACTIVE AURORA_BORDER_ACTIVE
#define WIN11_BORDER_INACTIVE AURORA_BORDER_INACTIVE
#define WIN11_BTN_HOVER AURORA_BTN_HOVER
#define WIN11_BTN_CLOSE_HOVER AURORA_BTN_CLOSE_HOVER
#define WIN11_TEXT_PRIMARY AURORA_TEXT_PRIMARY
#define WIN11_TEXT_SECONDARY AURORA_TEXT_SECONDARY
#define WIN11_ACCENT AURORA_ACCENT
#define WIN11_CORNER_RADIUS AURORA_CORNER_RADIUS
#define WIN11_SHADOW_SIZE AURORA_SHADOW_SIZE
#define WIN11_TITLEBAR_HEIGHT AURORA_TITLEBAR_HEIGHT

// Colores heredados (por si algo aún los referencia)
#define AURORA_CYAN AURORA_ACCENT
#define AURORA_BLUE AURORA_ACCENT
#define AURORA_VIOLET AURORA_ACCENT
#define AURORA_MAGENTA AURORA_ACCENT