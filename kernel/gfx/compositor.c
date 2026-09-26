// kernel/gfx/compositor.c
#include "compositor.h"
#include "../bmp.h"
#include "../heap.h"
#include "../input.h"
#include "../klog.h"
#include "../sched.h"
#include "../serial.h"
#include "../simd.h"
#include "../tarfs.h"
#include "../time.h"
#include "../wait.h"
#include "font_manager.h"
#include "gfx.h"
#include "gfx_filter.h"
#include "taskbar.h"
#include "theme.h"
#include "window.h"
#include "winsrv.h"
#include <emmintrin.h>
#include <stddef.h>
#include <stdint.h>

extern uint32_t *fb_ptr;
extern uint32_t fb_width;
extern uint32_t fb_height;
extern uint32_t fb_pitch;

static uint32_t *backbuffer = NULL;
static uint32_t *wallpaper_cache = NULL;
static window_t *window_stack = NULL;
static rect_t global_damage = {0, 0, 0, 0};
static task_t *compositor_task_ref = NULL;

static int cursor_x = 400, cursor_y = 300;

static wait_queue_t compositor_wq = WAIT_QUEUE_INIT(compositor_wq);
static volatile int compositor_has_input = 0;
static volatile int compositor_needs_clock = 0;

static window_t *drag_window = NULL;
static int drag_offset_x = 0;
static int drag_offset_y = 0;

static window_t *pressed_win = NULL;
static window_btn_state_t pressed_btn = WIN_BTN_NONE;
static tar_node_t *bg_wallpaper_node = NULL;
static int last_mouse_buttons = 0;
static void compositor_focus_window(window_t *win);

static spinlock_t compositor_lock;

/* =========== SSE helpers =========== */
static inline void sse_memset32(uint32_t *dest, uint32_t val, size_t count) {
  simd_fill_row(dest, val, count);
}

static inline void sse_memcpy_vram(uint32_t *dest, const uint32_t *src,
                                   size_t count) {
  simd_copy_to_vram(dest, src, count);
}

static void compositor_invalidate_taskbar(void) {
  rect_t bar_rect = taskbar_get_bounds(fb_width, fb_height);
  rect_t damage = {bar_rect.x - 16, bar_rect.y - 16, bar_rect.w + 32,
                   bar_rect.h + 32};
  compositor_invalidate_rect(damage);
}

static inline uint64_t comp_rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}
extern uint64_t klog_get_tsc_freq(void);

/* ============================================================================
 * [NUEVO] Diagnóstico de rendimiento por frame.
 * Acumula ciclos gastados en compositor_render() y loguea la media cada
 * N frames. Con el TSC calibrado, la conversión a ms es exacta. Sirve para
 * saber si el cuello de botella está en el render, en el blend, o en el
 * blit final a VRAM.
 * ==========================================================================*/
#define COMP_FRAME_LOG_INTERVAL 120 /* cada ~2 segundos a 60fps */

static uint64_t g_frame_cycles_sum = 0;
static uint32_t g_frame_count = 0;
static uint64_t g_frame_cycles_max = 0;

static void compositor_report_frame_stats(uint64_t elapsed_cycles) {
  g_frame_cycles_sum += elapsed_cycles;
  if (elapsed_cycles > g_frame_cycles_max)
    g_frame_cycles_max = elapsed_cycles;
  g_frame_count++;

  if (g_frame_count >= COMP_FRAME_LOG_INTERVAL) {
    uint64_t freq = klog_get_tsc_freq();
    if (freq > 0) {
      /* Calcular en microsegundos, enteros. 1 ms = 1000 us. */
      uint64_t avg_us =
          (g_frame_cycles_sum / g_frame_count) * 1000000ULL / freq;
      uint64_t max_us = g_frame_cycles_max * 1000000ULL / freq;
      LOG_DEBUG(
          "[COMP-PERF] render: avg=%u.%03u ms, max=%u.%03u ms (%u frames)",
          (unsigned)(avg_us / 1000), (unsigned)(avg_us % 1000),
          (unsigned)(max_us / 1000), (unsigned)(max_us % 1000),
          (unsigned)g_frame_count);
    }
    g_frame_cycles_sum = 0;
    g_frame_cycles_max = 0;
    g_frame_count = 0;
  }
}

static void taskbar_item_on_click(taskbar_item_t *item) {
  if (!item)
    return;
  window_t *win = window_stack;
  while (win) {
    if (win->taskbar_item == item) {
      if (window_is_visible(win) && (win->flags & WIN_FLAGS_FOCUSED)) {
        compositor_minimize_window(win);
        taskbar_set_active_item(NULL);
        compositor_invalidate_taskbar();
      } else {
        compositor_focus_window(win);
      }
      break;
    }
    win = win->next;
  }
}

