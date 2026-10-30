// kernel/gfx/theme.h
#pragma once
#include <stdint.h>

// Colors format: 0xAARRGGBB

// ============================================================================
// AURORA OS — identidad visual (escritorio estilo macOS Tahoe, modo oscuro)
// ----------------------------------------------------------------------------
// Base de grises neutros + UN acento (azul de sistema). Los únicos colores
// "vivos" son los semáforos de las ventanas y el acento de selección. Los
// paneles flotantes (Dock, menús, tooltips) usan "cristal": fondo desenfocado
// y saturado + tinte + borde especular (ver gfx_glass_panel).
// Para cambiar el color de marca basta con cambiar AURORA_ACCENT*.
// ============================================================================

// --- Acento único ---
#define AURORA_ACCENT 0xFF54DA75 /* verde aurora (docs/Diseno_Identidad_Grafica.md) */
#define AURORA_ACCENT_HOVER 0xFF7BE594
#define AURORA_ACCENT_DIM 0x5554DA75
#define AURORA_ACCENT_TEXT 0xFF08260F /* texto sobre el acento */

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
#define AURORA_TITLEBAR_ACTIVE 0xFF262628
#define AURORA_TITLEBAR_INACTIVE 0xFF212123
#define AURORA_BORDER_ACTIVE 0x2EFFFFFF
#define AURORA_BORDER_INACTIVE 0x1AFFFFFF
#define AURORA_TITLEBAR_SEP 0x00000000 /* Tahoe: sin separador */

#define AURORA_TITLEBAR_HAIRLINE 0x66000000 /* separación barra/contenido */

// --- Semáforos (izquierda de la barra de título) ---
#define AURORA_TL_CLOSE 0xFFFF6A3D
#define AURORA_TL_MINIMIZE 0xFFFFD23F
#define AURORA_TL_MAXIMIZE 0xFF3CC8E0
#define AURORA_TL_INACTIVE 0xFF4A4A4D
#define AURORA_TL_RIM 0x33000000   /* borde fino oscuro */
/* Glifos (×, −, flechas de zoom) al pasar el ratón: tono oscuro de cada
 * botón, como en macOS, en vez de un negro genérico. */
#define AURORA_TL_GLYPH_CLOSE 0xE06B2410
#define AURORA_TL_GLYPH_MINIMIZE 0xE0805A00
#define AURORA_TL_GLYPH_MAXIMIZE 0xE0064C5A
#define AURORA_TL_PRESSED 0x40000000 /* oscurecido mientras se pulsa */

// --- Texto ---
#define AURORA_TEXT_PRIMARY AURORA_WHITE
#define AURORA_TEXT_SECONDARY 0xCCFFFFFF
#define AURORA_TEXT_TERTIARY 0x99FFFFFF
#define AURORA_TEXT_DISABLED 0x55FFFFFF

// --- Iconos ---
#define AURORA_ICON_DEFAULT 0xFFE8E8F0
#define AURORA_ICON_INACTIVE 0xFF808090
#define AURORA_ICON_MUTED 0xFF5A5A68

// --- Barra de menús: transparente (Tahoe). Solo un velo muy tenue arriba para
// que el texto blanco se lea sobre fondos claros. ---
#define AURORA_MENUBAR_H 28
#define AURORA_MENUBAR_SCRIM 0x40000000   /* alpha arriba, se desvanece a 0 */
#define AURORA_MENUBAR_TEXT_SHADOW 0x70000000
#define AURORA_MENUBAR_PILL 0x38FFFFFF    /* título resaltado (menú abierto) */

// --- Cristal líquido ---
#define AURORA_GLASS_BLUR 12              /* radio del box-blur (2 pasadas) */
#define AURORA_GLASS_SAT 300              /* saturación del fondo, 256 = igual */
#define AURORA_DOCK_TINT 0x3CFFFFFF
#define AURORA_MENU_TINT 0x9A1E1E22
#define AURORA_TOOLTIP_TINT 0xB0202024
#define AURORA_MENU_BG AURORA_MENU_TINT
#define AURORA_MENU_HILITE AURORA_ACCENT

// --- Taskbar (legado: ya no se dibuja; se mantiene el modelo de items) ---
#define AURORA_TASKBAR_BG 0xE81A1A1A
#define AURORA_TASKBAR_TOPLIGHT 0x28FFFFFF
#define AURORA_TASKBAR_ITEM_HOVER 0x18FFFFFF
#define AURORA_TASKBAR_ITEM_ACTIVE 0x14FFFFFF
#define AURORA_TASKBAR_INDICATOR AURORA_ACCENT

// --- Geometría ---
#define AURORA_CORNER_RADIUS 20
#define AURORA_CORNER_RADIUS_SM 8
#define AURORA_CORNER_RADIUS_LG 26
#define AURORA_TITLEBAR_HEIGHT 38

// Sombra en dos capas, como macOS: una de contacto, corta y algo más oscura,
// y otra ambiental, ancha y desplazada hacia abajo. La superficie de la
// ventana reserva BLUR + OFFSET_Y px por lado.
#define AURORA_SHADOW_BLUR 30          /* alcance de la capa ambiental */
#define AURORA_SHADOW_OFFSET_Y 10      /* desplazamiento de la ambiental */
#define AURORA_SHADOW_KEY_BLUR 9       /* alcance de la capa de contacto */
#define AURORA_SHADOW_KEY_OFFSET_Y 2
#define AURORA_SHADOW_AMB_ALPHA 66
#define AURORA_SHADOW_KEY_ALPHA 72
#define AURORA_SHADOW_INACTIVE 150     /* intensidad 0..256 sin foco */
#define AURORA_SHADOW_SIZE (AURORA_SHADOW_BLUR + AURORA_SHADOW_OFFSET_Y)

// --- Animaciones ---
#define WIN_ANIM_OPEN_MS 240
#define WIN_ANIM_CLOSE_MS 170
#define WIN_ANIM_MINIMIZE_MS 460 /* efecto "genie" hacia el Dock */
#define WIN_ANIM_RESTORE_MS 380
#define WIN_ANIM_OPEN_SCALE 240  /* escala inicial (de 256) al abrir */
#define WIN_ANIM_CLOSE_SCALE 238 /* escala final (de 256) al cerrar */
#define WIN_DOUBLE_CLICK_MS 420

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
