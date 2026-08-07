// kernel/gfx/compositor.h
#pragma once
#include <stdint.h>
#include "gfx.h"
#include "window.h"

void compositor_init(void);
void compositor_invalidate_rect(rect_t damage);
window_t *compositor_create_window(int x, int y, int w, int h, const char *title, uint32_t flags);
void compositor_thread(void);
void compositor_notify_event(void);
void compositor_notify_clock_tick(void);
void compositor_process_events(void);
void compositor_close_window(window_t *win);