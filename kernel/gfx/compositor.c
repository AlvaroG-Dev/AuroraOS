// kernel/gfx/compositor.c
#include "compositor.h"
#include "../bmp.h"
#include "../heap.h"
#include "../input.h"
#include "../klog.h"
#include "../sched.h"
#include "../serial.h"
#include "../tarfs.h"
#include "../wait.h"
#include "font_manager.h"
#include "gfx.h"
#include "taskbar.h"
#include "theme.h"
#include "window.h"
#include "winsrv.h"
#include <emmintrin.h>
#include <stddef.h>

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

/* =========== SSE helpers =========== */
static inline void sse_memset32(uint32_t *dest, uint32_t val, size_t count) {
  size_t i = 0;
  while (((uintptr_t)&dest[i] & 15) != 0 && i < count) {
    dest[i] = val;
    i++;
  }
  __m128i val_vec = _mm_set1_epi32((int)val);
  for (; i + 4 <= count; i += 4) {
    _mm_store_si128((__m128i *)&dest[i], val_vec);
  }
  for (; i < count; i++)
    dest[i] = val;
}

static inline void sse_memcpy_vram(uint32_t *dest, const uint32_t *src,
                                   size_t count) {
  size_t i = 0;
  while (((uintptr_t)&dest[i] & 15) != 0 && i < count) {
    dest[i] = src[i];
    i++;
  }
  for (; i + 4 <= count; i += 4) {
    __m128i chunk = _mm_loadu_si128((const __m128i *)&src[i]);
    _mm_stream_si128((__m128i *)&dest[i], chunk);
  }
  for (; i < count; i++)
    dest[i] = src[i];
  _mm_sfence();
}

static void compositor_invalidate_taskbar(void) {
  rect_t bar_rect = taskbar_get_bounds(fb_width, fb_height);
  rect_t damage = {bar_rect.x - 16, bar_rect.y - 16, bar_rect.w + 32,
                   bar_rect.h + 32};
  compositor_invalidate_rect(damage);
}

