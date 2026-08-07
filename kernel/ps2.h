// kernel/ps2.h
#ifndef PS2_H
#define PS2_H

#include <stdint.h>

void ps2_init(void);
void ps2_process(void);

int ps2_pop_scancode(uint8_t *out);
int ps2_pop_mouse(int16_t *dx, int16_t *dy, uint8_t *buttons);

void ps2_start_thread(void);
int ps2_has_scancode(void);
int ps2_has_mouse(void);

#endif