// kernel/tty.c
#include "tty.h"
#include "klog.h"
#include "serial.h"
#include "string.h"

#define EINTR 4

static tty_t tty0;
static vfs_node_t tty0_node;
static int tty_ready = 0;

// ---------------------------------------------------------------------------
// Helpers de eco (solo serial por ahora)
// ---------------------------------------------------------------------------
static void tty_echo(char c) {
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
// Recepción de caracteres desde el driver de teclado
// ---------------------------------------------------------------------------
void tty_receive_char(tty_t *tty, char c) {
  if (!tty || !tty_ready)
    return;

  unsigned long flags = spin_lock_irqsave(&tty->lock);

  // Backspace
  if (c == '\b') {
    if (tty->line_len > 0) {
      tty->line_len--;
      spin_unlock_irqrestore(&tty->lock, flags);
      tty_echo('\b');
      return;
    }
    spin_unlock_irqrestore(&tty->lock, flags);
    return;
  }

  // Newline: mover line_buf → done_buf y despertar
  if (c == '\n' || c == '\r') {
    if (tty->has_line) {
      // Ya hay una línea sin consumir; descartar input nuevo.
      spin_unlock_irqrestore(&tty->lock, flags);
      tty_echo('\n');
      return;
    }
    size_t copy = tty->line_len;
    if (copy > TTY_LINE_MAX)
      copy = TTY_LINE_MAX;
    for (size_t i = 0; i < copy; i++)
      tty->done_buf[i] = tty->line_buf[i];
    tty->done_len = copy;
    tty->has_line = 1;
    tty->line_len = 0;
    spin_unlock_irqrestore(&tty->lock, flags);

    tty_echo('\n');
    wake_up_all(&tty->read_wq);
    return;
  }

  // Carácter imprimible
  if (c >= 0x20 && c < 0x7F) {
    if (tty->line_len < TTY_LINE_MAX - 1) {
      tty->line_buf[tty->line_len++] = c;
      spin_unlock_irqrestore(&tty->lock, flags);
      tty_echo(c);
      return;
    }
    spin_unlock_irqrestore(&tty->lock, flags);
    return;
  }

  // Control char desconocido: ignorar.
  spin_unlock_irqrestore(&tty->lock, flags);
}

// ---------------------------------------------------------------------------
// Condición para wait_event
// ---------------------------------------------------------------------------
static bool tty_has_line_cond(void *arg) {
  tty_t *tty = (tty_t *)arg;
  return tty->has_line != 0;
}

int tty_has_line(tty_t *tty) {
  if (!tty)
    return 0;
  unsigned long flags = spin_lock_irqsave(&tty->lock);
  int has = tty->has_line;
  spin_unlock_irqrestore(&tty->lock, flags);
  return has;
}

// ---------------------------------------------------------------------------
// tty_read: bloqueante hasta que haya una línea completa
// ---------------------------------------------------------------------------
int tty_read(tty_t *tty, char *buf, size_t n) {
  if (!tty || !buf || n == 0)
    return -1;

  int rc = wait_event_interruptible(&tty->read_wq, tty_has_line_cond, tty);
  if (rc < 0)
    return rc;

  unsigned long flags = spin_lock_irqsave(&tty->lock);

  if (!tty->has_line) {
    // Race raro: otro lector se llevó la línea.
    spin_unlock_irqrestore(&tty->lock, flags);
    return 0;
  }

  size_t copy = tty->done_len;
  if (copy > n)
    copy = n;

  for (size_t i = 0; i < copy; i++)
    buf[i] = tty->done_buf[i];

  tty->has_line = 0;
  tty->done_len = 0;

  spin_unlock_irqrestore(&tty->lock, flags);
  return (int)copy;
}

// ---------------------------------------------------------------------------
// VFS ops para /dev/tty
// ---------------------------------------------------------------------------
static bool tty_vfs_readable(vfs_node_t *node) {
  (void)node;
  return tty_has_line(&tty0);
}

static int64_t tty_vfs_read(vfs_node_t *node, uint64_t offset, size_t size,
                            void *buf) {
  (void)node;
  (void)offset;
  if (!buf || size == 0)
    return -1;
  // El VFS ya hizo stac/clac; buf es dirección de kernel válida.
  return (int64_t)tty_read(&tty0, (char *)buf, size);
}

static int64_t tty_vfs_write(vfs_node_t *node, uint64_t offset, size_t size,
                             const void *buf) {
  (void)node;
  (void)offset;
  if (!buf || size == 0)
    return 0;

  const char *str = (const char *)buf;
  for (size_t i = 0; i < size; i++) {
    if (str[i] == '\n') {
      serial_putc('\r');
      serial_putc('\n');
    } else {
      serial_putc(str[i]);
    }
  }
  return (int64_t)size;
}

static vfs_ops_t tty_ops = {
    .read = tty_vfs_read,
    .write = tty_vfs_write,
    .open = NULL,
    .close = NULL,
    .readable = tty_vfs_readable,
};

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void tty_init(void) {
  spin_init(&tty0.lock);
  wait_queue_init(&tty0.read_wq);
  tty0.line_len = 0;
  tty0.done_len = 0;
  tty0.has_line = 0;

  memset(&tty0_node, 0, sizeof(tty0_node));
  strcpy(tty0_node.name, "tty");
  tty0_node.flags = VFS_CHARDEVICE;
  tty0_node.ops = &tty_ops;

  tty_ready = 1;
  LOG_INFO("[TTY] /dev/tty0 inicializado");
}

tty_t *tty_default(void) { return tty_ready ? &tty0 : NULL; }

vfs_node_t *tty_get_node(void) { return tty_ready ? &tty0_node : NULL; }