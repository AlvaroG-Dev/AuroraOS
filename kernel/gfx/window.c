// kernel/gfx/window.c
#include "window.h"
#include "../bmp.h"
#include "../heap.h"
#include "../klog.h"
#include "../simd.h"
#include "compositor.h"
#include "font_manager.h"
#include "gfx_filter.h"
#include "taskbar.h"
#include "theme.h"
#include <stddef.h>

#define SH AURORA_SHADOW_SIZE
#define R AURORA_CORNER_RADIUS

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

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
  const int O = WIN_RESIZE_OUT;
  if (lx < -O || ly < -O || lx >= w->width + O || ly >= w->height + O)
    return WIN_HIT_NONE;

  /* Bordes de redimensionado: unos píxeles dentro y fuera de la ventana. */
  int near_l = lx < WIN_RESIZE_IN;
  int near_r = lx >= w->width - WIN_RESIZE_IN;
  int near_t = ly < WIN_RESIZE_IN_TOP;
  int near_b = ly >= w->height - WIN_RESIZE_IN;
  if (w->flags & WIN_FLAGS_FIXED_SIZE) {
    if (lx < 0 || ly < 0 || lx >= w->width || ly >= w->height)
      return WIN_HIT_NONE;
  } else if (near_l || near_r || near_t || near_b) {
    const int C = WIN_RESIZE_CORNER;
    int cl = lx < C, cr = lx >= w->width - C;
    int ct = ly < C, cb = ly >= w->height - C;
    if ((near_t && cl) || (near_l && ct))
      return WIN_HIT_RESIZE_TL;
    if ((near_t && cr) || (near_r && ct))
      return WIN_HIT_RESIZE_TR;
    if ((near_b && cl) || (near_l && cb))
      return WIN_HIT_RESIZE_BL;
    if ((near_b && cr) || (near_r && cb))
      return WIN_HIT_RESIZE_BR;
    if (near_l)
      return WIN_HIT_RESIZE_L;
    if (near_r)
      return WIN_HIT_RESIZE_R;
    if (near_t)
      return WIN_HIT_RESIZE_T;
    return WIN_HIT_RESIZE_B;
  }

  int tb = window_titlebar_h(w);
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
  win->anim_scale = 256;
  win->pending_destroy = 0;
  win->genie_to = (rect_t){0, 0, 0, 0};
  win->minimized = 0;
  win->last_vis = (rect_t){0, 0, 0, 0};

  win->hover_btn = 0;
  win->hover_controls = 0;
  win->pressed_btn = 0;

  win->maximized = 0;
  win->pending_max = 0;
  win->restore_x = win->restore_y = win->restore_w = win->restore_h = 0;
  spin_init(&win->content_lock);
  win->content_dmg = (rect_t){0, 0, 0, 0};
  win->chrome_dmg = (rect_t){0, 0, 0, 0};

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

/* Oscurece un color opaco (amt 0..256 de negro). */
static uint32_t darken(uint32_t c, int amt) {
  uint32_t r = ((c >> 16) & 0xFF) * (256 - amt) >> 8;
  uint32_t g = ((c >> 8) & 0xFF) * (256 - amt) >> 8;
  uint32_t b = (c & 0xFF) * (256 - amt) >> 8;
  return (c & 0xFF000000) | (r << 16) | (g << 8) | b;
}

/* Semáforo i (0 cerrar, 1 minimizar, 2 zoom): círculo con antialiasing y un
 * aro algo más oscuro; al pasar el ratón por el grupo muestra su glifo en el
 * tono oscuro del propio botón (×, −, flechas de pantalla completa). */
