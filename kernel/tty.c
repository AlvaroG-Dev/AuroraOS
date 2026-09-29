// kernel/tty.c
//
// [PTY Fase 1] Refactor hacia multi-instancia.
//
// Este fichero sigue manejando un único tty global (tty0, el "console
// tty"). Lo que cambia en esta fase es la PREPARACIÓN para soportar N
// ttys:
//
//   1. tty_t gana un campo `pty` (forward-declared) que será != NULL
//      cuando el tty sea el slave de un PTY. Se define y gestiona en
//      kernel/pty.c (Fase 2).
//
//   2. tty_default() pasa a llamarse tty_console(). Se mantiene un
//      alias inline en tty.h para no romper llamantes antiguos.
//
//   3. Se añade tty_slave_receive(): la entrada del slave de un PTY
//      (bytes que llegan por el master fd) se procesa igual que la del
//      PS/2 (canon/push), pero NO hace eco al console ni postea
//      WINSRV_EV_TTY_INPUT. Esa responsabilidad es del console tty.
//
//   4. tty_write() gana un early-return para el caso slave: cuando
//      tty->pty != NULL, la salida va al master del PTY (buffer), no a
//      winsrv/serial. En Fase 1 ningún tty tiene pty != NULL, así que
//      la rama es muerta pero sirve como punto de enganche para Fase 2.
//
// Nada de esto cambia el comportamiento observable del sistema.

#include "tty.h"
#include "gfx/window.h"
#include "gfx/winsrv.h"
#include "klog.h"
#include "process.h"
#include "serial.h"
#include "string.h"
#include "uaccess.h"

static tty_t tty0;
static int tty_ready = 0;