static void on_start_click(taskbar_item_t *item) {
  (void)item;
  LOG_INFO("[COMPOSITOR] Botón de Inicio pulsado");
}

static int compositor_has_active_animations(void);
static bool compositor_has_work(void *arg) {
  (void)arg;
  return compositor_has_input || compositor_needs_clock ||
         (global_damage.w > 0 && global_damage.h > 0) ||
         taskbar_has_active_hover() || compositor_has_active_animations();
}

void compositor_notify_event(void) {
  compositor_has_input = 1;
  wake_up_all(&compositor_wq);
}

void compositor_notify_clock_tick(void) {
  compositor_needs_clock = 1;
  wake_up_all(&compositor_wq);
}

void compositor_init(void) {
  spin_init(&compositor_lock);
  font_manager_init();

  compositor_has_input = 0;
  compositor_needs_clock = 0;

  size_t size = (size_t)fb_width * fb_height * sizeof(uint32_t);
  backbuffer = (uint32_t *)kmalloc(size);

  if (!backbuffer) {
    LOG_ERR("[COMPOSITOR] ERROR CRITICO: No se pudo alojar el Backbuffer");
    return;
  }

  global_damage = (rect_t){0, 0, (int)fb_width, (int)fb_height};
  LOG_INFO(
      "[COMPOSITOR] Motor grafico con aceleracion SIMD SSE2 inicializado.");
  taskbar_init();

  winsrv_init();

  tar_node_t *start_icon_node = tar_find_file("system/icons/start-icon.bmp");
  bg_wallpaper_node = tar_find_file("system/wallpapers/wallpaper-main.bmp");

  wallpaper_cache = (uint32_t *)kmalloc(size);
  if (wallpaper_cache) {
    /* [FIX] kmalloc no zeroa. Si el BMP tiene alpha=0 en algunos píxeles
     * (típico de BMPs generados desde JPEG), blend_pixel devuelve el dst
     * sin tocarlo, y ese dst es basura del heap. En QEMU suele ser cero,
     * en VirtualBox es ruido. Inicializamos a negro opaco: los píxeles
     * con alpha=0 quedan negros en vez de basura. */
    for (size_t i = 0; i < (size_t)fb_width * fb_height; i++)
      wallpaper_cache[i] = 0xFF000000;

    int drawn = 0;
    if (bg_wallpaper_node) {
      rect_t full_clip = {0, 0, (int)fb_width, (int)fb_height};
      if (bmp_draw_scaled(bg_wallpaper_node, wallpaper_cache, fb_width,
                          full_clip, 0, 0, fb_width, fb_height) == 0) {
        drawn = 1;
      }
    }
    if (!drawn) {
      for (int y = 0; y < (int)fb_height; y++) {
        int t = (y * 256) / (int)fb_height;
        uint32_t top = 0xFF060612;
        uint32_t bot = 0xFF12102A;
        uint32_t base = ((uint32_t)(((top >> 24) & 0xFF) * (256 - t) / 256 +
                                    ((bot >> 24) & 0xFF) * t / 256)
                         << 24) |
                        ((uint32_t)(((top >> 16) & 0xFF) * (256 - t) / 256 +
                                    ((bot >> 16) & 0xFF) * t / 256)
                         << 16) |
                        ((uint32_t)(((top >> 8) & 0xFF) * (256 - t) / 256 +
                                    ((bot >> 8) & 0xFF) * t / 256)
                         << 8) |
                        (uint32_t)(((top & 0xFF) * (256 - t) / 256 +
                                    (bot & 0xFF) * t / 256));
        sse_memset32(&wallpaper_cache[y * fb_width], base, fb_width);
      }

      struct {
        int yc;
        int amp;
        uint32_t color;
        int thick;
      } bands[] = {
          {(int)fb_height * 28 / 100, 40, 0x1800D4FF, 120},
          {(int)fb_height * 42 / 100, 55, 0x147C4DFF, 140},
          {(int)fb_height * 55 / 100, 70, 0x10FF4DD2, 160},
      };
      for (int b = 0; b < 3; b++) {
        for (int y = 0; y < (int)fb_height; y++) {
          int dy = y - bands[b].yc;
          if (dy < -bands[b].thick || dy > bands[b].thick)
            continue;
          for (int x = 0; x < (int)fb_width; x++) {
            int wave = (x * 6 / (int)fb_width) * 2;
            int offset = (dy + ((x / 32) % 3) * 3 - 3 + wave);
            int ad = offset < 0 ? -offset : offset;
            if (ad > bands[b].thick)
              continue;
            int a = (bands[b].thick - ad) * 255 / bands[b].thick;
            int alpha = ((bands[b].color >> 24) & 0xFF) * a / 255;
            if (alpha <= 1)
              continue;
            uint32_t c =
                ((uint32_t)alpha << 24) | (bands[b].color & 0x00FFFFFF);
            wallpaper_cache[y * fb_width + x] =
                blend_pixel_fast(c, wallpaper_cache[y * fb_width + x]);
          }
        }
      }
    }
  }

  if (start_icon_node) {
    taskbar_add_item_bmp(TASKBAR_ITEM_START_BTN, "Inicio", start_icon_node,
                         on_start_click);
  }
}