static void draw_traffic_light(window_t *win, rect_t s_clip, int i,
                               uint32_t color, int glyph, int pressed) {
  uint32_t *s = win->surface;
  int st = win->surface_w;
  float cx = (float)(SH + tl_cx(i));
  float cy = (float)(SH + AURORA_TITLEBAR_HEIGHT / 2);
  float r = WIN_TL_DIAM / 2.0f;

  gfx_fill_circle(s, st, s_clip, cx, cy, r, darken(color, 46));
  gfx_fill_circle(s, st, s_clip, cx, cy, r - 0.8f, color);
  if (pressed)
    gfx_fill_circle(s, st, s_clip, cx, cy, r, AURORA_TL_PRESSED);
  if (!glyph)
    return;

  if (i == 0) {
    uint32_t g = AURORA_TL_GLYPH_CLOSE;
    gfx_draw_capsule(s, st, s_clip, cx - 2.6f, cy - 2.6f, cx + 2.6f,
                     cy + 2.6f, 0.85f, g);
    gfx_draw_capsule(s, st, s_clip, cx - 2.6f, cy + 2.6f, cx + 2.6f,
                     cy - 2.6f, 0.85f, g);
  } else if (i == 1) {
    gfx_draw_capsule(s, st, s_clip, cx - 3.4f, cy, cx + 3.4f, cy, 0.85f,
                     AURORA_TL_GLYPH_MINIMIZE);
  } else { /* dos triángulos con el ángulo recto hacia las esquinas */
    uint32_t g = AURORA_TL_GLYPH_MAXIMIZE;
    gfx_fill_triangle(s, st, s_clip, cx - 3.3f, cy - 3.3f, cx + 1.2f,
                      cy - 3.3f, cx - 3.3f, cy + 1.2f, g);
    gfx_fill_triangle(s, st, s_clip, cx + 3.3f, cy + 3.3f, cx - 1.2f,
                      cy + 3.3f, cx + 3.3f, cy - 1.2f, g);
  }
}

// ---------------------------------------------------------------------------
// Tubería de dibujo única, limitada por clip. El redibujado completo usa el
// clip de toda la superficie; el de contenido, solo la zona que cambió. Como
// ambos recorren exactamente las mismas etapas, el camino barato produce
// píxeles idénticos al completo (incluidas las esquinas redondeadas, donde
// mezclar encima de lo ya dibujado acumularía borde). El coste es
// proporcional al área del clip.
// ---------------------------------------------------------------------------
static void draw_window(window_t *win, rect_t s_clip) {
  rect_t sc = rect_clip(s_clip, (rect_t){0, 0, win->surface_w, win->surface_h});
  if (sc.w <= 0 || sc.h <= 0)
    return;

  for (int y = sc.y; y < sc.y + sc.h; y++)
    simd_fill_row(&win->surface[y * win->surface_w + sc.x], 0x00000000,
                  (size_t)sc.w);

  rect_t win_local = {SH, SH, win->width, win->height};
  int focused = (win->flags & WIN_FLAGS_FOCUSED) ? 1 : 0;

  /* 1. Sombra (más tenue en ventanas inactivas) */
  gfx_draw_shadow_aurora_ex(win->surface, win->surface_w, sc, win_local, R,
                            focused ? 256 : AURORA_SHADOW_INACTIVE);

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

    /* Semáforos a la izquierda. Inactiva y sin hover: los tres grises. */
    int show = focused || win->hover_controls;
    int glyph = win->hover_controls;
    static const uint32_t k_tl[3] = {AURORA_TL_CLOSE, AURORA_TL_MINIMIZE,
                                     AURORA_TL_MAXIMIZE};
    static const int k_btn_id[3] = {3, 1, 2}; /* ids de hover/pressed */
    for (int i = 0; i < 3; i++) {
      /* Zoom deshabilitado en ventanas de tamaño fijo: gris y sin glifo. */
      int off = (i == 2) && (win->flags & WIN_FLAGS_FIXED_SIZE);
      draw_traffic_light(win, sc, i,
                         show && !off ? k_tl[i] : AURORA_TL_INACTIVE,
                         glyph && !off, !off && win->pressed_btn == k_btn_id[i]);
    }

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
                      focused ? AURORA_TEXT_SECONDARY : AURORA_TEXT_TERTIARY,
                      FONT_ID_TITLEBAR);
    }

    /* Línea fina que separa la barra del contenido. */
    gfx_blend_rect(win->surface, win->surface_w, sc,
                   (rect_t){SH, SH + tb - 1, win->width, 1},
                   AURORA_TITLEBAR_HAIRLINE);
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
                 focused ? 0x38FFFFFF : 0x1CFFFFFF);
}

