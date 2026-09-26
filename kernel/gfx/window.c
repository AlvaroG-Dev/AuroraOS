// kernel/gfx/window.c
#include "window.h"
#include "../bmp.h"
#include "../heap.h"
#include "../klog.h"
#include "compositor.h"
#include "font_manager.h"
#include "gfx_filter.h"
#include "taskbar.h"
#include "theme.h"
#include <stddef.h>

static void draw_app_icon(window_t *win, uint32_t *dst, int stride, rect_t clip,
                          int x, int y) {
  if (!win)
    return;

  /* [DEBUG] Dump único del contenido del cache. Si el icono sale doble
   * en pantalla, aquí veremos si el doble está en el cache o en el blit. */
  static int icon_dbg_done = 0;
  if (!icon_dbg_done && win->has_icon_cache && win->title[0] == 'A') {
    icon_dbg_done = 1;
    LOG_DEBUG("[ICON-DBG] '%s' %dx%d:", win->title, WIN_ICON_SIZE,
              WIN_ICON_SIZE);
    for (int y = 0; y < WIN_ICON_SIZE; y++) {
      char line[WIN_ICON_SIZE + 1];
      for (int x = 0; x < WIN_ICON_SIZE; x++) {
        uint32_t c = win->icon_cache[y * WIN_ICON_SIZE + x];
        uint32_t a = (c >> 24) & 0xFF;
        line[x] = (a > 128) ? '#' : (a > 32) ? '+' : (a > 8) ? '.' : ' ';
      }
      line[WIN_ICON_SIZE] = '\0';
      LOG_DEBUG("[ICON-DBG] |%s|", line);
    }
  }

  const int SZ = WIN_ICON_SIZE;
  const int RADIUS = 5;

  /* has_icon_cache ya significa "el cache está poblado". No hace falta
   * comprobar icon_cache[0]: la esquina superior izquierda de un icono
   * es transparente (alpha=0, RGB=0), así que ese test siempre fallaba
   * y se entraba al segundo branch, redibujando el icono encima. */
  if (win->has_icon_cache) {
    gfx_bit_blat(dst, stride, win->icon_cache, SZ, clip, x, y, SZ, SZ);
    return;
  }
  if (win->icon_bmp_node) {
    for (int i = 0; i < SZ * SZ; i++)
      win->icon_cache[i] = 0;
    if (bmp_draw_icon_scaled(win->icon_bmp_node, win->icon_cache, SZ, SZ) ==
        0) {
      win->has_icon_cache = 1;
      gfx_bit_blat(dst, stride, win->icon_cache, SZ, clip, x, y, SZ, SZ);
      return;
    }
  }

  /* Fallback: pastilla con gradiente + símbolo */
  uint32_t bg = win->icon_bg_color;
  if (bg == 0xFF0078D4)
    bg = AURORA_ACCENT;

  uint32_t bg_top = bg;
  uint32_t bg_bot =
      (bg & 0xFF000000) | ((((bg >> 16) & 0xFF) * 70 / 100) << 16) |
      ((((bg >> 8) & 0xFF) * 70 / 100) << 8) | (((bg & 0xFF) * 75 / 100));

  gfx_fill_rounded_rect_gradient_v(dst, stride, clip, (rect_t){x, y, SZ, SZ},
                                   RADIUS, bg_top, bg_bot);

  gfx_blend_rect(dst, stride, clip, (rect_t){x + RADIUS, y, SZ - RADIUS * 2, 1},
                 0x55FFFFFF);
  gfx_draw_rounded_border(dst, stride, clip, (rect_t){x, y, SZ, SZ}, RADIUS,
                          0x25FFFFFF);

  const font_aa_t *f = font_manager_get(FONT_ID_MAIN_BOLD);
  if (f && win->icon_symbol[0]) {
    unsigned char c = (unsigned char)win->icon_symbol[0];
    if (c < 128) {
      const glyph_aa_t *g = &f->glyphs[c];
      if (g->bitmap && g->width > 0 && g->height > 0) {
        int gw = g->width, gh = g->height;
        int tx = x + (SZ - gw) / 2 - g->bearing_x;
        int ty = y + (SZ - gh) / 2;
        gfx_draw_string(dst, stride, clip, tx, ty + 1, win->icon_symbol,
                        0x80000000, FONT_ID_MAIN_BOLD);
        gfx_draw_string(dst, stride, clip, tx, ty, win->icon_symbol, 0xFFFFFFFF,
                        FONT_ID_MAIN_BOLD);
      }
    }
  }
}