void compositor_invalidate_rect(rect_t damage) {
  if (damage.w <= 0 || damage.h <= 0)
    return;
  if (global_damage.w <= 0 || global_damage.h <= 0) {
    global_damage = damage;
  } else {
    global_damage = rect_bounding_box(global_damage, damage);
  }
  wake_up_all(&compositor_wq);
}

window_t *compositor_create_window(int x, int y, int w, int h,
                                   const char *title, uint32_t flags) {
  window_t *win = window_create(x, y, w, h, title, flags);
  if (!win)
    return NULL;

  unsigned long irqflags = spin_lock_irqsave(&compositor_lock);
  win->next = window_stack;
  if (window_stack)
    window_stack->prev = win;
  window_stack = win;
  spin_unlock_irqrestore(&compositor_lock, irqflags);

  rect_t damage = {x - WIN11_SHADOW_SIZE, y - WIN11_SHADOW_SIZE,
                   w + WIN11_SHADOW_SIZE * 2, h + WIN11_SHADOW_SIZE * 2};
  compositor_invalidate_rect(damage);

  taskbar_item_t *tb_item =
      taskbar_add_item(TASKBAR_ITEM_APP_ICON, win->title, win->icon_symbol,
                       win->icon_bg_color, taskbar_item_on_click);
  win->taskbar_item = tb_item;

  compositor_has_input = 1;
  wake_up_all(&compositor_wq);
  return win;
}

void compositor_close_window(window_t *win) {
  if (!win)
    return;

  window_start_close_animation(win);
  win->pending_destroy = 1;

  if (drag_window == win)
    drag_window = NULL;
  if (pressed_win == win) {
    pressed_win = NULL;
    pressed_btn = WIN_BTN_NONE;
  }

  rect_t damage = {
      win->x - WIN11_SHADOW_SIZE, win->y - WIN11_SHADOW_SIZE + win->anim_dy,
      win->width + WIN11_SHADOW_SIZE * 2, win->height + WIN11_SHADOW_SIZE * 2};
  compositor_invalidate_rect(damage);

  compositor_has_input = 1;
  wake_up_all(&compositor_wq);
}

void compositor_minimize_window(window_t *win) {
  if (!win)
    return;

  window_start_minimize_animation(win);
  win->pending_destroy = 0;

  if (win->flags & WIN_FLAGS_FOCUSED) {
    win->flags &= ~WIN_FLAGS_FOCUSED;
    win->flags |= WIN_FLAGS_INACTIVE;
    win->dirty = 1;
    winsrv_post_event(win, WINSRV_EV_BLUR, 0, 0, 0);
    compositor_invalidate_taskbar();
  }

  if (drag_window == win)
    drag_window = NULL;
  if (pressed_win == win) {
    pressed_win = NULL;
    pressed_btn = WIN_BTN_NONE;
  }

  rect_t damage = {
      win->x - WIN11_SHADOW_SIZE, win->y - WIN11_SHADOW_SIZE + win->anim_dy,
      win->width + WIN11_SHADOW_SIZE * 2, win->height + WIN11_SHADOW_SIZE * 2};
  compositor_invalidate_rect(damage);

  compositor_has_input = 1;
  wake_up_all(&compositor_wq);
}

static void compositor_do_close_window(window_t *win) {
  if (!win)
    return;

  unsigned long irqflags = spin_lock_irqsave(&compositor_lock);
  if (win->prev)
    win->prev->next = win->next;
  else
    window_stack = win->next;
  if (win->next)
    win->next->prev = win->prev;
  spin_unlock_irqrestore(&compositor_lock, irqflags);

  rect_t damage = {win->x - WIN11_SHADOW_SIZE, win->y - WIN11_SHADOW_SIZE,
                   win->width + WIN11_SHADOW_SIZE * 2,
                   win->height + WIN11_SHADOW_SIZE * 2};
  compositor_invalidate_rect(damage);
  window_destroy(win);
}

#define CURSOR_W 12
#define CURSOR_H 19

