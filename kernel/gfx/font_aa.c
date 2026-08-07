#include "font_aa.h"

void gfx_draw_string_aa(uint32_t *dst, int stride, rect_t clip, int x, int y,
                        const char *str, uint32_t color,
                        const font_aa_t *font) {
  if (!font)
    return;

  int cur_x = x;
  uint32_t base_alpha = (color >> 24) & 0xFF;
  uint32_t rgb = color & 0x00FFFFFF;

  while (*str) {
    unsigned char c = (unsigned char)*str;
    if (c < 128) {
      const glyph_aa_t *g = &font->glyphs[c];
      if (g->bitmap) {
        int draw_x = cur_x + g->bearing_x;
        int draw_y = y + (font->height - g->bearing_y);

        for (int gy = 0; gy < g->height; gy++) {
          int py = draw_y + gy;
          if (py < clip.y || py >= clip.y + clip.h)
            continue;

          for (int gx = 0; gx < g->width; gx++) {
            int px = draw_x + gx;
            if (px < clip.x || px >= clip.x + clip.w)
              continue;

            uint8_t font_alpha = g->bitmap[gy * g->width + gx];
            if (font_alpha > 0) {
              uint32_t final_alpha = (base_alpha * font_alpha) / 255;
              uint32_t final_color = (final_alpha << 24) | rgb;

              uint32_t *pixel = &dst[py * stride + px];
              // Mezcla Alpha perfecta por subpíxel
              *pixel = blend_pixel_fast(final_color, *pixel);
            }
          }
        }
      }
      cur_x += g->advance;
    }
    str++;
  }
}