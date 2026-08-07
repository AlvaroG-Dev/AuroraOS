// kernel/gfx/window.c
#include "window.h"
#include "../heap.h"
#include "../bmp.h"
#include "compositor.h"
#include "theme.h"
#include "taskbar.h"
#include <stddef.h>

static void draw_app_icon(window_t *win, uint32_t *dst, int stride, rect_t clip, int x, int y) {
    if (!win) return;
    
    if (win->icon_bmp_node) {
        // Dibuja el BMP de la ventana escalado a 18x18px en el Titlebar
        bmp_draw_scaled(win->icon_bmp_node, dst, stride, clip, x, y, 18, 18);
    } else {
        // Fallback predeterminado con texto/color
        gfx_fill_rounded_rect(dst, stride, clip, (rect_t){x, y, 18, 18}, 4, win->icon_bg_color);
        gfx_draw_string(dst, stride, clip, x + 5, y, win->icon_symbol, 0xFFFFFFFF, FONT_ID_MONO);
    }
}

void win_set_icon_bmp(window_t *win, tar_node_t *bmp_file) {
    if (!win) return;
    win->icon_bmp_node = bmp_file;
    win->dirty = 1;
    
    // Si la ventana tiene un botón asociado en la barra de tareas, actualiza su icono también
    if (win->taskbar_item) {
        win->taskbar_item->icon_bmp_node = bmp_file;
    }
}

window_t *window_create(int x, int y, int w, int h, const char *title, uint32_t flags) {
  window_t *win = (window_t *)kmalloc(sizeof(window_t));
  if (!win) return NULL;

  win->x = x;
  win->y = y;
  win->width = w;
  win->height = h;
  win->flags = (flags | WIN_FLAGS_MOVABLE) & ~WIN_FLAGS_HIDDEN;
  win->z_order = 0;
  win->next = NULL;
  win->prev = NULL;

  win->icon_symbol[0] = '>';
  win->icon_symbol[1] = '\0';
  win->icon_bg_color = 0xFF0078D4;
  win->icon_buffer = NULL;
  win->taskbar_item = NULL;

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

  // Buffer cliente
  size_t buf_size = (size_t)win->content_w * win->content_h * sizeof(uint32_t);
  win->content_buffer = (uint32_t *)kmalloc(buf_size);
  if (!win->content_buffer) {
    kfree(win);
    return NULL;
  }

  // Asignar memoria para el Surface offscreen (Ancho + Sombras)
  win->surface_w = win->width + (WIN11_SHADOW_SIZE * 2);
  win->surface_h = win->height + (WIN11_SHADOW_SIZE * 2);
  size_t surf_bytes = (size_t)win->surface_w * win->surface_h * sizeof(uint32_t);

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

  // Forzar redibujado de la superficie
  win->dirty = 1;
  window_redraw_surface(win);

  return win;
}

void window_set_visible(window_t *win, int visible) {
  if (!win) return;

  int was_visible = !(win->flags & WIN_FLAGS_HIDDEN);
  if (visible) {
    win->flags &= ~WIN_FLAGS_HIDDEN;
  } else {
    win->flags |= WIN_FLAGS_HIDDEN;
  }

  if (was_visible != visible) {
    win->dirty = 1;
    rect_t damage = {win->x - WIN11_SHADOW_SIZE,
                     win->y - WIN11_SHADOW_SIZE,
                     win->width + WIN11_SHADOW_SIZE * 2,
                     win->height + WIN11_SHADOW_SIZE * 2};
    compositor_invalidate_rect(damage);
  }
}

int window_is_visible(window_t *win) {
  return win && !(win->flags & WIN_FLAGS_HIDDEN);
}

void window_destroy(window_t *win) {
  if (!win) return;
  if (win->taskbar_item) {
    taskbar_remove_item(win->taskbar_item->id);
    win->taskbar_item = NULL;
  }
  if (win->content_buffer) kfree(win->content_buffer);
  if (win->surface) kfree(win->surface);
  kfree(win);
}