static const uint8_t g_cursor_arrow[19][12] = {
    {2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {2, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0}, {2, 1, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0},
    {2, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0, 0}, {2, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0},
    {2, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0}, {2, 1, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0},
    {2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 0, 0}, {2, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 0},
    {2, 1, 2, 0, 2, 1, 1, 1, 2, 0, 0, 0}, {2, 2, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0},
    {0, 0, 0, 0, 0, 2, 1, 1, 1, 2, 0, 0}, {0, 0, 0, 0, 0, 2, 1, 1, 1, 2, 0, 0},
    {0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 0, 0}, {0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0},
};

static void draw_cursor_overlay(uint32_t *dst, int stride, rect_t clip) {
  int cx = cursor_x;
  int cy = cursor_y;

  for (int y = 0; y < CURSOR_H; y++) {
    int py = cy + y;
    if (py < clip.y || py >= clip.y + clip.h)
      continue;
    const uint8_t *row = g_cursor_arrow[y];
    for (int x = 0; x < CURSOR_W; x++) {
      uint8_t v = row[x];
      if (v == 0)
        continue;
      int px = cx + x;
      if (px < clip.x || px >= clip.x + clip.w)
        continue;
      uint32_t c = (v == 1) ? 0xFFFFFFFF : 0xFF000000;
      dst[py * stride + px] = c;
    }
  }
}

static void compositor_focus_window(window_t *win) {
  if (!win)
    return;

  int need_restore = (win->flags & WIN_FLAGS_HIDDEN) ||
                     (win->anim_state == WIN_ANIM_MINIMIZING);

  if (win->anim_state == WIN_ANIM_CLOSING)
    return;

  if (need_restore) {
    win->flags &= ~WIN_FLAGS_HIDDEN;
    win->flags &= ~WIN_FLAGS_INACTIVE;

    if (win->anim_state == WIN_ANIM_MINIMIZING) {
      win->anim_state = WIN_ANIM_NONE;
    }

    if (win->anim_state != WIN_ANIM_OPENING) {
      window_start_open_animation(win);
    }
    win->dirty = 1;

    compositor_invalidate_rect((rect_t){win->x - WIN11_SHADOW_SIZE,
                                        win->y - WIN11_SHADOW_SIZE,
                                        win->width + WIN11_SHADOW_SIZE * 2,
                                        win->height + WIN11_SHADOW_SIZE * 2});

    compositor_has_input = 1;
    wake_up_all(&compositor_wq);
  }

  if (window_stack == win && (win->flags & WIN_FLAGS_FOCUSED))
    return;

  preempt_disable();

  window_t *old_focus = NULL;
  window_t *curr = window_stack;
  while (curr) {
    if (curr != win && (curr->flags & WIN_FLAGS_FOCUSED)) {
      old_focus = curr;
      curr->flags &= ~WIN_FLAGS_FOCUSED;
      curr->flags |= WIN_FLAGS_INACTIVE;
      curr->dirty = 1;
    }
    curr = curr->next;
  }

  if (window_stack != win) {
    if (win->prev)
      win->prev->next = win->next;
    if (win->next)
      win->next->prev = win->prev;
    win->next = window_stack;
    win->prev = NULL;
    if (window_stack)
      window_stack->prev = win;
    window_stack = win;
  }

  win->flags |= WIN_FLAGS_FOCUSED;
  win->flags &= ~WIN_FLAGS_INACTIVE;
  win->dirty = 1;

  if (win->taskbar_item) {
    taskbar_set_active_item(win->taskbar_item);
  }

  rect_t win_damage = {win->x - WIN11_SHADOW_SIZE, win->y - WIN11_SHADOW_SIZE,
                       win->width + WIN11_SHADOW_SIZE * 2,
                       win->height + WIN11_SHADOW_SIZE * 2};
  if (old_focus) {
    rect_t old_damage = {old_focus->x - WIN11_SHADOW_SIZE,
                         old_focus->y - WIN11_SHADOW_SIZE,
                         old_focus->width + WIN11_SHADOW_SIZE * 2,
                         old_focus->height + WIN11_SHADOW_SIZE * 2};
    compositor_invalidate_rect(rect_bounding_box(win_damage, old_damage));
  } else {
    compositor_invalidate_rect(win_damage);
  }

  preempt_enable();

  if (old_focus) {
    winsrv_post_event(old_focus, WINSRV_EV_BLUR, 0, 0, 0);
  }
  winsrv_post_event(win, WINSRV_EV_FOCUS, 0, 0, 0);

  compositor_invalidate_taskbar();
}

static void compositor_unfocus_all(void) {
  int changed = 0;
  window_t *curr = window_stack;
  while (curr) {
    if (curr->flags & WIN_FLAGS_FOCUSED) {
      curr->flags &= ~WIN_FLAGS_FOCUSED;
      curr->flags |= WIN_FLAGS_INACTIVE;
      curr->dirty = 1;
      compositor_invalidate_rect(
          (rect_t){curr->x - WIN11_SHADOW_SIZE, curr->y - WIN11_SHADOW_SIZE,
                   curr->width + WIN11_SHADOW_SIZE * 2,
                   curr->height + WIN11_SHADOW_SIZE * 2});
      winsrv_post_event(curr, WINSRV_EV_BLUR, 0, 0, 0);
      changed = 1;
    }
    curr = curr->next;
  }
  if (changed) {
    taskbar_set_active_item(NULL);
    compositor_invalidate_taskbar();
  }
}

static void compositor_handle_key(uint8_t scancode, int pressed) {
  if (!pressed)
    return;

  window_t *w = window_stack;
  while (w) {
    if (!(w->flags & WIN_FLAGS_HIDDEN) && (w->flags & WIN_FLAGS_FOCUSED)) {
      winsrv_post_event(w, WINSRV_EV_KEY, (int32_t)scancode, 1, 0);
      break;
    }
    w = w->next;
  }
}

static void compositor_handle_mouse(int16_t dx, int16_t dy, uint8_t buttons) {
  int old_x = cursor_x, old_y = cursor_y;
  int mouse_moved = 0;

  cursor_x += dx;
  cursor_y += dy;

  if (cursor_x < 0)
    cursor_x = 0;
  if (cursor_x >= (int)fb_width)
    cursor_x = fb_width - 1;
  if (cursor_y < 0)
    cursor_y = 0;
  if (cursor_y >= (int)fb_height)
    cursor_y = fb_height - 1;

  if (dx != 0 || dy != 0)
    mouse_moved = 1;

  int pressed = buttons & 0x01;
  int released = (last_mouse_buttons & 0x01) && !pressed;

  /* ------------------------------------------------------------------
   * 0. Hover de la taskbar
   * ------------------------------------------------------------------ */
  {
    taskbar_item_t *hit =
        taskbar_hit_test(cursor_x, cursor_y, (int)fb_width, (int)fb_height);
    taskbar_item_t *it = taskbar_items_head();
    int changed = 0;
    while (it) {
      int should = (it == hit) ? 1 : 0;
      if (it->is_hovered != should) {
        it->is_hovered = should;
        changed = 1;
        rect_t r = taskbar_item_get_bounds(it, (int)fb_width, (int)fb_height);
        compositor_invalidate_rect(
            (rect_t){r.x - 4, r.y - 4, r.w + 8, r.h + 8});
      }
      it = it->next;
    }
    if (changed) {
      compositor_has_input = 1;
      wake_up_all(&compositor_wq);
    }
  }

  /* ------------------------------------------------------------------
   * 0.5 Hover de botones de ventana (min/max/close)
   * ------------------------------------------------------------------ */
  {
    window_t *w = window_stack;
    int dirty_any = 0;
    while (w) {
      int old_btn = w->hover_btn;
      int old_ctrl = w->hover_controls;
      w->hover_btn = 0;
      w->hover_controls = 0;

      if (!(w->flags & WIN_FLAGS_HIDDEN) && w->anim_state == WIN_ANIM_NONE) {
        int in_x = (cursor_x >= w->x && cursor_x < w->x + w->width);
        int in_y =
            (cursor_y >= w->y && cursor_y < w->y + WIN11_TITLEBAR_HEIGHT);
        if (in_x && in_y) {
          w->hover_controls = 1;
          int base_x = w->x + w->width;
          int close_x = base_x - 46;
          int max_x = close_x - 46;
          int min_x = max_x - 46;
          if (cursor_x >= close_x)
            w->hover_btn = 3;
          else if (cursor_x >= max_x)
            w->hover_btn = 2;
          else if (cursor_x >= min_x)
            w->hover_btn = 1;
        }
      }
      if (w->hover_btn != old_btn || w->hover_controls != old_ctrl) {
        w->dirty = 1;
        compositor_invalidate_rect((rect_t){w->x - WIN11_SHADOW_SIZE,
                                            w->y - WIN11_SHADOW_SIZE,
                                            w->width + WIN11_SHADOW_SIZE * 2,
                                            w->height + WIN11_SHADOW_SIZE * 2});
        dirty_any = 1;
      }
      w = w->next;
    }
    if (dirty_any) {
      compositor_has_input = 1;
      wake_up_all(&compositor_wq);
    }
  }

  /* ------------------------------------------------------------------
   * 1. Click izquierdo pulsado
   * ------------------------------------------------------------------ */
  if (pressed && !(last_mouse_buttons & 0x01)) {
    rect_t bar_rect = taskbar_get_bounds(fb_width, fb_height);
    if (cursor_y >= bar_rect.y && cursor_y < bar_rect.y + TASKBAR_BAR_H) {
      taskbar_item_t *item =
          taskbar_hit_test(cursor_x, cursor_y, fb_width, fb_height);
      if (item && item->on_click)
        item->on_click(item);
    } else {
      window_t *win = window_stack;
      int hit_any = 0;

      while (win) {
        if (win->flags & WIN_FLAGS_HIDDEN) {
          win = win->next;
          continue;
        }
        if (win->anim_state == WIN_ANIM_OPENING ||
            win->anim_state == WIN_ANIM_CLOSING ||
            win->anim_state == WIN_ANIM_MINIMIZING) {
          win = win->next;
          continue;
        }

        if (cursor_x >= win->x && cursor_x < win->x + win->width &&
            cursor_y >= win->y && cursor_y < win->y + win->height) {

          hit_any = 1;

          int close_x = win->x + win->width - 46;
          int max_x = win->x + win->width - 92;
          int min_x = win->x + win->width - 138;

          if (cursor_y >= win->y && cursor_y < win->y + 32) {
            if (cursor_x >= close_x && cursor_x < close_x + 46) {
              pressed_win = win;
              pressed_btn = WIN_BTN_CLOSE_PRESSED;
            } else if (cursor_x >= max_x && cursor_x < close_x) {
              pressed_win = win;
              pressed_btn = WIN_BTN_MAXIMIZE_PRESSED;
            } else if (cursor_x >= min_x && cursor_x < max_x) {
              pressed_win = win;
              pressed_btn = WIN_BTN_MINIMIZE_PRESSED;
            } else {
              compositor_focus_window(win);
              drag_window = win;
              drag_offset_x = cursor_x - win->x;
              drag_offset_y = cursor_y - win->y;
            }
          } else {
            compositor_focus_window(win);
            int local_x = cursor_x - win->x;
            int local_y = cursor_y - win->y - WIN11_TITLEBAR_HEIGHT;
            winsrv_post_event(win, WINSRV_EV_MOUSE, local_x, local_y, 1);
          }
          break;
        }
        win = win->next;
      }

      if (!hit_any) {
        compositor_unfocus_all();
      }
    }
  }

  /* ------------------------------------------------------------------
   * 2. Click izquierdo soltado
   * ------------------------------------------------------------------ */
  if (released) {
    if (pressed_win && pressed_btn != WIN_BTN_NONE) {
      int close_x = pressed_win->x + pressed_win->width - 46;
      int max_x = pressed_win->x + pressed_win->width - 92;
      int min_x = pressed_win->x + pressed_win->width - 138;

      if (pressed_btn == WIN_BTN_CLOSE_PRESSED) {
        if (cursor_x >= close_x && cursor_x < close_x + 46 &&
            cursor_y >= pressed_win->y && cursor_y < pressed_win->y + 32) {
          if (winsrv_has_window(pressed_win)) {
            winsrv_post_event(pressed_win, WINSRV_EV_CLOSE, 0, 0, 0);
          } else {
            compositor_close_window(pressed_win);
          }
        }
      } else if (pressed_btn == WIN_BTN_MAXIMIZE_PRESSED) {
        /* TODO: maximizar */
      } else if (pressed_btn == WIN_BTN_MINIMIZE_PRESSED) {
        if (cursor_x >= min_x && cursor_x < max_x &&
            cursor_y >= pressed_win->y && cursor_y < pressed_win->y + 32) {
          compositor_minimize_window(pressed_win);
          taskbar_set_active_item(NULL);
          compositor_invalidate_taskbar();
        }
      }
    }
    drag_window = NULL;
    pressed_win = NULL;
    pressed_btn = WIN_BTN_NONE;
  }

  /* ------------------------------------------------------------------
   * 3. Drag de ventana
   * ------------------------------------------------------------------ */
  if (pressed && drag_window) {
    int new_x = cursor_x - drag_offset_x;
    int new_y = cursor_y - drag_offset_y;
    if (new_x != drag_window->x || new_y != drag_window->y) {
      rect_t old_r = {drag_window->x - WIN11_SHADOW_SIZE,
                      drag_window->y - WIN11_SHADOW_SIZE,
                      drag_window->width + WIN11_SHADOW_SIZE * 2,
                      drag_window->height + WIN11_SHADOW_SIZE * 2};
      drag_window->x = new_x;
      drag_window->y = new_y;
      rect_t new_r = {drag_window->x - WIN11_SHADOW_SIZE,
                      drag_window->y - WIN11_SHADOW_SIZE,
                      drag_window->width + WIN11_SHADOW_SIZE * 2,
                      drag_window->height + WIN11_SHADOW_SIZE * 2};
      compositor_invalidate_rect(rect_bounding_box(old_r, new_r));

      winsrv_post_event(drag_window, WINSRV_EV_MOVE, new_x, new_y, 0);
    }
  }

  last_mouse_buttons = buttons;

  /* ==================================================================
   * 4. Refresco del cursor — VÍA SISTEMA DE DAÑO
   *
   * La versión anterior copiaba directamente del backbuffer a fb en cada
   * evento. Durante un drag rápido, el backbuffer aún contenía la ventana
   * en su posición VIEJA, así que esa copia pintaba trozos de la ventana
   * antigua en la pantalla → ghosting.
   *
   * Ahora solo metemos el cursor en el sistema de daño. El compositor lo
   * pintará en el próximo frame, cuando el backbuffer esté coherente.
   * El cursor se actualiza a 60 FPS mientras hay input, sin lag visible.
   * ================================================================== */
  if (mouse_moved && (old_x != cursor_x || old_y != cursor_y)) {
    /* Zona antigua (con margen de 2 px por si el redondeo del área nueva
     * tapa parcialmente la vieja). */
    compositor_invalidate_rect(
        (rect_t){old_x - 2, old_y - 2, CURSOR_W + 4, CURSOR_H + 4});
    /* Zona nueva */
    compositor_invalidate_rect(
        (rect_t){cursor_x, cursor_y, CURSOR_W, CURSOR_H});

    compositor_has_input = 1;
    wake_up_all(&compositor_wq);
  }
}

static void compositor_process_events(void) {
  input_event_t ev;
  while (input_pop(&ev)) {
    switch (ev.type) {
    case INPUT_EV_KEY:
      compositor_handle_key(ev.code, ev.value);
      break;
    case INPUT_EV_MOUSE:
      compositor_handle_mouse(ev.value, ev.value2, ev.code);
      break;
    default:
      break;
    }
  }
}

static void compositor_render(void) {
  if (global_damage.w <= 0 || global_damage.h <= 0)
    return;

  rect_t clip =
      rect_clip(global_damage, (rect_t){0, 0, (int)fb_width, (int)fb_height});
  global_damage = (rect_t){0, 0, 0, 0};

  if (clip.w <= 0 || clip.h <= 0)
    return;

  /* 1. Wallpaper (backbuffer ← wallpaper_cache, memoria normal) */
  if (wallpaper_cache) {
    for (int y = clip.y; y < clip.y + clip.h; y++) {
      simd_copy_normal(&backbuffer[y * fb_width + clip.x],
                       &wallpaper_cache[y * fb_width + clip.x], clip.w);
    }
  } else {
    for (int y = clip.y; y < clip.y + clip.h; y++) {
      simd_fill_row(&backbuffer[y * fb_width + clip.x], 0xFF1E1E2E, clip.w);
    }
  }

  /* 2. Ventanas, de atrás hacia adelante */
  window_t *curr = window_stack;
  while (curr && curr->next)
    curr = curr->next;
  while (curr) {
    if (curr->flags & WIN_FLAGS_HIDDEN) {
      curr = curr->prev;
      continue;
    }
    rect_t win_bounds = {curr->x - WIN11_SHADOW_SIZE,
                         curr->y - WIN11_SHADOW_SIZE,
                         curr->width + WIN11_SHADOW_SIZE * 2,
                         curr->height + WIN11_SHADOW_SIZE * 2};
    if (rect_intersects(clip, win_bounds)) {
      window_render_frame(curr, backbuffer, fb_width, clip);
    }
    curr = curr->prev;
  }

  /* 3. Taskbar */
  rect_t taskbar_rect = taskbar_get_bounds(fb_width, fb_height);
  rect_t taskbar_damage = {taskbar_rect.x - 16, taskbar_rect.y - 16,
                           taskbar_rect.w + 32, taskbar_rect.h + 32};
  if (rect_intersects(clip, taskbar_damage)) {
    taskbar_render(backbuffer, fb_width, clip, fb_width, fb_height);
  }

  /* 4. Blit a VRAM (non-temporal) + cursor */
  if (fb_ptr) {
    for (int y = clip.y; y < clip.y + clip.h; y++) {
      simd_copy_to_vram(&fb_ptr[y * fb_pitch + clip.x],
                        &backbuffer[y * fb_width + clip.x], clip.w);
    }
    rect_t cursor_rect = {cursor_x, cursor_y, CURSOR_W, CURSOR_H};
    rect_t c_clip = rect_clip(clip, cursor_rect);
    if (c_clip.w > 0 && c_clip.h > 0) {
      draw_cursor_overlay(fb_ptr, fb_pitch, c_clip);
    }
  }
}

static int compositor_has_active_animations(void) {
  window_t *w = window_stack;
  while (w) {
    if (w->anim_state != WIN_ANIM_NONE)
      return 1;
    w = w->next;
  }
  return 0;
}

static int compositor_tick_animations(int dt_ms) {
  window_t *w = window_stack;
  while (w) {
    window_t *next = w->next;

    if (w->anim_state != WIN_ANIM_NONE) {
      window_advance_animation(w, dt_ms);

      rect_t damage = {
          w->x - WIN11_SHADOW_SIZE, w->y - WIN11_SHADOW_SIZE + w->anim_dy,
          w->width + WIN11_SHADOW_SIZE * 2, w->height + WIN11_SHADOW_SIZE * 2};
      rect_t damage2 = {w->x - WIN11_SHADOW_SIZE, w->y - WIN11_SHADOW_SIZE,
                        w->width + WIN11_SHADOW_SIZE * 2,
                        w->height + WIN11_SHADOW_SIZE * 2};
      compositor_invalidate_rect(rect_bounding_box(damage, damage2));

      if (w->anim_state == WIN_ANIM_NONE) {
        if (w->pending_destroy) {
          w->pending_destroy = 0;
          compositor_do_close_window(w);
        } else if (w->anim_alpha == 0 && !(w->flags & WIN_FLAGS_HIDDEN)) {
          w->flags |= WIN_FLAGS_HIDDEN;
          w->flags &= ~WIN_FLAGS_FOCUSED;
          w->flags |= WIN_FLAGS_INACTIVE;
          w->dirty = 1;
        }
      }
    }
    w = next;
  }
  return compositor_has_active_animations();
}

static void compositor_frame_delay(void) {
  uint64_t freq = klog_get_tsc_freq();
  if (freq > 0) {
    uint64_t target = freq / 60;
    uint64_t start = comp_rdtsc();
    while (comp_rdtsc() - start < target) {
      sched_yield();
    }
  } else {
    uint64_t target = tick_count + 16;
    while (tick_count < target) {
      sched_yield();
    }
  }
}

static void compositor_frame_delay_30fps(void) {
  uint64_t freq = klog_get_tsc_freq();
  if (freq > 0) {
    uint64_t target = freq / 30;
    uint64_t start = comp_rdtsc();
    while (comp_rdtsc() - start < target) {
      sched_yield();
    }
  } else {
    uint64_t target = tick_count + 33;
    while (tick_count < target)
      sched_yield();
  }
}

void compositor_thread(void) {
  compositor_task_ref = sched_current();
  LOG_INFO("[COMP] Compositor thread started");

  uint64_t tsc_freq = klog_get_tsc_freq();
  uint64_t last_tsc = comp_rdtsc();

  while (1) {
    int animations_active = compositor_has_active_animations();
    int hover_active = taskbar_has_active_hover();

    if (!animations_active && !hover_active) {
      wait_event(&compositor_wq, compositor_has_work, NULL);
      last_tsc = comp_rdtsc();
    }

    if (compositor_has_input) {
      compositor_has_input = 0;
      compositor_process_events();
    }

    if (compositor_needs_clock) {
      compositor_needs_clock = 0;
      taskbar_refresh_clock();
      compositor_invalidate_taskbar();
    }

    uint64_t now_tsc = comp_rdtsc();
    int dt;
    if (tsc_freq > 0) {
      dt = (int)(((now_tsc - last_tsc) * 1000) / tsc_freq);
    } else {
      dt = 1;
    }
    last_tsc = now_tsc;
    if (dt < 1)
      dt = 1;
    if (dt > 33)
      dt = 33;

    int still_animating = compositor_tick_animations(dt);

    int hover_changed = taskbar_tick_hover(dt);
    if (hover_changed) {
      taskbar_item_t *it = taskbar_items_head();
      while (it) {
        if (it->hover_t > 0 && it->hover_t < 256) {
          rect_t r = taskbar_item_get_bounds(it, (int)fb_width, (int)fb_height);
          compositor_invalidate_rect(
              (rect_t){r.x - 4, r.y - 4, r.w + 8, r.h + 8});
        }
        it = it->next;
      }
    }

    /* [NUEVO] Diagnóstico: mide el tiempo real de compositor_render(). */
    uint64_t t_render_start = comp_rdtsc();
    compositor_render();
    uint64_t t_render_end = comp_rdtsc();
    compositor_report_frame_stats(t_render_end - t_render_start);

    if (still_animating) {
      compositor_frame_delay();
    } else if (hover_changed) {
      compositor_frame_delay_30fps();
    }
  }
}