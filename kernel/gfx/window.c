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

#define SH AURORA_SHADOW_SIZE
#define R AURORA_CORNER_RADIUS

int window_titlebar_h(const window_t *win) {
  if (!win || (win->flags & WIN_FLAGS_NODECORATION))
    return 0;
  return AURORA_TITLEBAR_HEIGHT;
}

/* Centro (relativo a la ventana) del semáforo i: 0 cerrar, 1 min, 2 max. */
static inline int tl_cx(int i) {
  return WIN_TL_X + i * WIN_TL_PITCH + WIN_TL_DIAM / 2;
}

win_hit_t window_hit_test(const window_t *w, int sx, int sy) {
  if (!w)
    return WIN_HIT_NONE;
  int lx = sx - w->x, ly = sy - w->y;
  if (lx < 0 || ly < 0 || lx >= w->width || ly >= w->height)
    return WIN_HIT_NONE;

  int tb = window_titlebar_h(w);
  int near_r = lx >= w->width - WIN_RESIZE_BORDER;
  int near_b = ly >= w->height - WIN_RESIZE_BORDER;
  if (near_r && near_b)
    return WIN_HIT_RESIZE_BR;
  if (near_b)
    return WIN_HIT_RESIZE_B;
  if (near_r)
    return WIN_HIT_RESIZE_R;

  if (tb > 0 && ly < tb) {
    int cy = tb / 2;
    for (int i = 0; i < 3; i++) {
      int dx = lx - tl_cx(i), dy = ly - cy;
      if (dx < 0)
        dx = -dx;
      if (dy < 0)
        dy = -dy;
      if (dx <= WIN_TL_HIT / 2 && dy <= WIN_TL_HIT / 2)
        return i == 0 ? WIN_HIT_CLOSE
                      : (i == 1 ? WIN_HIT_MINIMIZE : WIN_HIT_MAXIMIZE);
    }
    return WIN_HIT_TITLEBAR;
  }
  return WIN_HIT_CLIENT;
}

int window_in_controls(const window_t *w, int sx, int sy) {
  if (!w)
    return 0;
  int tb = window_titlebar_h(w);
  int lx = sx - w->x, ly = sy - w->y;
  if (tb == 0 || ly < 0 || ly >= tb || lx < 0)
    return 0;
  return lx < tl_cx(2) + WIN_TL_DIAM / 2 + 8;
}

// ---------------------------------------------------------------------------
// Iconos: la ventana ya no pinta icono en la barra de título (como macOS);
// el icono vive en el Dock. Aquí solo se guarda el nodo BMP y se invalida la
// caché del Dock para que lo regenere.
// ---------------------------------------------------------------------------
void win_set_icon_bmp(window_t *win, tar_node_t *bmp_file) {
  if (!win)
    return;
  win->icon_bmp_node = bmp_file;
  win->has_icon_cache = bmp_file ? 1 : 0;
  if (win->taskbar_item) {
    win->taskbar_item->icon_bmp_node = bmp_file;
    win->taskbar_item->dock_icon_state = 0; /* regenerar */
  }
  compositor_invalidate_dock();
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
  if (win->taskbar_item) {
    i = 0;
    while (symbol && symbol[i] && i < 7) {
      win->taskbar_item->icon_symbol[i] = symbol[i];
      i++;
    }
    win->taskbar_item->icon_symbol[i] = '\0';
    win->taskbar_item->icon_color = bg_color;
    win->taskbar_item->dock_icon_state = 0;
  }
  compositor_invalidate_dock();
}

