// kernel/gfx/theme.h
#pragma once
#include <stdint.h>

// Colors format: 0xAARRGGBB

// ============================================================================
// Tema en tiempo de ejecución (oscuro / claro)
// ----------------------------------------------------------------------------
// Las superficies y el texto dependen del modo; el acento, los semáforos y la
// geometría son comunes. Los AURORA_* de abajo que cambian con el modo son
// macros que leen g_theme, así que NO sirven en inicializadores estáticos.
// ============================================================================
typedef struct {
  uint32_t win_bg, surface_card, bg_base, titlebar_active, titlebar_inactive;
  uint32_t border_active, border_inactive, tl_inactive;
  uint32_t text_primary, text_secondary, text_tertiary, text_disabled;
  uint32_t menu_tint, tooltip_tint, dock_tint;
  uint32_t menubar_scrim, menubar_text_shadow, menubar_pill;
  uint32_t icon_tile;   /* baldosa que se pone detrás de iconos sin fondo */
  uint32_t accent_text; /* texto sobre el acento */
  int light;
} aurora_theme_t;

extern const aurora_theme_t *g_theme;
void theme_set_light(int light);
int theme_is_light(void);
uint32_t theme_generation(void); /* sube con cada cambio de modo */

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
#define AURORA_ACCENT 0xFF3EE89B /* verde aurora (docs/Diseno_Identidad_Grafica.md) */
#define AURORA_ACCENT_HOVER 0xFF6BF083
#define AURORA_ACCENT_DIM 0x553EE89B
#define AURORA_ACCENT_TEXT 0xFF04261A /* texto sobre el acento */

// --- Segundo color de marca (extremo turquesa del logo) ---
#define AURORA_TEAL 0xFF00D4D8
#define AURORA_TEAL_BRIGHT 0xFF00EEEE

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
#define AURORA_BG_DARK (g_theme->bg_base)
#define AURORA_MICA_BASE (g_theme->win_bg)
#define AURORA_SURFACE_CARD (g_theme->surface_card)
#define AURORA_WIN_BG (g_theme->win_bg)

// --- Ventanas ---
#define AURORA_TITLEBAR_ACTIVE (g_theme->titlebar_active)
#define AURORA_TITLEBAR_INACTIVE (g_theme->titlebar_inactive)
#define AURORA_BORDER_ACTIVE (g_theme->border_active)
#define AURORA_BORDER_INACTIVE (g_theme->border_inactive)
#define AURORA_TITLEBAR_SEP 0x00000000 /* Tahoe: sin separador */

#define AURORA_TITLEBAR_HAIRLINE 0x66000000 /* separación barra/contenido */

// --- Semáforos (izquierda de la barra de título) ---
#define AURORA_TL_CLOSE 0xFFFF6A3D
#define AURORA_TL_MINIMIZE 0xFF4DA3FF
#define AURORA_TL_MAXIMIZE 0xFF3EE89B
#define AURORA_TL_INACTIVE (g_theme->tl_inactive)
#define AURORA_TL_RIM 0x33000000   /* borde fino oscuro */
/* Glifos (×, −, flechas de zoom) al pasar el ratón: tono oscuro de cada
 * botón, como en macOS, en vez de un negro genérico. */
#define AURORA_TL_GLYPH_CLOSE 0xE06B2410
#define AURORA_TL_GLYPH_MINIMIZE 0xE0083A7A
#define AURORA_TL_GLYPH_MAXIMIZE 0xE00A5A38
#define AURORA_TL_PRESSED 0x40000000 /* oscurecido mientras se pulsa */

// --- Texto ---
#define AURORA_TEXT_PRIMARY (g_theme->text_primary)
#define AURORA_TEXT_SECONDARY (g_theme->text_secondary)
#define AURORA_TEXT_TERTIARY (g_theme->text_tertiary)
#define AURORA_TEXT_DISABLED (g_theme->text_disabled)

// --- Iconos ---
#define AURORA_ICON_DEFAULT 0xFFE8E8F0
#define AURORA_ICON_INACTIVE 0xFF808090
#define AURORA_ICON_MUTED 0xFF5A5A68

// --- Barra de menús: transparente (Tahoe). Solo un velo muy tenue arriba para
// que el texto blanco se lea sobre fondos claros. ---
#define AURORA_MENUBAR_H 28
#define AURORA_MENUBAR_SCRIM (g_theme->menubar_scrim)
#define AURORA_MENUBAR_TEXT_SHADOW (g_theme->menubar_text_shadow)
#define AURORA_MENUBAR_PILL (g_theme->menubar_pill)

// --- Cristal líquido ---
#define AURORA_GLASS_BLUR 12              /* radio del box-blur (2 pasadas) */
#define AURORA_GLASS_SAT 300              /* saturación del fondo, 256 = igual */
#define AURORA_DOCK_TINT (g_theme->dock_tint)
#define AURORA_MENU_TINT (g_theme->menu_tint)
#define AURORA_TOOLTIP_TINT (g_theme->tooltip_tint)
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