void win_set_icon_bmp(window_t *win, tar_node_t *bmp_file) {
  if (!win)
    return;

  if (!bmp_file) {
    win->icon_bmp_node = NULL;
    win->has_icon_cache = 0;
    if (win->taskbar_item) {
      win->taskbar_item->icon_bmp_node = NULL;
      win->taskbar_item->has_icon_cache = 0;
    }
  } else {
    for (int i = 0; i < WIN_ICON_SIZE * WIN_ICON_SIZE; i++)
      win->icon_cache[i] = 0;
    int rc = bmp_draw_icon_scaled(bmp_file, win->icon_cache, WIN_ICON_SIZE,
                                  WIN_ICON_SIZE);
    if (rc < 0) {
      LOG_ERR("[WINDOW] BMP de icono rechazado para '%s' (%dx%d)",
              bmp_file->name, WIN_ICON_SIZE, WIN_ICON_SIZE);
      win->has_icon_cache = 0;
      return;
    }

    if (win->taskbar_item) {
      for (int i = 0; i < 20 * 20; i++)
        win->taskbar_item->icon_cache[i] = 0;
      int rc20 =
          bmp_draw_icon_scaled(bmp_file, win->taskbar_item->icon_cache, 20, 20);
      if (rc20 < 0) {
        LOG_ERR("[WINDOW] BMP de icono rechazado para '%s' (20x20)",
                bmp_file->name);
        win->has_icon_cache = 0;
        win->taskbar_item->has_icon_cache = 0;
        return;
      }
      win->taskbar_item->icon_bmp_node = bmp_file;
      win->taskbar_item->has_icon_cache = 1;
    }

    win->icon_bmp_node = bmp_file;
    win->has_icon_cache = 1;
  }

  win->dirty = 1;
  rect_t damage = {win->x - WIN11_SHADOW_SIZE, win->y - WIN11_SHADOW_SIZE,
                   win->width + WIN11_SHADOW_SIZE * 2,
                   win->height + WIN11_SHADOW_SIZE * 2};
  compositor_invalidate_rect(damage);
}

window_t *window_create(int x, int y, int w, int h, const char *title,
                        uint32_t flags) {
  window_t *win = (window_t *)kmalloc(sizeof(window_t));
  if (!win)
    return NULL;

  win->x = x;
  win->y = y;
  win->width = w;
  win->height = h;
  win->flags = (flags | WIN_FLAGS_MOVABLE) & ~WIN_FLAGS_HIDDEN;
  win->z_order = 0;
  win->next = NULL;
  win->prev = NULL;

  win->icon_bmp_node = NULL;
  // [FIX] antes era 18*18, ahora usa la constante real
  for (int i = 0; i < WIN_ICON_SIZE * WIN_ICON_SIZE; i++)
    win->icon_cache[i] = 0;
  win->has_icon_cache = 0;
  win->icon_symbol[0] = '>';
  win->icon_symbol[1] = '\0';
  win->icon_bg_color = 0xFF0078D4;
  win->icon_buffer = NULL;
  win->taskbar_item = NULL;

  win->anim_state = WIN_ANIM_NONE;
  win->anim_t = 0;
  win->anim_alpha = 255;
  win->anim_dy = 0;
  win->pending_destroy = 0;

  win->hover_btn = 0;
  win->hover_controls = 0;

  if (!(flags & WIN_FLAGS_NODECORATION)) {
    win->content_w = w;
    win->content_h = h - WIN11_TITLEBAR_HEIGHT;
  } else {
    win->content_w = w;
    win->content_h = h;
  }

  if (win->content_w <= 0 || win->content_h <= 0) {
    kfree(win);
    return NULL;
  }

  size_t buf_size = (size_t)win->content_w * win->content_h * sizeof(uint32_t);
  win->content_buffer = (uint32_t *)kmalloc(buf_size);
  if (!win->content_buffer) {
    kfree(win);
    return NULL;
  }

  win->surface_w = win->width + (WIN11_SHADOW_SIZE * 2);
  win->surface_h = win->height + (WIN11_SHADOW_SIZE * 2);
  size_t surf_bytes =
      (size_t)win->surface_w * win->surface_h * sizeof(uint32_t);

  win->surface = (uint32_t *)kmalloc(surf_bytes);
  if (!win->surface) {
    kfree(win->content_buffer);
    kfree(win);
    return NULL;
  }

  win_clear(win, WIN11_SURFACE_CARD);

  int i = 0;
  while (title && title[i] && i < 63) {
    win->title[i] = title[i];
    i++;
  }
  win->title[i] = '\0';

  win->dirty = 1;
  window_redraw_surface(win);

  window_start_open_animation(win);

  return win;
}

