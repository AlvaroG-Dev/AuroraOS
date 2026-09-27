// kernel/tty.h
#ifndef KERNEL_TTY_H
#define KERNEL_TTY_H

#include "spinlock.h"
#include "wait.h"
#include <stddef.h>
#include <stdint.h>

// Buffer de entrada. 256 bytes cubren uso interactivo y paste pequeño.
// Si se llena, se descartan bytes nuevos (el driver no puede bloquearse).
#define TTY_BUF_SIZE 256

// Índices en cc[].
#define TTY_CC_VINTR 0  // Ctrl+C
#define TTY_CC_VQUIT 1  // Ctrl+backslash (0x1C)
#define TTY_CC_VERASE 2 // Backspace / DEL
#define TTY_CC_VKILL 3  // Ctrl+U
#define TTY_CC_VEOF 4   // Ctrl+D
#define TTY_CC_VSTART 5 // Ctrl+Q
#define TTY_CC_VSTOP 6  // Ctrl+S
#define TTY_CC_VSUSP 7  // Ctrl+Z
#define TTY_CC_VMIN 8
#define TTY_CC_VTIME 9
#define TTY_CC_N 32

// iflag / oflag / lflag / cflag.
#define TTY_IFLAG_ICRNL 0x00000100
#define TTY_OFLAG_OPOST 0x00000001
#define TTY_OFLAG_ONLCR 0x00000004
#define TTY_LFLAG_ISIG 0x00000001
#define TTY_LFLAG_ICANON 0x00000002
#define TTY_LFLAG_ECHO 0x00000008
#define TTY_LFLAG_ECHOE 0x00000010
#define TTY_LFLAG_ECHOK 0x00000020
#define TTY_CFLAG_CREAD 0x00000080
#define TTY_CFLAG_CS8 0x00000030
#define TTY_CFLAG_B38400 0x00000F00


typedef struct tty {
  spinlock_t lock;

  // Ring buffer de entrada (post-canonicalización).
  uint8_t buf[TTY_BUF_SIZE];
  size_t head;
  size_t tail;
  size_t count;

  wait_queue_t read_wq;

  uint32_t iflag;
  uint32_t oflag;
  uint32_t cflag;
  uint32_t lflag;
  uint8_t cc[TTY_CC_N];

  uint32_t winsize_rows;
  uint32_t winsize_cols;

  // Acumulador canónico.
  uint8_t canon_buf[TTY_BUF_SIZE];
  size_t canon_len;

  int eof_pending;
} tty_t;

void tty_init(void);
tty_t *tty_default(void);
void tty_set_console_window(void *win);
void tty_receive_char(tty_t *tty, char c);

int64_t tty_read(tty_t *tty, uint64_t offset, size_t size, void *buf);
int64_t tty_write(tty_t *tty, uint64_t offset, size_t size, const void *buf);
int tty_poll(tty_t *tty, short events);
int64_t tty_ioctl(tty_t *tty, unsigned long req, uint64_t arg);

#endif