static inline int surface_needs_redraw(const window_t *win) {
  return win->dirty || win->content_dirty || win->chrome_dmg.w > 0;
}

/* Zona de los tres semáforos en coordenadas de la superficie. */
static rect_t controls_surface_rect(void) {
  int tb = AURORA_TITLEBAR_HEIGHT;
  return (rect_t){SH + WIN_TL_X - 3, SH + (tb - WIN_TL_DIAM) / 2 - 3,
                  2 * WIN_TL_PITCH + WIN_TL_DIAM + 6, WIN_TL_DIAM + 6};
}

rect_t window_invalidate_controls(window_t *win) {
  if (!win || (win->flags & WIN_FLAGS_NODECORATION))
    return (rect_t){0, 0, 0, 0};
  rect_t c = controls_surface_rect();
  win->chrome_dmg = rect_bounding_box(win->chrome_dmg, c);
  return (rect_t){win->x - SH + c.x, win->y - SH + c.y + win->anim_dy, c.w,
                  c.h};
}

void window_redraw_surface(window_t *win) {
  if (!win || !win->surface || (win->flags & WIN_FLAGS_HIDDEN))
    return;
  if (!surface_needs_redraw(win))
    return;

  rect_t chrome = win->chrome_dmg;
  win->chrome_dmg = (rect_t){0, 0, 0, 0};

  /* Se recoge (y se limpia) el daño de contenido antes de dibujar: un blit
   * que llegue mientras dibujamos deja su propio daño para el siguiente
   * frame. */
  unsigned long fl = spin_lock_irqsave(&win->content_lock);
  rect_t d = win->content_dmg;
  int content = win->content_dirty;
  win->content_dmg = (rect_t){0, 0, 0, 0};
  win->content_dirty = 0;
  spin_unlock_irqrestore(&win->content_lock, fl);

  if (win->dirty) { /* completo */
    win->dirty = 0;
    draw_window(win, (rect_t){0, 0, win->surface_w, win->surface_h});
    return;
  }

  if (chrome.w > 0 && chrome.h > 0)
    draw_window(win, chrome);
  if (!content)
    return;

  /* Contenido: la zona dañada, o todo el área si no se sabe cuál. */
  int tb = window_titlebar_h(win);
  rect_t area = {0, 0, win->content_w, win->content_h};
  d = (d.w > 0 && d.h > 0) ? rect_clip(d, area) : area;
  if (d.w <= 0 || d.h <= 0)
    return;
  draw_window(win, (rect_t){SH + d.x, SH + tb + d.y, d.w, d.h});
}

void window_mark_content_locked(window_t *win, rect_t r) {
  if (r.w <= 0 || r.h <= 0)
    return;
  win->content_dmg = rect_bounding_box(win->content_dmg, r);
  win->content_dirty = 1;
}

static void mark_content_full(window_t *win) {
  unsigned long fl = spin_lock_irqsave(&win->content_lock);
  window_mark_content_locked(win,
                             (rect_t){0, 0, win->content_w, win->content_h});
  spin_unlock_irqrestore(&win->content_lock, fl);
}

/* Rectángulo de la superficie escalada `s256` alrededor del centro de la
 * ventana, en coordenadas de pantalla. */
static rect_t scaled_surface_rect(const window_t *win, int s256) {
  int sw = (win->surface_w * s256 + 255) >> 8;
  int sh = (win->surface_h * s256 + 255) >> 8;
  int cx = win->x + win->width / 2;
  int cy = win->y + win->height / 2 + win->anim_dy;
  return (rect_t){cx - sw / 2, cy - sh / 2, sw, sh};
}

static int is_genie(const window_t *win) {
  return win->anim_state == WIN_ANIM_MINIMIZING ||
         win->anim_state == WIN_ANIM_RESTORING;
}