void window_set_visible(window_t *win, int visible) {
  if (!win)
    return;

  int was_visible = !(win->flags & WIN_FLAGS_HIDDEN);
  if (visible) {
    win->flags &= ~WIN_FLAGS_HIDDEN;
  } else {
    win->flags |= WIN_FLAGS_HIDDEN;
  }

  if (was_visible != visible) {
    win->dirty = 1;
    rect_t damage = {win->x - WIN11_SHADOW_SIZE, win->y - WIN11_SHADOW_SIZE,
                     win->width + WIN11_SHADOW_SIZE * 2,
                     win->height + WIN11_SHADOW_SIZE * 2};
    compositor_invalidate_rect(damage);
  }
}

int window_is_visible(window_t *win) {
  return win && !(win->flags & WIN_FLAGS_HIDDEN);
}

void window_destroy(window_t *win) {
  if (!win)
    return;
  if (win->taskbar_item) {
    taskbar_remove_item_ptr(win->taskbar_item);
    win->taskbar_item = NULL;
  }
  if (win->content_buffer)
    kfree(win->content_buffer);
  if (win->surface)
    kfree(win->surface);
  kfree(win);
}

void window_redraw_surface(window_t *win) {
  if (!win || !win->surface || !win->dirty || (win->flags & WIN_FLAGS_HIDDEN))
    return;

  size_t total_p = (size_t)win->surface_w * win->surface_h;
  for (size_t i = 0; i < total_p; i++)
    win->surface[i] = 0x00000000;

  rect_t s_clip = {0, 0, win->surface_w, win->surface_h};
  rect_t win_local = {WIN11_SHADOW_SIZE, WIN11_SHADOW_SIZE, win->width,
                      win->height};

  /* 1. Sombra */
  gfx_draw_shadow_aurora(win->surface, win->surface_w, s_clip, win_local,
                         WIN11_CORNER_RADIUS);

  if (win->flags & WIN_FLAGS_NODECORATION) {
    gfx_bit_blat(win->surface, win->surface_w, win->content_buffer,
                 win->content_w, s_clip, WIN11_SHADOW_SIZE, WIN11_SHADOW_SIZE,
                 win->content_w, win->content_h);
    win->dirty = 0;
    return;
  }

  int focused = (win->flags & WIN_FLAGS_FOCUSED) ? 1 : 0;

  /* 2. Cuerpo con gradiente sutil */
  uint32_t body_top, body_bot;
  if (focused) {
    body_top = AURORA_WIN_BG;
    body_bot = (AURORA_WIN_BG & 0xFF000000) |
               ((((AURORA_WIN_BG >> 16) & 0xFF) * 92 / 100) << 16) |
               ((((AURORA_WIN_BG >> 8) & 0xFF) * 92 / 100) << 8) |
               (((AURORA_WIN_BG & 0xFF) * 96 / 100));
  } else {
    body_top = (AURORA_WIN_BG & 0xFF000000) |
               ((((AURORA_WIN_BG >> 16) & 0xFF) * 92 / 100) << 16) |
               ((((AURORA_WIN_BG >> 8) & 0xFF) * 92 / 100) << 8) |
               (((AURORA_WIN_BG & 0xFF) * 94 / 100));
    body_bot = (AURORA_WIN_BG & 0xFF000000) |
               ((((AURORA_WIN_BG >> 16) & 0xFF) * 84 / 100) << 16) |
               ((((AURORA_WIN_BG >> 8) & 0xFF) * 84 / 100) << 8) |
               (((AURORA_WIN_BG & 0xFF) * 88 / 100));
  }
  gfx_fill_rounded_rect_gradient_v(win->surface, win->surface_w, s_clip,
                                   win_local, WIN11_CORNER_RADIUS, body_top,
                                   body_bot);

  /* 3. Titlebar SÓLIDO con esquinas redondeadas arriba.
   *    Sin superponer gradiente encima: la versión anterior aplicaba
   *    gfx_gradient_rect_v sobre la mitad inferior del titlebar con
   *    blend, lo que duplicaba la intensidad y creaba una línea horizontal. */
  rect_t titlebar = {WIN11_SHADOW_SIZE, WIN11_SHADOW_SIZE, win->width,
                     WIN11_TITLEBAR_HEIGHT};
  uint32_t tb_color =
      focused ? AURORA_TITLEBAR_ACTIVE : AURORA_TITLEBAR_INACTIVE;
  gfx_fill_top_rounded_rect(win->surface, win->surface_w, s_clip, titlebar,
                            WIN11_CORNER_RADIUS, tb_color);

  gfx_blend_rect(win->surface, win->surface_w, s_clip,
                 (rect_t){titlebar.x, titlebar.y + WIN11_TITLEBAR_HEIGHT - 1,
                          titlebar.w, 1},
                 AURORA_TITLEBAR_SEP);

  /* 4. Icono + título centrados verticalmente */
  const int ICON_PAD_LEFT = 10;
  const int ICON_TEXT_GAP = 8;
  const int FONT_H_FALLBACK = 14; /* solo si no hay fuente cargada todavía */

  int icon_x = WIN11_SHADOW_SIZE + ICON_PAD_LEFT;
  int icon_y = WIN11_SHADOW_SIZE + (WIN11_TITLEBAR_HEIGHT - WIN_ICON_SIZE) / 2;
  draw_app_icon(win, win->surface, win->surface_w, s_clip, icon_x, icon_y);

  /* [FIX] Antes se centraba con FONT_H=14 "a ojo", un número inventado que
   * no tiene por qué coincidir con el ink-height real de FONT_ID_MAIN_BOLD.
   * Si no coincide, el texto queda centrado respecto a una caja que no es
   * la suya de verdad y se desalinea del icono (que sí usa métricas reales
   * desde el fix de bmp_draw_icon_scaled). Usamos aquí el mismo enfoque que
   * ya se usa para el símbolo de fallback del icono: medir un glifo de
   * referencia real ('A', sin descendente, buen proxy del cap-height) y
   * centrar con su altura de verdad. */
  int font_h = FONT_H_FALLBACK;
  const font_aa_t *title_font = font_manager_get(FONT_ID_MAIN_BOLD);
  if (title_font) {
    const glyph_aa_t *ref = &title_font->glyphs[(unsigned char)'A'];
    if (ref->bitmap && ref->height > 0) {
      font_h = ref->height;
    }
  }

  int title_x = icon_x + WIN_ICON_SIZE + ICON_TEXT_GAP;
  int title_y = WIN11_SHADOW_SIZE + (WIN11_TITLEBAR_HEIGHT - font_h - 4) / 2;
  gfx_draw_string(
      win->surface, win->surface_w, s_clip, title_x, title_y, win->title,
      focused ? AURORA_TEXT_PRIMARY : AURORA_TEXT_SECONDARY, FONT_ID_MAIN_BOLD);

  /* 5. Botones de ventana */
  const int BTN_W = 46;
  const int TB_H = WIN11_TITLEBAR_HEIGHT;
  int base_x = WIN11_SHADOW_SIZE + win->width;
  int close_x = base_x - BTN_W;
  int max_x = close_x - BTN_W;
  int min_x = max_x - BTN_W;

  if (win->hover_controls) {
    int bx = -1;
    uint32_t hover_color = AURORA_BTN_HOVER;
    if (win->hover_btn == 1)
      bx = min_x;
    else if (win->hover_btn == 2)
      bx = max_x;
    else if (win->hover_btn == 3) {
      bx = close_x;
      hover_color = AURORA_BTN_CLOSE_HOVER;
    }

    if (bx >= 0) {
      if (win->hover_btn == 3) {
        rect_t r = {bx, WIN11_SHADOW_SIZE, BTN_W, TB_H};
        for (int yy = r.y; yy < r.y + r.h; yy++) {
          for (int xx = r.x; xx < r.x + r.w; xx++) {
            int dx = (r.x + r.w - 1) - xx;
            int dy = yy - r.y;
            if (dy < WIN11_CORNER_RADIUS && dx < WIN11_CORNER_RADIUS) {
              int cx2 = WIN11_CORNER_RADIUS - dx;
              int cy2 = WIN11_CORNER_RADIUS - dy;
              int d2 = cx2 * cx2 + cy2 * cy2;
              int rr = WIN11_CORNER_RADIUS * WIN11_CORNER_RADIUS;
              if (d2 > rr)
                continue;
            }
            if (xx >= s_clip.x && xx < s_clip.x + s_clip.w && yy >= s_clip.y &&
                yy < s_clip.y + s_clip.h)
              win->surface[yy * win->surface_w + xx] = blend_pixel_fast(
                  hover_color, win->surface[yy * win->surface_w + xx]);
          }
        }
      } else {
        gfx_blend_rect(win->surface, win->surface_w, s_clip,
                       (rect_t){bx, WIN11_SHADOW_SIZE, BTN_W, TB_H},
                       hover_color);
      }
    }
  }

  uint32_t glyph_color = AURORA_TEXT_PRIMARY;

  {
    int cx = min_x + BTN_W / 2, cy = WIN11_SHADOW_SIZE + TB_H / 2;
    gfx_blend_rect(win->surface, win->surface_w, s_clip,
                   (rect_t){cx - 5, cy, 10, 1}, glyph_color);
  }

  {
    int cx = max_x + BTN_W / 2, cy = WIN11_SHADOW_SIZE + TB_H / 2;
    gfx_draw_rect_outline(win->surface, win->surface_w, s_clip,
                          (rect_t){cx - 5, cy - 5, 10, 10}, glyph_color);
  }

  {
    int cx = close_x + BTN_W / 2, cy = WIN11_SHADOW_SIZE + TB_H / 2;
    gfx_draw_line(win->surface, win->surface_w, s_clip, cx - 5, cy - 5, cx + 5,
                  cy + 5, glyph_color);
    gfx_draw_line(win->surface, win->surface_w, s_clip, cx - 5, cy + 5, cx + 5,
                  cy - 5, glyph_color);
  }

  /* 6. Contenido cliente */
  gfx_bit_blat_rounded(
      win->surface, win->surface_w, win->content_buffer, win->content_w,
      win->content_h, s_clip, WIN11_SHADOW_SIZE,
      WIN11_SHADOW_SIZE + WIN11_TITLEBAR_HEIGHT, WIN11_SHADOW_SIZE,
      WIN11_SHADOW_SIZE, win->width, win->height, WIN11_CORNER_RADIUS);

  /* 7. Borde + rim lights */
  uint32_t b_color = focused ? AURORA_BORDER_ACTIVE : AURORA_BORDER_INACTIVE;
  gfx_draw_rounded_border(win->surface, win->surface_w, s_clip, win_local,
                          WIN11_CORNER_RADIUS, b_color);

  gfx_blend_rect(win->surface, win->surface_w, s_clip,
                 (rect_t){WIN11_SHADOW_SIZE + WIN11_CORNER_RADIUS,
                          WIN11_SHADOW_SIZE,
                          win->width - WIN11_CORNER_RADIUS * 2, 1},
                 focused ? 0x66FFFFFF : 0x30FFFFFF);

  gfx_blend_rect(win->surface, win->surface_w, s_clip,
                 (rect_t){WIN11_SHADOW_SIZE,
                          WIN11_SHADOW_SIZE + WIN11_CORNER_RADIUS, 1,
                          win->height - WIN11_CORNER_RADIUS * 2},
                 0x18FFFFFF);

  win->dirty = 0;
}

