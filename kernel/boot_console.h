#ifndef BOOT_CONSOLE_H
#define BOOT_CONSOLE_H

#include "klog.h"
#include <stdint.h>

void boot_console_init(uint32_t *fb, uint32_t width, uint32_t height,
                       uint32_t pitch_px);
void boot_console_clear(void);
void boot_console_putchar(char c, klog_level_t level, klog_fb_style_t style);
void boot_console_hook_install(void);
void boot_console_hook_remove(void);

void boot_console_overlay_enable(int on);
int boot_console_overlay_enabled(void);
void boot_console_draw_overlay(uint32_t *dst, uint32_t dst_pitch, int clip_x,
                               int clip_y, int clip_w, int clip_h);

#endif