rect_t window_visual_rect(const window_t *win) {
  if (!win || (win->flags & WIN_FLAGS_HIDDEN))
    return (rect_t){0, 0, 0, 0};
  if (is_genie(win))
    return rect_bounding_box((rect_t){win->x, win->y, win->width, win->height},
                             rect_grow(win->genie_to, 2));
  if (win->anim_scale < 256)
    return rect_grow(scaled_surface_rect(win, win->anim_scale), 1);
  return (rect_t){win->x - SH, win->y - SH + win->anim_dy, win->surface_w,
                  win->surface_h};
}

int window_covers(const window_t *win, rect_t r) {
  if (!win || (win->flags & (WIN_FLAGS_HIDDEN | WIN_FLAGS_NODECORATION)) ||
      win->anim_state != WIN_ANIM_NONE || win->anim_alpha < 255)
    return 0;
  /* Opaco: todo salvo las esquinas redondeadas (dos rectángulos en cruz). */
  rect_t tall = {win->x + R, win->y, win->width - 2 * R, win->height};
  rect_t wide = {win->x, win->y + R, win->width, win->height - 2 * R};
  return rect_contains(tall, r) || rect_contains(wide, r);
}

// ---------------------------------------------------------------------------
// Efecto "genie" de macOS: la ventana se estrecha por abajo hacia el icono del
// Dock formando un embudo curvo y después se desliza dentro de él. p = 0 es
// la ventana en su sitio, p = 1 la ventana dentro del icono. Cada fila de
// destino toma una fila de la ventana (muestreo al más cercano) estirada entre
// los dos bordes del embudo, con antialiasing en esos bordes.
// ---------------------------------------------------------------------------
static inline float clamp01(float v) {
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}
static inline float smooth01(float t) {
  t = clamp01(t);
  return t * t * (3.0f - 2.0f * t);
}

#define GENIE_ROW_MAX 4096
static uint32_t g_genie_row[GENIE_ROW_MAX]; /* solo hilo del compositor */

static void render_genie(window_t *win, uint32_t *dst, int stride,
                         rect_t clip) {
  rect_t T = win->genie_to;
  float p = (float)win->anim_t / 256.0f;
  float wx = (float)win->x, wy = (float)win->y;
  float ww = (float)win->width, wh = (float)win->height;
  float tx = (float)T.x, ty = (float)T.y, tw = (float)T.w, th = (float)T.h;

  float squeeze = smooth01(p / 0.45f);       /* el embudo se forma */
  float eb = smooth01(p / 0.75f);            /* el borde inferior baja */
  float et = smooth01((p - 0.22f) / 0.78f);  /* el superior le sigue */
  float top = wy + (ty - wy) * et;
  float bot = (wy + wh) + ((ty + th) - (wy + wh)) * eb;
  if (bot - top < 1.0f)
    return;
  float span = (ty + th) - wy;
  if (span < 40.0f)
    span = 40.0f;
  /* Al final se funde dentro del icono. */
  uint32_t fade = (uint32_t)(255.0f * (1.0f - clamp01((p - 0.82f) / 0.18f)));
  if (fade == 0)
    return;

  int y0 = imax((int)top, clip.y);
  int y1 = imin((int)(bot + 1.0f), clip.y + clip.h);
  const uint32_t *src_base = &win->surface[SH * win->surface_w + SH];
  for (int y = y0; y < y1; y++) {
    float fy = (float)y + 0.5f;
    int sy = (int)((fy - top) / (bot - top) * wh);
    if (sy < 0 || sy >= win->height)
      continue;
    float f = smooth01((fy - wy) / span) * squeeze;
    float L = wx + (tx - wx) * f;
    float Rr = (wx + ww) + ((tx + tw) - (wx + ww)) * f;
    if (Rr - L < 0.5f)
      continue;
    int x0 = imax((int)L, clip.x);
    int x1 = imin((int)(Rr + 1.0f), clip.x + clip.w);
    if (x1 - x0 <= 0 || x1 - x0 > GENIE_ROW_MAX)
      continue;

    const uint32_t *srow = &src_base[sy * win->surface_w];
    /* u en 16.16: posición en la fila fuente del centro de cada píxel. */
    int64_t step = (int64_t)(ww / (Rr - L) * 65536.0f);
    int64_t u = (int64_t)((((float)x0 + 0.5f) - L) * ww / (Rr - L) * 65536.0f);
    for (int x = x0; x < x1; x++, u += step) {
      int sx = (int)(u >> 16);
      sx = sx < 0 ? 0 : (sx >= win->width ? win->width - 1 : sx);
      uint32_t c = srow[sx];
      uint32_t a = (c >> 24) * fade / 255;
      /* Cobertura del píxel por el tramo [L, Rr] (bordes suaves). */
      float cov = 1.0f;
      if ((float)x < L)
        cov -= L - (float)x;
      if ((float)(x + 1) > Rr)
        cov -= (float)(x + 1) - Rr;
      if (cov < 1.0f)
        a = (uint32_t)((float)a * clamp01(cov));
      g_genie_row[x - x0] = (a << 24) | (c & 0x00FFFFFF);
    }
    simd_blit_row(&dst[y * stride + x0], g_genie_row, x1 - x0);
  }
}