void window_render_frame(window_t *win, uint32_t *dst, int dst_stride,
                         rect_t clip) {
  if (!win || !dst || !win->surface || (win->flags & WIN_FLAGS_HIDDEN))
    return;

  if (win->dirty) {
    window_redraw_surface(win);
  }

  int surf_x = win->x - WIN11_SHADOW_SIZE;
  int surf_y = win->y - WIN11_SHADOW_SIZE + win->anim_dy;

  if (win->anim_alpha >= 255) {
    gfx_bit_blat(dst, dst_stride, win->surface, win->surface_w, clip, surf_x,
                 surf_y, win->surface_w, win->surface_h);
  } else if (win->anim_alpha > 0) {
    gfx_blit_with_alpha(dst, dst_stride, win->surface, win->surface_w, clip,
                        surf_x, surf_y, win->surface_w, win->surface_h,
                        (uint8_t)win->anim_alpha);
  }
}

void win_set_icon_text(window_t *win, const char *symbol, uint32_t bg_color) {
  if (!win)
    return;
  int i = 0;
  while (symbol && symbol[i] && i < 7) {
    win->icon_symbol[i] = symbol[i];
    i++;
  }
  win->icon_symbol[i] = '\0';
  win->icon_bg_color = bg_color;
  win->icon_buffer = NULL;
  win->dirty = 1;
  if (win->taskbar_item) {
    i = 0;
    while (symbol && symbol[i] && i < 7) {
      win->taskbar_item->icon_symbol[i] = symbol[i];
      i++;
    }
    win->taskbar_item->icon_symbol[i] = '\0';
    win->taskbar_item->icon_color = bg_color;
  }
}

