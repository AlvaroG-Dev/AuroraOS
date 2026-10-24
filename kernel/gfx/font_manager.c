// kernel/gfx/font_manager.c
#include "font_manager.h"

// Fuentes exportadas con convert-font.py
#include "fonts/font_inter_bold14.h"
#include "fonts/font_inter_regular14.h"
#include "fonts/font_jetbrains_bold14.h"
#include "fonts/font_jetbrains_regular14.h"

/* AJUSTA estos dos nombres al símbolo que exporta tu header generado de
 * Inter (el de JetBrains sigue el patrón family_style_style_size; Inter se
 * ha supuesto igual: font_inter_regular_regular_14 / font_inter_bold_bold_14).
 * Comprueba con:  grep -h "font_aa_t" kernel/gfx/fonts/font_inter_*.h */
#define AURORA_FONT_UI_REGULAR font_inter_regular_14
#define AURORA_FONT_UI_BOLD font_inter_bold_14

static const font_aa_t *system_fonts[FONT_ID_COUNT];

void font_manager_init(void) {
  // Interfaz: Inter. Terminal y texto técnico: JetBrains Mono.
  system_fonts[FONT_ID_MAIN_REGULAR] = &AURORA_FONT_UI_REGULAR;
  system_fonts[FONT_ID_MAIN_BOLD] = &AURORA_FONT_UI_BOLD;
  system_fonts[FONT_ID_TITLEBAR] = &AURORA_FONT_UI_BOLD;
  system_fonts[FONT_ID_MONO] = &font_jetbrainsmono_regular_regular_14;
  system_fonts[FONT_ID_MONO_BOLD] = &font_jetbrainsmono_bold_bold_14;
}

const font_aa_t *font_manager_get(font_id_t id) {
  if ((int)id < 0 || id >= FONT_ID_COUNT) {
    return system_fonts[FONT_ID_MAIN_REGULAR];
  }
  return system_fonts[id];
}
