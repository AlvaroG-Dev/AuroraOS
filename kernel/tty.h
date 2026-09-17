// kernel/tty.h
#ifndef KERNEL_TTY_H
#define KERNEL_TTY_H

#include "spinlock.h"
#include "vfs.h"
#include <stddef.h>
#include <stdint.h>

// TTY en modo raw. Ya no acumula líneas ni tiene wait queue.
// El driver de teclado llama a tty_receive_char() con caracteres
// ASCII ya traducidos, y el TTY:
//   1. Hace eco a serial (para debug / CI).
//   2. Publica el byte como evento WINSRV_EV_TTY_INPUT a la ventana
//      registrada como consola (si hay alguna).
//
// El TTY ya no soporta read() bloqueante útil. stdin sigue existiendo
// como dispositivo pero read() devuelve 0.

typedef struct tty {
    spinlock_t lock;
} tty_t;

// Inicializa el TTY.
void tty_init(void);

// Devuelve el TTY principal. No NULL tras tty_init.
tty_t *tty_default(void);

// Llamado desde el driver de teclado con un carácter ASCII ya traducido.
void tty_receive_char(tty_t *tty, char c);

// Registra (o desregistra, con NULL) la ventana del winsrv que
// recibirá los bytes del teclado como eventos.
void tty_set_console_window(void *win);

#endif