/* Blit de la superficie a 1:1. El cuerpo de una ventana decorada es opaco:
 * en cada fila, el tramo que cae dentro de la forma (todo el ancho en las
 * filas centrales, sin las esquinas en las bandas superior e inferior) se
 * copia tal cual y solo la sombra y las esquinas se mezclan. Es lo que más
 * pesa al arrastrar una ventana grande. */
static void blit_surface(const window_t *win, uint32_t *dst, int dst_stride,
                         rect_t clip, int surf_x, int surf_y) {
  rect_t r = rect_clip(clip, (rect_t){surf_x, surf_y, win->surface_w,
                                      win->surface_h});
  if (r.w <= 0 || r.h <= 0)
    return;
  int opaque = !(win->flags & WIN_FLAGS_NODECORATION);
  int wx = surf_x + SH, wy = surf_y + SH;
  for (int y = r.y; y < r.y + r.h; y++) {
    uint32_t *drow = &dst[y * dst_stride];
    /* srow[x] = píxel de la superficie bajo la columna de pantalla x. */
    const uint32_t *sbase = &win->surface[(y - surf_y) * win->surface_w];
#define srow(x) (&sbase[(x) - surf_x])
    int o0 = 0, o1 = 0; /* tramo opaco [o0, o1) en coordenadas de pantalla */
    if (opaque && y >= wy && y < wy + win->height) {
      int band = (y < wy + R) || (y >= wy + win->height - R);
      o0 = band ? wx + R : wx;
      o1 = band ? wx + win->width - R : wx + win->width;
    }
    o0 = imax(o0, r.x);
    o1 = imin(o1, r.x + r.w);
    if (o0 >= o1) {
      simd_blit_row(&drow[r.x], srow(r.x), r.w);
      continue;
    }
    if (o0 > r.x)
      simd_blit_row(&drow[r.x], srow(r.x), o0 - r.x);
    simd_copy_normal(&drow[o0], srow(o0), (size_t)(o1 - o0));
    if (r.x + r.w > o1)
      simd_blit_row(&drow[o1], srow(o1), r.x + r.w - o1);
#undef srow
  }
}

void window_render_frame(window_t *win, uint32_t *dst, int dst_stride,
                         rect_t clip) {
  if (!win || !dst || !win->surface || (win->flags & WIN_FLAGS_HIDDEN))
    return;

  if (surface_needs_redraw(win))
    window_redraw_surface(win);

  if (is_genie(win)) {
    render_genie(win, dst, dst_stride, clip);
    return;
  }

  if (win->anim_scale < 256) {
    rect_t r = scaled_surface_rect(win, win->anim_scale);
    if (win->anim_alpha > 0)
      gfx_blit_scaled_alpha(dst, dst_stride, clip, r.x, r.y, r.w, r.h,
                            win->surface, win->surface_w, win->surface_h,
                            GFX_FILTER_BILINEAR, (uint8_t)win->anim_alpha);
    return;
  }

  int surf_x = win->x - SH;
  int surf_y = win->y - SH + win->anim_dy;

  if (win->anim_alpha >= 255) {
    blit_surface(win, dst, dst_stride, clip, surf_x, surf_y);
  } else if (win->anim_alpha > 0) {
    gfx_blit_with_alpha(dst, dst_stride, win->surface, win->surface_w, clip,
                        surf_x, surf_y, win->surface_w, win->surface_h,
                        (uint8_t)win->anim_alpha);
  }
}