void win_set_icon_image(window_t *win, const uint32_t *icon_18x18) {
  if (!win)
    return;
  win->icon_buffer = icon_18x18;
  win->dirty = 1;
}

void win_clear(window_t *win, uint32_t color) {
  if (!win || !win->content_buffer)
    return;
  size_t total_pixels = (size_t)win->content_w * win->content_h;
  for (size_t i = 0; i < total_pixels; i++) {
    win->content_buffer[i] = color;
  }
  win->dirty = 1;
}

void win_draw_rect(window_t *win, int x, int y, int w, int h, uint32_t color) {
  if (!win || !win->content_buffer)
    return;
  rect_t win_clip = {0, 0, win->content_w, win->content_h};
  gfx_blend_rect(win->content_buffer, win->content_w, win_clip,
                 (rect_t){x, y, w, h}, color);
  win->dirty = 1;
}

void win_draw_rounded_rect(window_t *win, int x, int y, int w, int h,
                           int radius, uint32_t color) {
  if (!win || !win->content_buffer)
    return;
  rect_t win_clip = {0, 0, win->content_w, win->content_h};
  gfx_fill_rounded_rect(win->content_buffer, win->content_w, win_clip,
                        (rect_t){x, y, w, h}, radius, color);
  win->dirty = 1;
}