static void taskbar_item_on_click(taskbar_item_t *item) {
  if (!item)
    return;
  window_t *win = window_stack;
  while (win) {
    if (win->taskbar_item == item) {
      if (window_is_visible(win) && (win->flags & WIN_FLAGS_FOCUSED)) {
        window_set_visible(win, 0);
        win->flags &= ~WIN_FLAGS_FOCUSED;
        win->flags |= WIN_FLAGS_INACTIVE;
        taskbar_set_active_item(NULL);
        compositor_invalidate_rect(
            (rect_t){win->x - WIN11_SHADOW_SIZE, win->y - WIN11_SHADOW_SIZE,
                     win->width + WIN11_SHADOW_SIZE * 2,
                     win->height + WIN11_SHADOW_SIZE * 2});
        compositor_invalidate_taskbar();
      } else {
        window_set_visible(win, 1);
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

// ---------------------------------------------------------------------------
// Condición para despertar al compositor.
// ---------------------------------------------------------------------------
static bool compositor_has_work(void *arg) {
  (void)arg;
  return compositor_has_input || compositor_needs_clock ||
         (global_damage.w > 0 && global_damage.h > 0);
}

// ---------------------------------------------------------------------------
// Notificaciones desde otros subsistemas (input, timer, etc.)
// ---------------------------------------------------------------------------
void compositor_notify_event(void) {
  compositor_has_input = 1;
  wake_up_all(&compositor_wq);
}

void compositor_notify_clock_tick(void) {
  compositor_needs_clock = 1;
  wake_up_all(&compositor_wq);
}

void compositor_init(void) {
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
  bg_wallpaper_node = tar_find_file("system/wallpapers/default-background.bmp");

  wallpaper_cache = (uint32_t *)kmalloc(size);
  if (wallpaper_cache) {
    if (bg_wallpaper_node) {
      rect_t full_clip = {0, 0, (int)fb_width, (int)fb_height};
      bmp_draw_scaled(bg_wallpaper_node, wallpaper_cache, fb_width, full_clip,
                      0, 0, fb_width, fb_height);
    } else {
      for (int y = 0; y < (int)fb_height; y++) {
        uint8_t factor = (y * 30) / fb_height;
        uint32_t bg_color = 0xFF000000 | ((0x20 - (factor / 2)) << 16) |
                            ((0x22 - (factor / 2)) << 8) | (0x28 - factor);
        sse_memset32(&wallpaper_cache[y * fb_width], bg_color, fb_width);
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
  // [PREEMPT] Modificamos window_stack: no dejar que el scheduler
  // cambie de tarea a mitad del push.
  preempt_disable();

  window_t *win = window_create(x, y, w, h, title, flags);
  if (!win) {
    preempt_enable();
    return NULL;
  }

  win->next = window_stack;
  if (window_stack)
    window_stack->prev = win;
  window_stack = win;

  rect_t damage = {x - WIN11_SHADOW_SIZE, y - WIN11_SHADOW_SIZE,
                   w + WIN11_SHADOW_SIZE * 2, h + WIN11_SHADOW_SIZE * 2};

  // invalidate_rect hace wake_up sobre compositor_wq, lo cual está bien
  // dentro de preempt_disable (wake_up solo modifica la wq de otros).
  compositor_invalidate_rect(damage);

  taskbar_item_t *tb_item =
      taskbar_add_item(TASKBAR_ITEM_APP_ICON, win->title, win->icon_symbol,
                       win->icon_bg_color, taskbar_item_on_click);
  win->taskbar_item = tb_item;

  preempt_enable();
  return win;
}

void compositor_close_window(window_t *win) {
  if (!win)
    return;

  preempt_disable();

  if (drag_window == win)
    drag_window = NULL;
  if (pressed_win == win) {
    pressed_win = NULL;
    pressed_btn = WIN_BTN_NONE;
  }
  if (win->prev)
    win->prev->next = win->next;
  else
    window_stack = win->next;
  if (win->next)
    win->next->prev = win->prev;

  preempt_enable();

  compositor_invalidate_rect((rect_t){
      win->x - WIN11_SHADOW_SIZE, win->y - WIN11_SHADOW_SIZE,
      win->width + WIN11_SHADOW_SIZE * 2, win->height + WIN11_SHADOW_SIZE * 2});
  window_destroy(win);
}

static void draw_cursor_overlay(uint32_t *dst, int stride, rect_t clip) {
  rect_t cursor_rect = {cursor_x - 16, cursor_y - 16, 32, 32};
  if (rect_intersects(clip, cursor_rect)) {
    rect_t c_clip = rect_clip(clip, cursor_rect);
    gfx_fill_rounded_rect(dst, stride, c_clip,
                          (rect_t){cursor_x - 8, cursor_y - 8, 16, 16}, 4,
                          0xCCFFFFFF);
    gfx_fill_rounded_rect(dst, stride, c_clip,
                          (rect_t){cursor_x - 4, cursor_y - 4, 8, 8}, 2,
                          0xFF000000);
  }
}

static void compositor_focus_window(window_t *win) {
  if (!win)
    return;
  if (win->flags & WIN_FLAGS_HIDDEN)
    window_set_visible(win, 1);
  if (window_stack == win && (win->flags & WIN_FLAGS_FOCUSED))
    return;

  // [PREEMPT] Toda la reordenación de window_stack bajo preempt_disable.
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

  // Notificar al winsrv: blur a la vieja, focus a la nueva.
  if (old_focus) {
    winsrv_post_event(old_focus, WINSRV_EV_BLUR, 0, 0, 0);
  }
  winsrv_post_event(win, WINSRV_EV_FOCUS, 0, 0, 0);

  compositor_invalidate_taskbar();
}

// ---------------------------------------------------------------------------
// Manejo de eventos de teclado
// ---------------------------------------------------------------------------
static void compositor_handle_key(uint8_t scancode, int pressed) {
  if (!pressed)
    return;

  // Buscar la ventana con foco y notificar al winsrv.
  window_t *w = window_stack;
  while (w) {
    if (!(w->flags & WIN_FLAGS_HIDDEN) && (w->flags & WIN_FLAGS_FOCUSED)) {
      winsrv_post_event(w, WINSRV_EV_KEY, (int32_t)scancode, 1, 0);
      break;
    }
    w = w->next;
  }
}

// ---------------------------------------------------------------------------
// Manejo de eventos de ratón
// ---------------------------------------------------------------------------
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

  if (pressed && !(last_mouse_buttons & 0x01)) {
    rect_t bar_rect = taskbar_get_bounds(fb_width, fb_height);
    if (cursor_y >= bar_rect.y && cursor_y < bar_rect.y + TASKBAR_BAR_H) {
      taskbar_item_t *item =
          taskbar_hit_test(cursor_x, cursor_y, fb_width, fb_height);
      if (item && item->on_click)
        item->on_click(item);
    } else {
      window_t *win = window_stack;
      while (win) {
        if (win->flags & WIN_FLAGS_HIDDEN) {
          win = win->next;
          continue;
        }

        if (cursor_x >= win->x && cursor_x < win->x + win->width &&
            cursor_y >= win->y && cursor_y < win->y + win->height) {

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
            // Click dentro del contenido de la ventana.
            compositor_focus_window(win);
            int local_x = cursor_x - win->x;
            int local_y = cursor_y - win->y - WIN11_TITLEBAR_HEIGHT;
            winsrv_post_event(win, WINSRV_EV_MOUSE, local_x, local_y, 1);
          }
          break;
        }
        win = win->next;
      }
    }
  }

  if (released) {
    if (pressed_win && pressed_btn != WIN_BTN_NONE) {
      int close_x = pressed_win->x + pressed_win->width - 46;
      int max_x = pressed_win->x + pressed_win->width - 92;
      int min_x = pressed_win->x + pressed_win->width - 138;

      if (pressed_btn == WIN_BTN_CLOSE_PRESSED) {
        if (cursor_x >= close_x && cursor_x < close_x + 46 &&
            cursor_y >= pressed_win->y && cursor_y < pressed_win->y + 32) {
          // Notificar CLOSE al winsrv. NO cerramos aquí. La app
          // decidirá si destruye la ventana.
          winsrv_post_event(pressed_win, WINSRV_EV_CLOSE, 0, 0, 0);
        }
      } else if (pressed_btn == WIN_BTN_MAXIMIZE_PRESSED) {
        // maximizar (por ahora sin op)
      } else if (pressed_btn == WIN_BTN_MINIMIZE_PRESSED) {
        if (cursor_x >= min_x && cursor_x < max_x &&
            cursor_y >= pressed_win->y && cursor_y < pressed_win->y + 32) {
          window_set_visible(pressed_win, 0);
          pressed_win->flags &= ~WIN_FLAGS_FOCUSED;
          pressed_win->flags |= WIN_FLAGS_INACTIVE;
          taskbar_set_active_item(NULL);
          compositor_invalidate_rect(
              (rect_t){pressed_win->x - WIN11_SHADOW_SIZE,
                       pressed_win->y - WIN11_SHADOW_SIZE,
                       pressed_win->width + WIN11_SHADOW_SIZE * 2,
                       pressed_win->height + WIN11_SHADOW_SIZE * 2});
          compositor_invalidate_taskbar();
        }
      }
    }
    drag_window = NULL;
    pressed_win = NULL;
    pressed_btn = WIN_BTN_NONE;
  }

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

      // Notificar MOVE al winsrv.
      winsrv_post_event(drag_window, WINSRV_EV_MOVE, new_x, new_y, 0);
    }
  }

  last_mouse_buttons = buttons;

  // Refresco inmediato del cursor
  if (mouse_moved && (old_x != cursor_x || old_y != cursor_y)) {
    if (fb_ptr) {
      rect_t old_rect = {old_x - 16, old_y - 16, 32, 32};
      rect_t clip_old =
          rect_clip(old_rect, (rect_t){0, 0, (int)fb_width, (int)fb_height});
      if (clip_old.w > 0 && clip_old.h > 0) {
        for (int y = clip_old.y; y < clip_old.y + clip_old.h; y++) {
          sse_memcpy_vram(&fb_ptr[y * fb_pitch + clip_old.x],
                          &backbuffer[y * fb_width + clip_old.x], clip_old.w);
        }
      }
      rect_t new_rect = {cursor_x - 16, cursor_y - 16, 32, 32};
      rect_t clip_new =
          rect_clip(new_rect, (rect_t){0, 0, (int)fb_width, (int)fb_height});
      if (clip_new.w > 0 && clip_new.h > 0) {
        draw_cursor_overlay(fb_ptr, fb_pitch, clip_new);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Procesar todos los eventos de input pendientes
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Cuerpo de render
// ---------------------------------------------------------------------------
static void compositor_render(void) {
  if (global_damage.w <= 0 || global_damage.h <= 0)
    return;

  rect_t clip =
      rect_clip(global_damage, (rect_t){0, 0, (int)fb_width, (int)fb_height});
  global_damage = (rect_t){0, 0, 0, 0};

  if (clip.w <= 0 || clip.h <= 0)
    return;

  // 1. Wallpaper
  if (wallpaper_cache) {
    for (int y = clip.y; y < clip.y + clip.h; y++) {
      sse_memcpy_vram(&backbuffer[y * fb_width + clip.x],
                      &wallpaper_cache[y * fb_width + clip.x], clip.w);
    }
  } else {
    for (int y = clip.y; y < clip.y + clip.h; y++) {
      sse_memset32(&backbuffer[y * fb_width + clip.x], 0xFF1E1E2E, clip.w);
    }
  }

  // 2. Ventanas de atrás hacia adelante
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

  // 3. Taskbar
  rect_t taskbar_rect = taskbar_get_bounds(fb_width, fb_height);
  rect_t taskbar_damage = {taskbar_rect.x - 16, taskbar_rect.y - 16,
                           taskbar_rect.w + 32, taskbar_rect.h + 32};
  if (rect_intersects(clip, taskbar_damage)) {
    taskbar_render(backbuffer, fb_width, clip, fb_width, fb_height);
  }

  // 4. Copiar a VRAM + cursor
  if (fb_ptr) {
    for (int y = clip.y; y < clip.y + clip.h; y++) {
      sse_memcpy_vram(&fb_ptr[y * fb_pitch + clip.x],
                      &backbuffer[y * fb_width + clip.x], clip.w);
    }
    draw_cursor_overlay(fb_ptr, fb_pitch, clip);
  }
}

// ---------------------------------------------------------------------------
// Thread principal del compositor
// ---------------------------------------------------------------------------
void compositor_thread(void) {
  compositor_task_ref = sched_current();
  LOG_INFO("[COMP] Compositor thread started");

  while (1) {
    wait_event(&compositor_wq, compositor_has_work, NULL);

    if (compositor_has_input) {
      compositor_has_input = 0;
      compositor_process_events();
    }

    if (compositor_needs_clock) {
      compositor_needs_clock = 0;
      taskbar_refresh_clock();
      compositor_invalidate_taskbar();
    }

    compositor_render();
  }
}
