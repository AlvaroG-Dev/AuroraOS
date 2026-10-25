// kernel/evdev.h
//
// Interfaz evdev de Linux sobre el input de Aurora: /dev/input/event0
// (teclado) y /dev/input/event1 (raton), con struct input_event de Linux,
// EVIOC* ioctls, poll/epoll y grab. Es lo que usan libinput, libevdev,
// Weston, wlroots, X11...
//
// Los drivers (ps2, usb_hid) informan de cada tecla/movimiento con las
// funciones evdev_* de abajo; el input_push() original sigue alimentando al
// compositor nativo.

#ifndef KERNEL_EVDEV_H
#define KERNEL_EVDEV_H

#include <stdint.h>

void evdev_init(void);
// Expone los dispositivos en /sys (registro dinamico) y /run/udev/data.
void evdev_publish_sysfs(void);
void evdev_publish_udev(void);

// Teclado. `value`: 1 = pulsada, 0 = soltada (2 = autorepeat, se deduce solo).
void evdev_kbd_key(uint16_t linux_key, int value);
// PS/2 set 1: code = byte & 0x7F, ext = precedido de 0xE0.
void evdev_ps2_key(uint8_t code, int ext, int pressed);
// USB HID: prev/cur son bitmaps de 256 bits (indice = HID usage de la pagina
// 0x07, E0..E7 = modificadores). Emite las diferencias y deja prev = cur.
void evdev_hid_keys_sync(uint8_t prev[32], const uint8_t cur[32]);

// Raton. dx/dy relativos (positivo = derecha/abajo), buttons: bit0 izq,
// bit1 der, bit2 central. Emite solo lo que cambia.
void evdev_mouse_report(int dx, int dy, uint8_t buttons);
void evdev_mouse_wheel(int vertical, int horizontal);

#endif
