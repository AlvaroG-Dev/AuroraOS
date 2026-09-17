// kernel/tty.h
#ifndef KERNEL_TTY_H
#define KERNEL_TTY_H

#include "spinlock.h"
#include "gfx/window.h"
#include <stddef.h>
#include <stdint.h>

typedef struct tty {
    spinlock_t lock;
} tty_t;

void tty_init(void);
tty_t *tty_default(void);
void tty_receive_char(tty_t *tty, char c);
void tty_set_console_window(void *win);

#endif