void win_draw_string(window_t *win, int x, int y, const char *str,
                     uint32_t color, font_id_t font) {
  if (!win || !win->content_buffer || !str)
    return;
  rect_t win_clip = {0, 0, win->content_w, win->content_h};
  gfx_draw_string(win->content_buffer, win->content_w, win_clip, x, y, str,
                  color, font);
  win->dirty = 1;
}

void win_draw_string_aligned(window_t *win, rect_t rect, const char *str,
                             uint32_t color, font_id_t font, uint32_t align) {
  if (!win || !str)
    return;
  int len = 0;
  while (str[len])
    len++;
  int text_w = len * 8, text_h = 13;
  int x = rect.x, y = rect.y;

  if (align & ALIGN_CENTER_H)
    x = rect.x + (rect.w - text_w) / 2;
  else if (align & ALIGN_RIGHT)
    x = rect.x + rect.w - text_w;

  if (align & ALIGN_CENTER_V)
    y = rect.y + (rect.h - text_h) / 2;
  else if (align & ALIGN_BOTTOM)
    y = rect.y + rect.h - text_h;

  win_draw_string(win, x, y, str, color, font);
}

void win_draw_image(window_t *win, int x, int y, int w, int h,
                    const uint32_t *img_data) {
  if (!win || !win->content_buffer || !img_data)
    return;
  rect_t win_clip = {0, 0, win->content_w, win->content_h};
  gfx_bit_blat(win->content_buffer, win->content_w, img_data, w, win_clip, x, y,
               w, h);
  win->dirty = 1;
}

