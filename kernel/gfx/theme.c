// kernel/gfx/theme.c — paletas oscura y clara de AuroraOS
#include "theme.h"

static const aurora_theme_t k_dark = {
    .win_bg = 0xFF1E1E1E,
    .surface_card = 0xFF1E1E1E,
    .bg_base = 0xFF141414,
    .titlebar_active = 0xFF262628,
    .titlebar_inactive = 0xFF212123,
    .border_active = 0x2EFFFFFF,
    .border_inactive = 0x1AFFFFFF,
    .tl_inactive = 0xFF4A4A4D,
    .text_primary = 0xFFFFFFFF,
    .text_secondary = 0xCCFFFFFF,
    .text_tertiary = 0x99FFFFFF,
    .text_disabled = 0x55FFFFFF,
    .menu_tint = 0x9A1E1E22,
    .tooltip_tint = 0xB0202024,
    .dock_tint = 0x3CFFFFFF,
    .menubar_scrim = 0x40000000,
    .menubar_text_shadow = 0x70000000,
    .menubar_pill = 0x38FFFFFF,
    .icon_tile = 0xFF141F22,
    .accent_text = 0xFF04261A,
    .light = 0,
};

static const aurora_theme_t k_light = {
    .win_bg = 0xFFF4F7F5,
    .surface_card = 0xFFFFFFFF,
    .bg_base = 0xFFE9EEEC,
    .titlebar_active = 0xFFE6EBE9,
    .titlebar_inactive = 0xFFEEF2F0,
    .border_active = 0x26000000,
    .border_inactive = 0x14000000,
    .tl_inactive = 0xFFC4CAC7,
    .text_primary = 0xFF14211B,
    .text_secondary = 0xD014211B,
    .text_tertiary = 0x9914211B,
    .text_disabled = 0x5514211B,
    .menu_tint = 0xB0F1F5F3,
    .tooltip_tint = 0xC0F1F5F3,
    .dock_tint = 0x58FFFFFF,
    .menubar_scrim = 0x30FFFFFF,
    .menubar_text_shadow = 0x00000000,
    .menubar_pill = 0x24000000,
    .icon_tile = 0xFFFFFFFF,
    .accent_text = 0xFF04261A,
    .light = 1,
};

const aurora_theme_t *g_theme = &k_dark;
static uint32_t g_theme_gen = 1;

void theme_set_light(int light) {
  const aurora_theme_t *t = light ? &k_light : &k_dark;
  if (t == g_theme)
    return;
  g_theme = t;
  g_theme_gen++;
}

int theme_is_light(void) { return g_theme->light; }
uint32_t theme_generation(void) { return g_theme_gen; }
