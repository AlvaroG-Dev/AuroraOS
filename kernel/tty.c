// kernel/tty.c
#include "tty.h"
#include "gfx/window.h"
#include "gfx/winsrv.h"
#include "klog.h"
#include "process.h"
#include "serial.h"
#include "string.h"
#include "uaccess.h"

static tty_t tty0;
static void *g_console_win = NULL;
static int tty_ready = 0;

static void tty_apply_defaults(tty_t *tty) {
  tty->iflag = TTY_IFLAG_ICRNL;
  tty->oflag = TTY_OFLAG_OPOST | TTY_OFLAG_ONLCR;
  tty->cflag = TTY_CFLAG_CREAD | TTY_CFLAG_CS8 | TTY_CFLAG_B38400;
  tty->lflag = TTY_LFLAG_ISIG | TTY_LFLAG_ICANON | TTY_LFLAG_ECHO |
               TTY_LFLAG_ECHOE | TTY_LFLAG_ECHOK;

  memset(tty->cc, 0, sizeof(tty->cc));
  tty->cc[TTY_CC_VINTR] = 0x03;
  tty->cc[TTY_CC_VQUIT] = 0x1C;
  tty->cc[TTY_CC_VERASE] = 0x7F;
  tty->cc[TTY_CC_VKILL] = 0x15;
  tty->cc[TTY_CC_VEOF] = 0x04;
  tty->cc[TTY_CC_VSTART] = 0x11;
  tty->cc[TTY_CC_VSTOP] = 0x13;
  tty->cc[TTY_CC_VSUSP] = 0x1A;
  tty->cc[TTY_CC_VMIN] = 1;
  tty->cc[TTY_CC_VTIME] = 0;
}

static void tty_push_byte_locked(tty_t *tty, uint8_t c) {
  if (tty->count >= TTY_BUF_SIZE)
    return;
  tty->buf[tty->tail] = c;
  tty->tail = (tty->tail + 1) % TTY_BUF_SIZE;
  tty->count++;
}

static void tty_push_line_locked(tty_t *tty) {
  for (size_t i = 0; i < tty->canon_len; i++)
    tty_push_byte_locked(tty, tty->canon_buf[i]);
  tty_push_byte_locked(tty, '\n');
  tty->canon_len = 0;
}

static void tty_canon_input_locked(tty_t *tty, uint8_t c) {
  if ((tty->iflag & TTY_IFLAG_ICRNL) && c == '\r')
    c = '\n';

  if (c == '\n') {
    tty_push_line_locked(tty);
    return;
  }

  if (c == tty->cc[TTY_CC_VEOF]) {
    if (tty->canon_len == 0)
      tty->eof_pending = 1;
    else
      tty_push_line_locked(tty);
    return;
  }

  if (c == tty->cc[TTY_CC_VERASE] || c == '\b') {
    if (tty->canon_len > 0)
      tty->canon_len--;
    return;
  }

  if (c == tty->cc[TTY_CC_VKILL]) {
    tty->canon_len = 0;
    return;
  }

  if ((tty->lflag & TTY_LFLAG_ISIG) &&
      (c == tty->cc[TTY_CC_VINTR] || c == tty->cc[TTY_CC_VQUIT] ||
       c == tty->cc[TTY_CC_VSUSP])) {
    return;
  }

  if (tty->canon_len < TTY_BUF_SIZE - 1)
    tty->canon_buf[tty->canon_len++] = c;
}

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
// [FIX v3] Recepción desde el driver de teclado.
//
// Modelo:
//   - El TTY SIEMPRE acumula el byte (canon_buf o ring buffer).
//   - El TTY SIEMPRE hace eco al console si ECHO está activo. Este es el
//     ÚNICO punto que dibuja la tecla; el shell nativo NO hace eco por
//     su cuenta cuando recibe WINSRV_EV_TTY_INPUT.
//   - El TTY SIEMPRE avisa al shell nativo (WINSRV_EV_TTY_INPUT) en modo
//     canonical, y también para Ctrl+C en modo raw.
//
// Para evitar que el próximo lector (busybox, cat) lea comandos ya
// tecleados en el shell nativo, el shell debe llamar a
// ioctl(0, TCFLSH, TCIFLUSH) antes de spawnear. Es el shell quien
// decide cuándo limpiar, no el TTY quien intenta adivinar si hay lector.
// ---------------------------------------------------------------------------
void tty_receive_char(tty_t *tty, char c) {
  if (!tty || !tty_ready)
    return;

  uint8_t byte = (uint8_t)c;

  unsigned long flags = spin_lock_irqsave(&tty->lock);

  int canon = (tty->lflag & TTY_LFLAG_ICANON) != 0;

  // Eco a serial (debug).
  if (canon)
    tty_echo_serial(c);

  // Acumular SIEMPRE, sin condiciones. El shell limpia con TCFLSH antes
  // de spawnear, así que el buffer nunca arrastra comandos viejos.
  if (canon)
    tty_canon_input_locked(tty, byte);
  else
    tty_push_byte_locked(tty, byte);

  int echo_to_console = (tty->lflag & TTY_LFLAG_ECHO);

  spin_unlock_irqrestore(&tty->lock, flags);

  if (echo_to_console) {
    if (byte == '\n') {
      winsrv_console_output('\r');
      winsrv_console_output('\n');
    } else if (byte == '\b' || byte == 0x7F) {
      winsrv_console_output('\b');
      winsrv_console_output(' ');
      winsrv_console_output('\b');
    } else {
      winsrv_console_output((char)byte);
    }
  }

  wake_up_all(&tty->read_wq);

  // Avisar al shell nativo: en canonical siempre, en raw solo Ctrl+C.
  int post_to_shell = 0;
  int32_t post_byte = (int32_t)byte;

  if (canon) {
    post_to_shell = 1;
    if (byte == 0x7F)
      post_byte = 0x08;
  } else if (byte == 0x03) {
    post_to_shell = 1;
  }

  if (post_to_shell && g_console_win) {
    window_t *win = (window_t *)g_console_win;
    if (!(win->flags & WIN_FLAGS_HIDDEN) && (win->flags & WIN_FLAGS_FOCUSED)) {
      winsrv_post_event(win, WINSRV_EV_TTY_INPUT, post_byte, 0, 0);
    }
  }
}