void win_draw_image_scaled(window_t *win, int dst_x, int dst_y, int dst_w,
                           int dst_h, const uint32_t *img_data, int src_w,
                           int src_h) {
  if (!win || !win->content_buffer || !img_data || dst_w <= 0 || dst_h <= 0)
    return;

  rect_t win_clip = {0, 0, win->content_w, win->content_h};
  gfx_blit_scaled(win->content_buffer, win->content_w, win_clip, dst_x, dst_y,
                  dst_w, dst_h, img_data, src_w, src_h, GFX_FILTER_AUTO);
  win->dirty = 1;
}

void win_draw_image_full(window_t *win, const uint32_t *img_data, int src_w,
                         int src_h) {
  win_draw_image_scaled(win, 0, 0, win->content_w, win->content_h, img_data,
                        src_w, src_h);
}

void win_update(window_t *win) {
  if (!win)
    return;
  win->dirty = 1;
  rect_t damage = {win->x - WIN11_SHADOW_SIZE, win->y - WIN11_SHADOW_SIZE,
                   win->width + WIN11_SHADOW_SIZE * 2,
                   win->height + WIN11_SHADOW_SIZE * 2};
  compositor_invalidate_rect(damage);
}

/* ============================================================================
 * Animaciones
 * ==========================================================================*/

void window_start_open_animation(window_t *win) {
  if (!win)
    return;
  win->anim_state = WIN_ANIM_OPENING;
  win->anim_t = 0;
  win->anim_alpha = 0;
  win->anim_dy = WIN_ANIM_OPEN_DY;
}

void window_start_close_animation(window_t *win) {
  if (!win)
    return;
  if (win->anim_state == WIN_ANIM_CLOSING)
    return;
  win->anim_state = WIN_ANIM_CLOSING;
  win->anim_t = 256;
  win->anim_alpha = 255;
  win->anim_dy = 0;
}

int window_is_animating(window_t *win) {
  return win && win->anim_state != WIN_ANIM_NONE;
}

static int ease_out_cubic_fp(int t) {
  if (t <= 0)
    return 0;
  if (t >= 256)
    return 256;
  uint64_t inv = (uint64_t)(256 - t);
  uint64_t cube_inv = inv * inv * inv;
  uint64_t cube_256 = (uint64_t)256 * 256 * 256;
  uint64_t num = cube_256 - cube_inv;
  return (int)((num * 256 + cube_256 / 2) / cube_256);
}

void window_advance_animation(window_t *win, int dt_ms) {
  if (!win || win->anim_state == WIN_ANIM_NONE || dt_ms <= 0)
    return;

  int is_closing = (win->anim_state == WIN_ANIM_CLOSING ||
                    win->anim_state == WIN_ANIM_MINIMIZING);

  int duration = is_closing ? WIN_ANIM_CLOSE_MS : WIN_ANIM_OPEN_MS;
  if (duration <= 0)
    duration = 1;

  int delta = (256 * dt_ms) / duration;
  if (delta < 2)
    delta = 2;

  if (!is_closing) {
    win->anim_t += delta;
    if (win->anim_t >= 256) {
      win->anim_t = 256;
      win->anim_state = WIN_ANIM_NONE;
    }
  } else {
    win->anim_t -= delta;
    if (win->anim_t <= 0) {
      win->anim_t = 0;
      win->anim_state = WIN_ANIM_NONE;
    }
  }

  int ease = ease_out_cubic_fp(win->anim_t);
  win->anim_alpha = (ease * 255 + 128) / 256;

  if (win->anim_state == WIN_ANIM_NONE) {
    if (win->anim_t >= 256) {
      win->anim_alpha = 255;
      win->anim_dy = 0;
    } else {
      win->anim_alpha = 0;
      win->anim_dy = 0;
    }
  } else {
    int base_dy = (win->anim_state == WIN_ANIM_OPENING) ? WIN_ANIM_OPEN_DY : 0;
    win->anim_dy = (base_dy * (256 - ease)) / 256;
  }

  win->dirty = 1;
}

void window_start_minimize_animation(window_t *win) {
  if (!win)
    return;
  if (win->anim_state == WIN_ANIM_CLOSING ||
      win->anim_state == WIN_ANIM_MINIMIZING)
    return;
  win->anim_state = WIN_ANIM_MINIMIZING;
  win->anim_t = 256;
  win->anim_alpha = 255;
  win->anim_dy = 0;
}