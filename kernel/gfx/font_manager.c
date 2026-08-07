// kernel/gfx/font_manager.c
#include "font_manager.h"

// 1. Incluir aquí las fuentes exportadas con el script en Python
#include "fonts/font_inter_bold14.h"
#include "fonts/font_inter_regular14.h"
#include "fonts/font_jetbrains_bold14.h"
#include "fonts/font_jetbrains_regular14.h"

static const font_aa_t *system_fonts[FONT_ID_COUNT];

void font_manager_init(void) {
  system_fonts[FONT_ID_MAIN_REGULAR] = &font_inter_regular_14;
  system_fonts[FONT_ID_MAIN_BOLD] = &font_inter_bold_14;
  system_fonts[FONT_ID_TITLEBAR] = &font_inter_bold_14;
  system_fonts[FONT_ID_MONO] = &font_jetbrainsmono_regular_regular_14;
  system_fonts[FONT_ID_MONO_BOLD] = &font_jetbrainsmono_bold_bold_14;
}

const font_aa_t *font_manager_get(font_id_t id) {
  if (id < 0 || id >= FONT_ID_COUNT) {
    return system_fonts[FONT_ID_MAIN_REGULAR];
  }
  return system_fonts[id];
}