// ---------------------------------------------------------------------------
// Dibujo en el contenido (API para apps). Solo marcan el contenido como
// cambiado.
// ---------------------------------------------------------------------------
void win_clear(window_t *win, uint32_t color) {
  if (!win || !win->content_buffer)
    return;
  size_t total_pixels = (size_t)win->content_w * win->content_h;
  for (size_t i = 0; i < total_pixels; i++)
    win->content_buffer[i] = color;
  mark_content_full(win);
}

void win_draw_rect(window_t *win, int x, int y, int w, int h, uint32_t color) {
  if (!win || !win->content_buffer)
    return;
  rect_t win_clip = {0, 0, win->content_w, win->content_h};
  gfx_blend_rect(win->content_buffer, win->content_w, win_clip,
                 (rect_t){x, y, w, h}, color);
  mark_content_full(win);
}

void win_draw_rounded_rect(window_t *win, int x, int y, int w, int h,
                           int radius, uint32_t color) {
  if (!win || !win->content_buffer)
    return;
  rect_t win_clip = {0, 0, win->content_w, win->content_h};
  gfx_fill_rounded_rect(win->content_buffer, win->content_w, win_clip,
                        (rect_t){x, y, w, h}, radius, color);
  mark_content_full(win);
}

void win_draw_string(window_t *win, int x, int y, const char *str,
                     uint32_t color, font_id_t font) {
  if (!win || !win->content_buffer || !str)
    return;
  rect_t win_clip = {0, 0, win->content_w, win->content_h};
  gfx_draw_string(win->content_buffer, win->content_w, win_clip, x, y, str,
                  color, font);
  mark_content_full(win);
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
  mark_content_full(win);
}

void win_draw_image_scaled(window_t *win, int dst_x, int dst_y, int dst_w,
                           int dst_h, const uint32_t *img_data, int src_w,
                           int src_h) {
  if (!win || !win->content_buffer || !img_data || dst_w <= 0 || dst_h <= 0)
    return;
  rect_t win_clip = {0, 0, win->content_w, win->content_h};
  gfx_blit_scaled(win->content_buffer, win->content_w, win_clip, dst_x, dst_y,
                  dst_w, dst_h, img_data, src_w, src_h, GFX_FILTER_AUTO);
  mark_content_full(win);
}

void win_draw_image_full(window_t *win, const uint32_t *img_data, int src_w,
                         int src_h) {
  win_draw_image_scaled(win, 0, 0, win->content_w, win->content_h, img_data,
                        src_w, src_h);
}

void win_update(window_t *win) {
  if (!win)
    return;
  mark_content_full(win);
  compositor_invalidate_rect((rect_t){win->x - SH, win->y - SH,
                                      win->width + SH * 2,
                                      win->height + SH * 2});
}

