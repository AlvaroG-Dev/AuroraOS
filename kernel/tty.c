// kernel/tty.c
#include "tty.h"
#include "gfx/winsrv.h"
#include "klog.h"
#include "serial.h"
#include "string.h"

static tty_t tty0;
static void *g_console_win = NULL;
static int tty_ready = 0;

// ---------------------------------------------------------------------------
// Eco a serial. Solo para debug / CI. No depende de la ventana.
// ---------------------------------------------------------------------------
static void tty_echo_serial(char c) {
    if (c == '\n') {
        serial_putc('\r');
        serial_putc('\n');
    } else if (c == '\b') {
        serial_putc('\b');
        serial_putc(' ');
        serial_putc('\b');
    } else if (c >= 0x20 && c < 0x7F) {
        serial_putc(c);
    }
}

// ---------------------------------------------------------------------------
// Recepción de caracteres desde el driver de teclado.
// ---------------------------------------------------------------------------
void tty_receive_char(tty_t *tty, char c) {
    if (!tty || !tty_ready)
        return;

    // Eco a serial.
    unsigned long flags = spin_lock_irqsave(&tty->lock);
    tty_echo_serial(c);
    spin_unlock_irqrestore(&tty->lock, flags);

    // Publicar al winsrv si hay consola registrada.
    if (g_console_win) {
        winsrv_post_event((window_t *)g_console_win,
                          WINSRV_EV_TTY_INPUT,
                          (int32_t)(uint8_t)c, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void tty_init(void) {
    spin_init(&tty0.lock);
    g_console_win = NULL;
    tty_ready = 1;
    LOG_INFO("[TTY] Modo raw inicializado (sin line discipline)");
}

tty_t *tty_default(void) {
    return tty_ready ? &tty0 : NULL;
}

void tty_set_console_window(void *win) {
    g_console_win = win;
}