void tty_set_defaults(tty_t *tty) {
  tty->iflag = TTY_IFLAG_ICRNL;
  tty->oflag = TTY_OFLAG_OPOST | TTY_OFLAG_ONLCR;
  tty->cflag = TTY_CFLAG_CREAD | TTY_CFLAG_CS8 | TTY_CFLAG_B38400;
  tty->lflag = TTY_LFLAG_ISIG | TTY_LFLAG_ICANON | TTY_LFLAG_ECHO |
               TTY_LFLAG_ECHOE | TTY_LFLAG_ECHOK | TTY_LFLAG_ECHOCTL;

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

// ---------------------------------------------------------------------------
// Helpers internos. Llamar con tty->lock cogido.
// ---------------------------------------------------------------------------
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
// [PTY Fase 6] tty_receive_char: entrada del PS/2 al console tty.
//
// En el modelo PTY, el console tty ya NO procesa canonical, NO hace eco,
// NO acumula en su ring buffer, y NO pinta en pantalla. Es un simple
// distribuidor: cada byte se convierte en un WINSRV_EV_TTY_INPUT que
// va al terminal enfocado. Ese terminal escribe el byte en el master
// de su PTY, y el slave del PTY (con canon+echo) hace todo el trabajo.
//
// El eco a serial se mantiene para debug (no afecta al usuario).
// ---------------------------------------------------------------------------
void tty_receive_char(tty_t *tty, char c) {
  if (!tty || !tty_ready)
    return;

  // Debug por serial (no visible en la ventana).
  tty_echo_serial(c);

  winsrv_post_to_focused(WINSRV_EV_TTY_INPUT, (int32_t)(uint8_t)c, 0, 0);
}

// ---------------------------------------------------------------------------
// [PTY Fase 2] tty_slave_receive: entrada del slave de un PTY.
//
// Llamado desde pty_master_write cuando el terminal app escribe a su
// master fd. El byte entra al slave y se procesa como si viniera del
// teclado, PERO:
//
//   - El eco NO va al console: va al master del PTY, para que el
//     terminal app lo reciba por su read().
//   - No postea WINSRV_EV_TTY_INPUT (ese canal es del console tty).
//
// El eco lo hace el propio slave: si ECHO está activo, emite el byte
// (o la secuencia correspondiente a \b, \n, etc.) de vuelta al master
// vía pty_slave_emit.
// ---------------------------------------------------------------------------

void tty_slave_receive(tty_t *tty, uint8_t byte) {
  if (!tty || !tty_ready)
    return;

  unsigned long flags = spin_lock_irqsave(&tty->lock);

  int canon = (tty->lflag & TTY_LFLAG_ICANON) != 0;
  int echo = (tty->lflag & TTY_LFLAG_ECHO) != 0;
  int isig = (tty->lflag & TTY_LFLAG_ISIG) != 0;

  uint8_t cc_vintr = tty->cc[TTY_CC_VINTR];
  uint8_t cc_vquit = tty->cc[TTY_CC_VQUIT];
  uint8_t cc_vsusp = tty->cc[TTY_CC_VSUSP];
  uint32_t fg_pgid = tty->fg_pgid;

  if (canon)
    tty_canon_input_locked(tty, byte);
  else
    tty_push_byte_locked(tty, byte);

  // [NOFLSH] No exponemos NOFLSH a userland, así que aplicamos el
  // comportamiento por defecto: al recibir VINTR/VQUIT/VSUSP con ISIG
  // activo, vaciamos el input pendiente (buffer + línea canonical)
  // antes de enviar la señal. Sin esto, tras un Ctrl+C el shell vería
  // basura tecleada antes del ^C.
  if (isig && (byte == cc_vintr || byte == cc_vquit || byte == cc_vsusp)) {
    tty->head = 0;
    tty->tail = 0;
    tty->count = 0;
    tty->canon_len = 0;
    tty->eof_pending = 0;
  }

  spin_unlock_irqrestore(&tty->lock, flags);

  wake_up_all(&tty->read_wq);

  if (echo && tty->pty) {
    if (byte == '\n') {
      pty_slave_emit(tty->pty, "\r\n", 2);
    } else if (byte == '\b' || byte == 0x7F) {
      pty_slave_emit(tty->pty, "\b \b", 3);
    } else if (byte >= 0x20 && byte < 0x7F) {
      pty_slave_emit(tty->pty, &byte, 1);
    } else if ((tty->lflag & TTY_LFLAG_ECHOCTL) && byte < 0x20 &&
               byte != '\t') {
      // ^X para todos los controles excepto TAB.
      char seq[2] = {'^', (char)(byte + '@')};
      pty_slave_emit(tty->pty, seq, 2);
    }
  }

  // [JOB] Señales de control al foreground pgrp.
  if (isig && fg_pgid) {
    if (byte == cc_vintr) {
      process_signal_pgrp(fg_pgid, 1ULL << SIGINT);
    } else if (byte == cc_vquit) {
      process_signal_pgrp(fg_pgid, 1ULL << SIGQUIT);
    } else if (byte == cc_vsusp) {
      process_signal_pgrp(fg_pgid, 1ULL << SIGTSTP);
    }
  }
}

// ---------------------------------------------------------------------------
// Vaciar el buffer de entrada.
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

// ---------------------------------------------------------------------------
// tty_write: salida del proceso que tiene el fd abierto.
//
// Dos casos:
//
//   1. Console tty (tty->pty == NULL):
//      escribe a serial + winsrv. Es el modo original.
//
//   2. Slave de un PTY (tty->pty != NULL):
//      los bytes van al buffer del master del par, no a winsrv ni a
//      serial. El terminal app (que tiene el master fd abierto) los lee
//      y los dibuja en su propia ventana.
//
// En Fase 1 ningún tty tiene pty != NULL, así que la rama 2 nunca se
// ejecuta. Se deja preparada para Fase 2.
// ---------------------------------------------------------------------------
int64_t tty_write(tty_t *tty, uint64_t offset, size_t size, const void *buf) {
  (void)offset;
  if (!tty || !buf || size == 0)
    return 0;

  if (tty->pty) {
    // El slave escribe: emitir al buffer del master. El terminal app
    // (que tiene el master fd) lo leerá y lo pintará.
    pty_slave_emit(tty->pty, buf, size);
    return (int64_t)size;
  }

  // Console tty: escribir a serial + winsrv.
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
#define TIOCSCTTY 0x540E

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
    int32_t pg = (int32_t)tty->fg_pgid;
    if (!access_ok((void *)arg, sizeof(pg)))
      return -EFAULT;
    if (copy_to_user((void *)arg, &pg, sizeof(pg)) < 0)
      return -EFAULT;
    return 0;
  }
  case TIOCSPGRP: {
    if (!access_ok((void *)arg, sizeof(int32_t)))
      return -EFAULT;
    int32_t pg;
    if (copy_from_user(&pg, (void *)arg, sizeof(pg)) < 0)
      return -EFAULT;
    if (pg < 0)
      return -EINVAL;
    // Linux: ESRCH si el pgrp no existe; EPERM solo si pertenece a otra
    // sesión. No validamos sesión todavía.
    if (pg > 0 && !process_pgrp_exists((uint32_t)pg))
      return -ESRCH;
    tty->fg_pgid = (uint32_t)pg;
    return 0;
  }
  case TIOCSCTTY: {
    // [JOB] Tomar posesión de este tty como terminal de control.
    // Solo un session leader puede hacerlo.
    process_t *p = process_current();
    if (!p)
      return -EFAULT;
    if (p->sid != p->pid)
      return -EPERM;
    if (tty->session_leader_pid != 0 && tty->session_leader_pid != p->pid)
      return -EPERM;
    tty->session_leader_pid = p->pid;
    tty->fg_pgid = p->pgid;
    p->ctty = tty;
    return 0;
  }
  case TIOCGSID: {
    process_t *proc = process_current();
    int32_t sid = proc ? (int32_t)proc->pid : 0;
    if (!access_ok((void *)arg, sizeof(sid)))
      return -EFAULT;
    if (copy_to_user((void *)arg, &sid, sizeof(sid)) < 0)
      return -EFAULT;
    return 0;
  }
  case TIOCNOTTY: {
    process_t *p = process_current();
    if (p)
      p->ctty = NULL;
  }
    return 0;
  case TCFLSH: {
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

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void tty_init(void) {
  memset(&tty0, 0, sizeof(tty0));
  spin_init(&tty0.lock);
  wait_queue_init(&tty0.read_wq);
  tty_set_defaults(&tty0);
  tty0.winsize_cols = 100;
  tty0.winsize_rows = 35;
  // [PTY Fase 1] tty0 es el console tty, no el slave de ningún PTY.
  // memset ya lo dejó a NULL, lo repetimos explícito por claridad.
  tty0.pty = NULL;
  tty_ready = 1;
  LOG_INFO("[TTY] Inicializado (canon+echo+isig, 100x35)");
}

tty_t *tty_console(void) { return tty_ready ? &tty0 : NULL; }

void tty_set_console_window(void *win) { (void)win; }