// ---------------------------------------------------------------------------
// Animaciones. anim_t avanza linealmente con el tiempo; cada tipo deriva de
// él su curva:
//   abrir      : escala WIN_ANIM_OPEN_SCALE -> 1 y fundido de entrada
//   cerrar     : escala 1 -> WIN_ANIM_CLOSE_SCALE y fundido de salida
//   minimizar  : genie hacia el Dock (anim_t 0 -> 256)
//   restaurar  : genie inverso (anim_t 256 -> 0)
// ---------------------------------------------------------------------------
void window_start_open_animation(window_t *win) {
  if (!win)
    return;
  win->anim_state = WIN_ANIM_OPENING;
  win->anim_t = 0;
  win->anim_alpha = 0;
  win->anim_dy = 0;
  win->anim_scale = WIN_ANIM_OPEN_SCALE;
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
  win->anim_scale = 256;
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

  int duration;
  switch (win->anim_state) {
  case WIN_ANIM_OPENING:
    duration = WIN_ANIM_OPEN_MS;
    break;
  case WIN_ANIM_CLOSING:
    duration = WIN_ANIM_CLOSE_MS;
    break;
  case WIN_ANIM_MINIMIZING:
    duration = WIN_ANIM_MINIMIZE_MS;
    break;
  default:
    duration = WIN_ANIM_RESTORE_MS;
    break;
  }
  int delta = imax(2, (256 * dt_ms) / imax(duration, 1));

  switch (win->anim_state) {
  case WIN_ANIM_OPENING: {
    win->anim_t = imin(win->anim_t + delta, 256);
    int e = ease_out_cubic_fp(win->anim_t);
    win->anim_alpha = (e * 255 + 128) / 256;
    win->anim_scale =
        WIN_ANIM_OPEN_SCALE + ((256 - WIN_ANIM_OPEN_SCALE) * e) / 256;
    if (win->anim_t >= 256) {
      win->anim_state = WIN_ANIM_NONE;
      win->anim_alpha = 255;
      win->anim_scale = 256;
    }
    break;
  }
  case WIN_ANIM_CLOSING: {
    win->anim_t = imax(win->anim_t - delta, 0);
    int e = ease_out_cubic_fp(256 - win->anim_t);
    win->anim_alpha = 255 - (e * 255 + 128) / 256;
    win->anim_scale = 256 - ((256 - WIN_ANIM_CLOSE_SCALE) * e) / 256;
    if (win->anim_t <= 0) {
      win->anim_state = WIN_ANIM_NONE;
      win->anim_alpha = 0;
      win->anim_scale = 256;
    }
    break;
  }
  case WIN_ANIM_MINIMIZING:
    win->anim_t = imin(win->anim_t + delta, 256);
    if (win->anim_t >= 256) {
      win->anim_state = WIN_ANIM_NONE;
      win->anim_alpha = 0; /* el compositor la oculta */
    }
    break;
  case WIN_ANIM_RESTORING:
    win->anim_t = imax(win->anim_t - delta, 0);
    if (win->anim_t <= 0) {
      win->anim_state = WIN_ANIM_NONE;
      win->anim_alpha = 255;
    }
    break;
  }
}

/* Destino del genie: el icono del Dock, o (sin icono) un punto bajo la
 * ventana al pie de la pantalla. */
static rect_t genie_target(const window_t *win, rect_t target) {
  if (target.w > 0 && target.h > 0)
    return target;
  rect_t wa = compositor_work_area();
  int cx = win->x + win->width / 2;
  return (rect_t){cx - 24, wa.y + wa.h, 48, 48};
}

void window_start_minimize_animation(window_t *win, rect_t target) {
  if (!win)
    return;
  if (win->anim_state == WIN_ANIM_CLOSING ||
      win->anim_state == WIN_ANIM_MINIMIZING)
    return;
  /* Si se estaba restaurando, sigue desde el punto en que iba. */
  int from = win->anim_state == WIN_ANIM_RESTORING ? win->anim_t : 0;
  win->anim_state = WIN_ANIM_MINIMIZING;
  win->anim_t = from;
  win->anim_alpha = 255;
  win->anim_dy = 0;
  win->anim_scale = 256;
  win->genie_to = genie_target(win, target);
  win->minimized = 1;
}

void window_start_restore_animation(window_t *win, rect_t target) {
  if (!win)
    return;
  int from = win->anim_state == WIN_ANIM_MINIMIZING ? win->anim_t : 256;
  win->anim_state = WIN_ANIM_RESTORING;
  win->anim_t = from;
  win->anim_alpha = 255;
  win->anim_dy = 0;
  win->anim_scale = 256;
  win->genie_to = genie_target(win, target);
  win->minimized = 0;
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
  win->content_dmg = (rect_t){0, 0, 0, 0};
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