// ---------------------------------------------------------------------------
// [NEW] Vaciar el buffer de entrada.
// ---------------------------------------------------------------------------
void tty_flush_input(tty_t *tty) {
  if (!tty)
    return;
  unsigned long flags = spin_lock_irqsave(&tty->lock);
  tty->head = 0;
  tty->tail = 0;
  tty->count = 0;
  tty->canon_len = 0;
  tty->eof_pending = 0;
  spin_unlock_irqrestore(&tty->lock, flags);
}

// ---------------------------------------------------------------------------
// VFS read/write/poll/ioctl
// ---------------------------------------------------------------------------
static bool tty_readable_cond(void *arg) {
  tty_t *tty = (tty_t *)arg;
  return tty->count > 0 || tty->eof_pending;
}

int64_t tty_read(tty_t *tty, uint64_t offset, size_t size, void *buf) {
  (void)offset;
  if (!tty || !buf || size == 0)
    return 0;

  int rc = wait_event_interruptible(&tty->read_wq, tty_readable_cond, tty);
  if (rc < 0)
    return -EINTR;

  unsigned long flags = spin_lock_irqsave(&tty->lock);

  if (tty->eof_pending) {
    tty->eof_pending = 0;
    spin_unlock_irqrestore(&tty->lock, flags);
    return 0;
  }

  size_t n = tty->count < size ? tty->count : size;
  uint8_t *out = (uint8_t *)buf;
  for (size_t i = 0; i < n; i++) {
    out[i] = tty->buf[tty->head];
    tty->head = (tty->head + 1) % TTY_BUF_SIZE;
  }
  tty->count -= n;

  spin_unlock_irqrestore(&tty->lock, flags);
  return (int64_t)n;
}

int64_t tty_write(tty_t *tty, uint64_t offset, size_t size, const void *buf) {
  (void)tty;
  (void)offset;
  if (!buf || size == 0)
    return 0;

  const char *s = (const char *)buf;
  unsigned long flags;
  serial_lock_acquire(&flags);
  for (size_t i = 0; i < size; i++) {
    char c = s[i];
    if ((tty->oflag & TTY_OFLAG_ONLCR) && c == '\n') {
      serial_putc_locked('\r');
      winsrv_console_output('\r');
    }
    serial_putc_locked(c);
    winsrv_console_output(c);
  }
  serial_lock_release(flags);
  return (int64_t)size;
}

int tty_poll(tty_t *tty, short events) {
  int revents = 0;
  if (!tty)
    return 0;
  if (events & 1 /*POLLIN*/) {
    unsigned long flags = spin_lock_irqsave(&tty->lock);
    if (tty->count > 0 || tty->eof_pending)
      revents |= 1;
    spin_unlock_irqrestore(&tty->lock, flags);
  }
  if (events & 4 /*POLLOUT*/)
    revents |= 4;
  return revents;
}

#define TCGETS 0x5401
#define TCSETS 0x5402
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCGSID 0x5429
#define TIOCNOTTY 0x5422
#define TCFLSH 0x540B
#define TCXONC 0x540A

struct ktty_termios {
  uint32_t c_iflag;
  uint32_t c_oflag;
  uint32_t c_cflag;
  uint32_t c_lflag;
  uint8_t c_line;
  uint8_t c_cc[32];
  uint32_t c_ispeed;
  uint32_t c_ospeed;
};