void win_set_icon_image(window_t *win, const uint32_t *icon_18x18) {
  if (!win)
    return;
  win->icon_buffer = icon_18x18;
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

  win->maximized = 0;
  win->restore_x = win->restore_y = win->restore_w = win->restore_h = 0;
  spin_init(&win->content_lock);

  int tb = (flags & WIN_FLAGS_NODECORATION) ? 0 : AURORA_TITLEBAR_HEIGHT;
  win->content_w = w;
  win->content_h = h - tb;

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

  win->surface_w = win->width + SH * 2;
  win->surface_h = win->height + SH * 2;
  size_t surf_bytes =
      (size_t)win->surface_w * win->surface_h * sizeof(uint32_t);

  win->surface = (uint32_t *)kmalloc(surf_bytes);
  if (!win->surface) {
    kfree(win->content_buffer);
    kfree(win);
    return NULL;
  }

  win->dirty = 1;
  win->content_dirty = 0;
  win_clear(win, AURORA_SURFACE_CARD);

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
  if (visible)
    win->flags &= ~WIN_FLAGS_HIDDEN;
  else
    win->flags |= WIN_FLAGS_HIDDEN;

  if (was_visible != visible) {
    win->dirty = 1;
    compositor_invalidate_rect((rect_t){win->x - SH, win->y - SH,
                                        win->width + SH * 2,
                                        win->height + SH * 2});
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

// ---------------------------------------------------------------------------
// Título: se recorta con "..." si no cabe entre los semáforos y el borde.
// ---------------------------------------------------------------------------
static void fit_title(const char *src, char *out, size_t cap, int max_w,
                      font_id_t font) {
  size_t n = 0;
  while (src[n] && n < cap - 1) {
    out[n] = src[n];
    n++;
  }
  out[n] = '\0';
  if (gfx_text_width(out, font) <= max_w)
    return;

  const int dots_w = gfx_text_width("...", font);
  while (n > 0) {
    n--;
    while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80)
      n--; /* no cortar en mitad de una secuencia UTF-8 */
    out[n] = '\0';
    if (gfx_text_width(out, font) + dots_w <= max_w) {
      if (n + 4 < cap) {
        out[n] = '.';
        out[n + 1] = '.';
        out[n + 2] = '.';
        out[n + 3] = '\0';
      }
      return;
    }
  }
  out[0] = '\0';
}

static void draw_traffic_light(window_t *win, rect_t s_clip, int i,
                               uint32_t color, int glyph) {
  int tb = AURORA_TITLEBAR_HEIGHT;
  rect_t r = {SH + WIN_TL_X + i * WIN_TL_PITCH, SH + (tb - WIN_TL_DIAM) / 2,
              WIN_TL_DIAM, WIN_TL_DIAM};
  gfx_fill_rounded_rect(win->surface, win->surface_w, s_clip, r,
                        WIN_TL_DIAM / 2, color);
  gfx_draw_rounded_border(win->surface, win->surface_w, s_clip, r,
                          WIN_TL_DIAM / 2, AURORA_TL_RIM);
  if (!glyph)
    return;
  int cx = r.x + WIN_TL_DIAM / 2, cy = r.y + WIN_TL_DIAM / 2;
  uint32_t g = AURORA_TL_GLYPH;
  if (i == 0) { /* × */
    gfx_draw_line(win->surface, win->surface_w, s_clip, cx - 2, cy - 2, cx + 2,
                  cy + 2, g);
    gfx_draw_line(win->surface, win->surface_w, s_clip, cx - 2, cy + 2, cx + 2,
                  cy - 2, g);
  } else if (i == 1) { /* − */
    gfx_draw_line(win->surface, win->surface_w, s_clip, cx - 3, cy, cx + 3, cy,
                  g);
  } else { /* + */
    gfx_draw_line(win->surface, win->surface_w, s_clip, cx - 3, cy, cx + 3, cy,
                  g);
    gfx_draw_line(win->surface, win->surface_w, s_clip, cx, cy - 3, cx, cy + 3,
                  g);
  }
}

// ---------------------------------------------------------------------------
// Tubería de dibujo única, limitada por clip. El redibujado completo usa el
// clip de toda la superficie; el de "solo contenido" usa únicamente el área
// de contenido. Como ambos recorren exactamente las mismas etapas, el camino
// barato produce píxeles idénticos al completo (incluidas las esquinas
// redondeadas, donde mezclar encima de lo ya dibujado acumularía borde).
// El coste del camino barato es proporcional al área de contenido: no toca
// la sombra exterior, la barra de título ni los semáforos.
// ---------------------------------------------------------------------------
static void draw_window(window_t *win, rect_t s_clip) {
  rect_t sc = rect_clip(s_clip, (rect_t){0, 0, win->surface_w, win->surface_h});
  if (sc.w <= 0 || sc.h <= 0)
    return;

  for (int y = sc.y; y < sc.y + sc.h; y++) {
    uint32_t *row = &win->surface[y * win->surface_w];
    for (int x = sc.x; x < sc.x + sc.w; x++)
      row[x] = 0x00000000;
  }

  rect_t win_local = {SH, SH, win->width, win->height};
  int focused = (win->flags & WIN_FLAGS_FOCUSED) ? 1 : 0;

  /* 1. Sombra (más tenue en ventanas inactivas) */
  gfx_draw_shadow_aurora_ex(win->surface, win->surface_w, sc, win_local, R,
                            focused ? 256 : 140);

  if (win->flags & WIN_FLAGS_NODECORATION) {
    gfx_bit_blat(win->surface, win->surface_w, win->content_buffer,
                 win->content_w, sc, SH, SH, win->content_w, win->content_h);
    return;
  }

  const int tb = AURORA_TITLEBAR_HEIGHT;
  rect_t titlebar = {SH, SH, win->width, tb};

  /* 2. Cuerpo opaco con esquinas redondeadas */
  gfx_fill_rounded_rect(win->surface, win->surface_w, sc, win_local, R,
                        AURORA_WIN_BG);

  /* 3-5. Barra de título, semáforos y título (solo si el clip los alcanza) */
  if (rect_intersects(sc, titlebar)) {
    gfx_fill_top_rounded_rect(win->surface, win->surface_w, sc, titlebar, R,
                              focused ? AURORA_TITLEBAR_ACTIVE
                                      : AURORA_TITLEBAR_INACTIVE);
    gfx_blend_rect(win->surface, win->surface_w, sc,
                   (rect_t){SH, SH + tb - 1, win->width, 1},
                   AURORA_TITLEBAR_SEP);

    /* Semáforos a la izquierda. Inactiva y sin hover: los tres grises. */
    int show = focused || win->hover_controls;
    int glyph = win->hover_controls;
    draw_traffic_light(win, sc, 0,
                       show ? AURORA_TL_CLOSE : AURORA_TL_INACTIVE, glyph);
    draw_traffic_light(win, sc, 1,
                       show ? AURORA_TL_MINIMIZE : AURORA_TL_INACTIVE, glyph);
    draw_traffic_light(win, sc, 2,
                       show ? AURORA_TL_MAXIMIZE : AURORA_TL_INACTIVE, glyph);

    /* Título centrado (cap-height centrada con las métricas de la fuente).
     * El hueco de los semáforos se reserva simétrico a ambos lados. */
    int side = WIN_TL_X + 2 * WIN_TL_PITCH + WIN_TL_DIAM + 12;
    int max_w = win->width - 2 * side;
    if (max_w > 8) {
      char buf[64];
      fit_title(win->title, buf, sizeof(buf), max_w, FONT_ID_TITLEBAR);
      int tw = gfx_text_width(buf, FONT_ID_TITLEBAR);
      int tx = SH + (win->width - tw) / 2;
      int ty = gfx_text_center_y(FONT_ID_TITLEBAR, SH, tb);
      gfx_draw_string(win->surface, win->surface_w, sc, tx, ty, buf,
                      focused ? AURORA_TEXT_PRIMARY : AURORA_TEXT_TERTIARY,
                      FONT_ID_TITLEBAR);
    }
  }

  /* 6. Contenido cliente */
  gfx_bit_blat_rounded(win->surface, win->surface_w, win->content_buffer,
                       win->content_w, win->content_h, sc, SH, SH + tb, SH, SH,
                       win->width, win->height, R);

  /* 7. Borde fino y luz superior */
  gfx_draw_rounded_border(win->surface, win->surface_w, sc, win_local, R,
                          focused ? AURORA_BORDER_ACTIVE
                                  : AURORA_BORDER_INACTIVE);
  gfx_blend_rect(win->surface, win->surface_w, sc,
                 (rect_t){SH + R, SH, win->width - R * 2, 1},
                 focused ? 0x40FFFFFF : 0x20FFFFFF);
}

void window_redraw_surface(window_t *win) {
  if (!win || !win->surface || (win->flags & WIN_FLAGS_HIDDEN))
    return;
  if (!win->dirty && !win->content_dirty)
    return;

  if (win->dirty) { /* completo */
    win->dirty = 0;
    win->content_dirty = 0;
    draw_window(win, (rect_t){0, 0, win->surface_w, win->surface_h});
  } else { /* solo contenido */
    win->content_dirty = 0;
    int tb = window_titlebar_h(win);
    draw_window(win, (rect_t){SH, SH + tb, win->width, win->height - tb});
  }
}

void window_render_frame(window_t *win, uint32_t *dst, int dst_stride,
                         rect_t clip) {
  if (!win || !dst || !win->surface || (win->flags & WIN_FLAGS_HIDDEN))
    return;

  if (win->dirty || win->content_dirty)
    window_redraw_surface(win);

  int surf_x = win->x - SH;
  int surf_y = win->y - SH + win->anim_dy;

  if (win->anim_alpha >= 255) {
    gfx_bit_blat(dst, dst_stride, win->surface, win->surface_w, clip, surf_x,
                 surf_y, win->surface_w, win->surface_h);
  } else if (win->anim_alpha > 0) {
    gfx_blit_with_alpha(dst, dst_stride, win->surface, win->surface_w, clip,
                        surf_x, surf_y, win->surface_w, win->surface_h,
                        (uint8_t)win->anim_alpha);
  }
}

// ---------------------------------------------------------------------------
// Dibujo en el contenido (API para apps). Solo marcan content_dirty.
// ---------------------------------------------------------------------------
void win_clear(window_t *win, uint32_t color) {
  if (!win || !win->content_buffer)
    return;
  size_t total_pixels = (size_t)win->content_w * win->content_h;
  for (size_t i = 0; i < total_pixels; i++)
    win->content_buffer[i] = color;
  win->content_dirty = 1;
}

void win_draw_rect(window_t *win, int x, int y, int w, int h, uint32_t color) {
  if (!win || !win->content_buffer)
    return;
  rect_t win_clip = {0, 0, win->content_w, win->content_h};
  gfx_blend_rect(win->content_buffer, win->content_w, win_clip,
                 (rect_t){x, y, w, h}, color);
  win->content_dirty = 1;
}

void win_draw_rounded_rect(window_t *win, int x, int y, int w, int h,
                           int radius, uint32_t color) {
  if (!win || !win->content_buffer)
    return;
  rect_t win_clip = {0, 0, win->content_w, win->content_h};
  gfx_fill_rounded_rect(win->content_buffer, win->content_w, win_clip,
                        (rect_t){x, y, w, h}, radius, color);
  win->content_dirty = 1;
}

void win_draw_string(window_t *win, int x, int y, const char *str,
                     uint32_t color, font_id_t font) {
  if (!win || !win->content_buffer || !str)
    return;
  rect_t win_clip = {0, 0, win->content_w, win->content_h};
  gfx_draw_string(win->content_buffer, win->content_w, win_clip, x, y, str,
                  color, font);
  win->content_dirty = 1;
}

void win_draw_string_aligned(window_t *win, rect_t rect, const char *str,
                             uint32_t color, font_id_t font, uint32_t align) {
  if (!win || !str)
    return;
  /* Métricas reales de la fuente (antes: 8 px por carácter, solo válido
   * para una fuente monoespaciada). */
  int text_w = gfx_text_width(str, font);
  int x = rect.x;
  int y = rect.y;

  if (align & ALIGN_CENTER_H)
    x = rect.x + (rect.w - text_w) / 2;
  else if (align & ALIGN_RIGHT)
    x = rect.x + rect.w - text_w;

  if (align & ALIGN_CENTER_V)
    y = gfx_text_center_y(font, rect.y, rect.h);
  else if (align & ALIGN_BOTTOM)
    y = rect.y + rect.h - gfx_font_ascent(font) - 3;

  win_draw_string(win, x, y, str, color, font);
}

void win_draw_image(window_t *win, int x, int y, int w, int h,
                    const uint32_t *img_data) {
  if (!win || !win->content_buffer || !img_data)
    return;
  rect_t win_clip = {0, 0, win->content_w, win->content_h};
  gfx_bit_blat(win->content_buffer, win->content_w, img_data, w, win_clip, x, y,
               w, h);
  win->content_dirty = 1;
}

void win_draw_image_scaled(window_t *win, int dst_x, int dst_y, int dst_w,
                           int dst_h, const uint32_t *img_data, int src_w,
                           int src_h) {
  if (!win || !win->content_buffer || !img_data || dst_w <= 0 || dst_h <= 0)
    return;
  rect_t win_clip = {0, 0, win->content_w, win->content_h};
  gfx_blit_scaled(win->content_buffer, win->content_w, win_clip, dst_x, dst_y,
                  dst_w, dst_h, img_data, src_w, src_h, GFX_FILTER_AUTO);
  win->content_dirty = 1;
}

void win_draw_image_full(window_t *win, const uint32_t *img_data, int src_w,
                         int src_h) {
  win_draw_image_scaled(win, 0, 0, win->content_w, win->content_h, img_data,
                        src_w, src_h);
}

void win_update(window_t *win) {
  if (!win)
    return;
  win->content_dirty = 1;
  compositor_invalidate_rect((rect_t){win->x - SH, win->y - SH,
                                      win->width + SH * 2,
                                      win->height + SH * 2});
}

// ---------------------------------------------------------------------------
// Animaciones (sin cambios de comportamiento)
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Resize atómico: se alocan ambos buffers antes de tocar la ventana, y el
// intercambio de punteros/dimensiones ocurre bajo content_lock para que
// winsrv_blit (otra CPU) nunca vea un buffer liberado ni dimensiones a medias.
// Solo se llama desde el hilo del compositor.
// ---------------------------------------------------------------------------
void window_resize(window_t *win, int new_w, int new_h) {
  if (!win)
    return;
  if (new_w < 200 || new_h < 100)
    return;
  if (new_w > 2048 || new_h > 2048)
    return;
  if (new_w == win->width && new_h == win->height)
    return;

  int tb = window_titlebar_h(win);
  int new_content_w = new_w;
  int new_content_h = new_h - tb;
  if (new_content_w <= 0 || new_content_h <= 0)
    return;

  size_t new_buf_size =
      (size_t)new_content_w * new_content_h * sizeof(uint32_t);
  int new_surf_w = new_w + SH * 2;
  int new_surf_h = new_h + SH * 2;
  size_t new_surf_bytes = (size_t)new_surf_w * new_surf_h * sizeof(uint32_t);

  uint32_t *new_content = (uint32_t *)kmalloc(new_buf_size);
  if (!new_content)
    return;
  uint32_t *new_surface = (uint32_t *)kmalloc(new_surf_bytes);
  if (!new_surface) {
    kfree(new_content);
    return;
  }

  /* Copia del contenido antiguo (bounded). Puede perder un blit concurrente
   * en curso; el cliente repinta al recibir WINSRV_EV_RESIZE. */
  int copy_w = new_content_w < win->content_w ? new_content_w : win->content_w;
  int copy_h = new_content_h < win->content_h ? new_content_h : win->content_h;
  for (int y = 0; y < new_content_h; y++) {
    uint32_t *dst_row = &new_content[y * new_content_w];
    for (int x = 0; x < new_content_w; x++) {
      uint32_t c = AURORA_SURFACE_CARD;
      if (y < copy_h && x < copy_w)
        c = win->content_buffer[y * win->content_w + x];
      dst_row[x] = c;
    }
  }

  unsigned long fl = spin_lock_irqsave(&win->content_lock);
  uint32_t *old_content = win->content_buffer;
  uint32_t *old_surface = win->surface;
  win->content_buffer = new_content;
  win->content_w = new_content_w;
  win->content_h = new_content_h;
  win->surface = new_surface;
  win->surface_w = new_surf_w;
  win->surface_h = new_surf_h;
  win->width = new_w;
  win->height = new_h;
  spin_unlock_irqrestore(&win->content_lock, fl);

  kfree(old_content);
  kfree(old_surface);

  win->dirty = 1;
  window_redraw_surface(win);
}