void window_redraw_surface(window_t *win) {
  if (!win || !win->surface || !win->dirty || (win->flags & WIN_FLAGS_HIDDEN)) return;

  // Limpiar superficie local
  size_t total_p = (size_t)win->surface_w * win->surface_h;
  for (size_t i = 0; i < total_p; i++) {
    win->surface[i] = 0x00000000; // Transparente total
  }

  rect_t s_clip = {0, 0, win->surface_w, win->surface_h};
  rect_t win_local = {WIN11_SHADOW_SIZE, WIN11_SHADOW_SIZE, win->width, win->height};

  // 1. Sombras
  gfx_draw_shadow(win->surface, win->surface_w, s_clip, win_local, WIN11_CORNER_RADIUS, WIN11_SHADOW_SIZE);

  // 2. Fondo
  gfx_fill_rounded_rect(win->surface, win->surface_w, s_clip, win_local, WIN11_CORNER_RADIUS, WIN11_WIN_BG);

  if (!(win->flags & WIN_FLAGS_NODECORATION)) {
    rect_t titlebar = {WIN11_SHADOW_SIZE, WIN11_SHADOW_SIZE, win->width, WIN11_TITLEBAR_HEIGHT};
    uint32_t t_color = (win->flags & WIN_FLAGS_FOCUSED) ? WIN11_TITLEBAR_ACTIVE : WIN11_TITLEBAR_INACTIVE;

    gfx_fill_top_rounded_rect(win->surface, win->surface_w, s_clip, titlebar, WIN11_CORNER_RADIUS, t_color);

    gfx_blend_rect(win->surface, win->surface_w, s_clip,
                   (rect_t){WIN11_SHADOW_SIZE, WIN11_SHADOW_SIZE + WIN11_TITLEBAR_HEIGHT - 1, win->width, 1}, 0x30FFFFFF);

    draw_app_icon(win, win->surface, win->surface_w, s_clip, WIN11_SHADOW_SIZE + 10, WIN11_SHADOW_SIZE + 7);
    gfx_draw_string(win->surface, win->surface_w, s_clip, WIN11_SHADOW_SIZE + 36, WIN11_SHADOW_SIZE + 7, win->title, WIN11_TEXT_PRIMARY, FONT_ID_MONO);

    // Controles
    int btn_w = 46;
    int close_x = WIN11_SHADOW_SIZE + win->width - btn_w;
    int max_x = close_x - btn_w;
    int min_x = max_x - btn_w;

    gfx_draw_line(win->surface, win->surface_w, s_clip, min_x + 18, WIN11_SHADOW_SIZE + 16, min_x + 28, WIN11_SHADOW_SIZE + 16, WIN11_TEXT_PRIMARY);
    gfx_draw_rect_outline(win->surface, win->surface_w, s_clip, (rect_t){max_x + 18, WIN11_SHADOW_SIZE + 11, 10, 10}, WIN11_TEXT_PRIMARY);
    gfx_draw_line(win->surface, win->surface_w, s_clip, close_x + 18, WIN11_SHADOW_SIZE + 11, close_x + 27, WIN11_SHADOW_SIZE + 20, WIN11_TEXT_PRIMARY);
    gfx_draw_line(win->surface, win->surface_w, s_clip, close_x + 18, WIN11_SHADOW_SIZE + 20, close_x + 27, WIN11_SHADOW_SIZE + 11, WIN11_TEXT_PRIMARY);

    // Blit cliente
    gfx_bit_blat_rounded(win->surface, win->surface_w, win->content_buffer, win->content_w, win->content_h,
                         s_clip, WIN11_SHADOW_SIZE, WIN11_SHADOW_SIZE + WIN11_TITLEBAR_HEIGHT,
                         WIN11_SHADOW_SIZE, WIN11_SHADOW_SIZE, win->width, win->height, WIN11_CORNER_RADIUS);
  } else {
    gfx_bit_blat(win->surface, win->surface_w, win->content_buffer, win->content_w, s_clip,
                 WIN11_SHADOW_SIZE, WIN11_SHADOW_SIZE, win->content_w, win->content_h);
  }

  // Borde y Rim Light
  uint32_t b_color = (win->flags & WIN_FLAGS_FOCUSED) ? 0x33FFFFFF : 0x1AFFFFFF;
  gfx_draw_rounded_border(win->surface, win->surface_w, s_clip, win_local, WIN11_CORNER_RADIUS, b_color);
  gfx_blend_rect(win->surface, win->surface_w, s_clip, (rect_t){WIN11_SHADOW_SIZE + WIN11_CORNER_RADIUS, WIN11_SHADOW_SIZE, win->width - (WIN11_CORNER_RADIUS * 2), 1}, 0x40FFFFFF);

  win->dirty = 0;
}

void window_render_frame(window_t *win, uint32_t *dst, int dst_stride, rect_t clip) {
  if (!win || !dst || !win->surface || (win->flags & WIN_FLAGS_HIDDEN)) return;

  if (win->dirty) {
    window_redraw_surface(win);
  }

  // Renderizado instantáneo: Blit directo desde el surface precalculado
  int surf_x = win->x - WIN11_SHADOW_SIZE;
  int surf_y = win->y - WIN11_SHADOW_SIZE;

  gfx_bit_blat(dst, dst_stride, win->surface, win->surface_w, clip, surf_x, surf_y, win->surface_w, win->surface_h);
}