struct ktty_winsize {
  uint16_t ws_row;
  uint16_t ws_col;
  uint16_t ws_xpixel;
  uint16_t ws_ypixel;
};

int64_t tty_ioctl(tty_t *tty, unsigned long req, uint64_t arg) {
  if (!tty)
    return -ENOTTY;

  switch (req) {
  case TCGETS: {
    struct ktty_termios t;
    memset(&t, 0, sizeof(t));
    unsigned long flags = spin_lock_irqsave(&tty->lock);
    t.c_iflag = tty->iflag;
    t.c_oflag = tty->oflag;
    t.c_cflag = tty->cflag;
    t.c_lflag = tty->lflag;
    memcpy(t.c_cc, tty->cc, sizeof(tty->cc) < 32 ? sizeof(tty->cc) : 32);
    spin_unlock_irqrestore(&tty->lock, flags);
    if (!access_ok((void *)arg, sizeof(t)))
      return -EFAULT;
    if (copy_to_user((void *)arg, &t, sizeof(t)) < 0)
      return -EFAULT;
    return 0;
  }
  case TCSETS:
  case TCSETSW:
  case TCSETSF: {
    if (!access_ok((void *)arg, sizeof(struct ktty_termios)))
      return -EFAULT;
    struct ktty_termios t;
    if (copy_from_user(&t, (void *)arg, sizeof(t)) < 0)
      return -EFAULT;
    unsigned long flags = spin_lock_irqsave(&tty->lock);
    tty->iflag = t.c_iflag;
    tty->oflag = t.c_oflag;
    tty->cflag = t.c_cflag;
    tty->lflag = t.c_lflag;
    memcpy(tty->cc, t.c_cc, sizeof(tty->cc));
    spin_unlock_irqrestore(&tty->lock, flags);
    return 0;
  }
  case TIOCGWINSZ: {
    struct ktty_winsize ws;
    ws.ws_row = (uint16_t)tty->winsize_rows;
    ws.ws_col = (uint16_t)tty->winsize_cols;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    if (!access_ok((void *)arg, sizeof(ws)))
      return -EFAULT;
    if (copy_to_user((void *)arg, &ws, sizeof(ws)) < 0)
      return -EFAULT;
    return 0;
  }
  case TIOCSWINSZ: {
    if (!access_ok((void *)arg, sizeof(struct ktty_winsize)))
      return -EFAULT;
    struct ktty_winsize ws;
    if (copy_from_user(&ws, (void *)arg, sizeof(ws)) < 0)
      return -EFAULT;
    unsigned long flags = spin_lock_irqsave(&tty->lock);
    tty->winsize_rows = ws.ws_row;
    tty->winsize_cols = ws.ws_col;
    spin_unlock_irqrestore(&tty->lock, flags);
    return 0;
  }
  case TIOCGPGRP: {
    process_t *proc = process_current();
    int32_t pgrp = proc ? (int32_t)proc->pid : 0;
    if (!access_ok((void *)arg, sizeof(pgrp)))
      return -EFAULT;
    if (copy_to_user((void *)arg, &pgrp, sizeof(pgrp)) < 0)
      return -EFAULT;
    return 0;
  }
  case TIOCSPGRP:
    return 0;
  case TIOCGSID: {
    process_t *proc = process_current();
    int32_t sid = proc ? (int32_t)proc->pid : 0;
    if (!access_ok((void *)arg, sizeof(sid)))
      return -EFAULT;
    if (copy_to_user((void *)arg, &sid, sizeof(sid)) < 0)
      return -EFAULT;
    return 0;
  }
  case TIOCNOTTY:
    return 0;
  case TCFLSH: {
    // arg: 0 = TCIFLUSH (input), 1 = TCOFLUSH (output), 2 = TCIOFLUSH.
    // Solo implementamos input. Output no tenemos buffer propio.
    int que = (int)arg;
    if (que == 0 || que == 2)
      tty_flush_input(tty);
    return 0;
  }
  case TCXONC:
    return 0;
  default:
    return -ENOTTY;
  }
}

void tty_init(void) {
  memset(&tty0, 0, sizeof(tty0));
  spin_init(&tty0.lock);
  wait_queue_init(&tty0.read_wq);
  tty_apply_defaults(&tty0);
  tty0.winsize_cols = 100;
  tty0.winsize_rows = 35;
  g_console_win = NULL;
  tty_ready = 1;
  LOG_INFO("[TTY] Inicializado (canon+echo+isig, 100x35)");
}

tty_t *tty_default(void) { return tty_ready ? &tty0 : NULL; }

void tty_set_console_window(void *win) { g_console_win = win; }