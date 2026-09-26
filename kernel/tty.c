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

  LOG_DEBUG("[TTY-IN] byte=0x%02x", (unsigned)(uint8_t)c); // <-- añade esto
  // Eco a serial (debug).
  unsigned long flags = spin_lock_irqsave(&tty->lock);
  tty_echo_serial(c);
  spin_unlock_irqrestore(&tty->lock, flags);

  // Entregar el caracter a la ventana consola SOLO si está enfocada
  // y visible. Sin este filtro, escribir con el escritorio activo o
  // con la ventana minimizada seguía inyectando WINSRV_EV_TTY_INPUT
  // en la app, y la shell escribía igualmente porque su bucle
  // procesa ese evento sin mirar el foco.
  if (g_console_win) {
    window_t *win = (window_t *)g_console_win;
    if (!(win->flags & WIN_FLAGS_HIDDEN) && (win->flags & WIN_FLAGS_FOCUSED)) {
      winsrv_post_event(win, WINSRV_EV_TTY_INPUT, (int32_t)(uint8_t)c, 0, 0);
    }
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

tty_t *tty_default(void) { return tty_ready ? &tty0 : NULL; }

void tty_set_console_window(void *win) { g_console_win = win; }