void win_set_icon_text(window_t *win, const char *symbol, uint32_t bg_color) {
  if (!win) return;
  int i = 0;
  while (symbol && symbol[i] && i < 7) { win->icon_symbol[i] = symbol[i]; i++; }
  win->icon_symbol[i] = '\0';
  win->icon_bg_color = bg_color;
  win->icon_buffer = NULL;
  win->dirty = 1;
  // Update associated taskbar item if present
  if (win->taskbar_item) {
    i = 0;
    while (symbol && symbol[i] && i < 7) { win->taskbar_item->icon_symbol[i] = symbol[i]; i++; }
    win->taskbar_item->icon_symbol[i] = '\0';
    win->taskbar_item->icon_color = bg_color;
  }
}

void win_set_icon_image(window_t *win, const uint32_t *icon_18x18) {
  if (!win) return;
  win->icon_buffer = icon_18x18;
  win->dirty = 1;
}

void win_clear(window_t *win, uint32_t color) {
  if (!win || !win->content_buffer) return;
  size_t total_pixels = (size_t)win->content_w * win->content_h;
  for (size_t i = 0; i < total_pixels; i++) {
    win->content_buffer[i] = color;
  }
  win->dirty = 1;
}

void win_draw_rect(window_t *win, int x, int y, int w, int h, uint32_t color) {
  if (!win || !win->content_buffer) return;
  rect_t win_clip = {0, 0, win->content_w, win->content_h};
  gfx_blend_rect(win->content_buffer, win->content_w, win_clip, (rect_t){x, y, w, h}, color);
  win->dirty = 1;
}

void win_draw_rounded_rect(window_t *win, int x, int y, int w, int h, int radius, uint32_t color) {
  if (!win || !win->content_buffer) return;
  rect_t win_clip = {0, 0, win->content_w, win->content_h};
  gfx_fill_rounded_rect(win->content_buffer, win->content_w, win_clip, (rect_t){x, y, w, h}, radius, color);
  win->dirty = 1;
}

void win_draw_string(window_t *win, int x, int y, const char *str, uint32_t color, font_id_t font) {
  if (!win || !win->content_buffer || !str) return;
  rect_t win_clip = {0, 0, win->content_w, win->content_h};
  gfx_draw_string(win->content_buffer, win->content_w, win_clip, x, y, str, color, font);
  win->dirty = 1;
}

void win_draw_string_aligned(window_t *win, rect_t rect, const char *str, uint32_t color, font_id_t font, uint32_t align) {
  if (!win || !str) return;
  int len = 0;
  while (str[len]) len++;
  int text_w = len * 8, text_h = 13;
  int x = rect.x, y = rect.y;

  if (align & ALIGN_CENTER_H) x = rect.x + (rect.w - text_w) / 2;
  else if (align & ALIGN_RIGHT) x = rect.x + rect.w - text_w;

  if (align & ALIGN_CENTER_V) y = rect.y + (rect.h - text_h) / 2;
  else if (align & ALIGN_BOTTOM) y = rect.y + rect.h - text_h;

  win_draw_string(win, x, y, str, color, font);
}

void win_draw_image(window_t *win, int x, int y, int w, int h, const uint32_t *img_data) {
  if (!win || !win->content_buffer || !img_data) return;
  rect_t win_clip = {0, 0, win->content_w, win->content_h};
  gfx_bit_blat(win->content_buffer, win->content_w, img_data, w, win_clip, x, y, w, h);
  win->dirty = 1;
}

void win_draw_image_scaled(window_t *win, int dst_x, int dst_y, int dst_w, int dst_h, const uint32_t *img_data, int src_w, int src_h) {
  if (!win || !win->content_buffer || !img_data || dst_w <= 0 || dst_h <= 0) return;
  for (int dy = 0; dy < dst_h; dy++) {
    int py = dst_y + dy;
    if (py < 0 || py >= win->content_h) continue;
    int sy = (dy * src_h) / dst_h;
    uint32_t *dst_row = &win->content_buffer[py * win->content_w];
    const uint32_t *src_row = &img_data[sy * src_w];

    for (int dx = 0; dx < dst_w; dx++) {
      int px = dst_x + dx;
      if (px < 0 || px >= win->content_w) continue;
      int sx = (dx * src_w) / dst_w;
      dst_row[px] = blend_pixel_fast(src_row[sx], dst_row[px]);
    }
  }
  win->dirty = 1;
}

void win_draw_image_full(window_t *win, const uint32_t *img_data, int src_w, int src_h) {
  win_draw_image_scaled(win, 0, 0, win->content_w, win->content_h, img_data, src_w, src_h);
}

void win_update(window_t *win) {
  if (!win) return;
  win->dirty = 1;
  rect_t damage = {win->x - WIN11_SHADOW_SIZE, win->y - WIN11_SHADOW_SIZE,
                   win->width + WIN11_SHADOW_SIZE * 2, win->height + WIN11_SHADOW_SIZE * 2};
  compositor_invalidate_rect(damage);
}