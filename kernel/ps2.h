// kernel/ps2.h
#ifndef KERNEL_PS2_H
#define KERNEL_PS2_H

#include "driver.h"
#include <stddef.h>
#include <stdint.h>

extern struct driver ps2_driver;

// [FASE 5.5] PS/2 en Ring 3 (ps2.aur).
int ps2_user_mode(void);
// Entrega bytes crudos del i8042 leídos por ps2.aur (is_mouse: 0 teclado,
// 1 ratón). Los procesa la misma tarea que los de los IRQ handlers.
void ps2_feed(int is_mouse, const uint8_t *data, size_t len);
// El raton envia paquetes de 4 bytes (IntelliMouse, con rueda).
void ps2_set_mouse_wheel(int on);

#endif
