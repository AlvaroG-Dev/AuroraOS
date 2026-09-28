// kernel/tty.h
#ifndef KERNEL_TTY_H
#define KERNEL_TTY_H

#include "spinlock.h"
#include "wait.h"
#include <stddef.h>
#include <stdint.h>

#define TTY_BUF_SIZE 256

#define TTY_CC_VINTR 0
#define TTY_CC_VQUIT 1
#define TTY_CC_VERASE 2
#define TTY_CC_VKILL 3
#define TTY_CC_VEOF 4
#define TTY_CC_VSTART 5
#define TTY_CC_VSTOP 6
#define TTY_CC_VSUSP 7
#define TTY_CC_VMIN 8
#define TTY_CC_VTIME 9
#define TTY_CC_N 32

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
#define TTY_LFLAG_ECHOCTL 0x00000200

// [PTY Fase 1] Forward declaration. El par PTY (master buffer +
// slave) se define en kernel/pty.h (Fase 2). Aquí solo necesitamos
// un puntero opaco para que tty_t sepa si es el slave de un PTY.
struct tty_pty;

typedef struct tty {
  spinlock_t lock;

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

  uint8_t canon_buf[TTY_BUF_SIZE];
  size_t canon_len;

  int eof_pending;

  // [PTY Fase 1] Si != NULL, este tty es el slave de un PTY y su
  // dueño es este pty_pair. La escritura al slave va al buffer del
  // master del par (ver kernel/pty.c, Fase 2), no a winsrv/serial.
  // NULL para tty0 (console tty del kernel, el único que existe hoy).
  struct tty_pty *pty;

  // [JOB] Process group en foreground del terminal. Ctrl+C / Ctrl+Z se
  // envían a este pgrp. 0 = sin dueño (nadie recibe señales del tty).
  uint32_t fg_pgid;

  // [JOB] Session leader que posee este tty (0 = libre). Lo setea
  // TIOCSCTTY y lo consulta el código de terminación cuando el líder
  // muere.
  uint32_t session_leader_pid;
} tty_t;

void tty_init(void);

// [PTY Fase 1] tty de la consola del kernel (tty0). Es el que usan
// los procesos que no abren un PTY (tests, kmain_task, y el terminal
// app antes de migrar a Fase 7).
tty_t *tty_console(void);

// [PTY Fase 1] Compat temporal: algunos módulos antiguos usan
// tty_default(). Envoltorio inline para no romper nada mientras
// migramos a tty_console().
static inline tty_t *tty_default(void) { return tty_console(); }

void tty_set_console_window(void *win);

// Input del driver PS/2. Procesa canon + echo, y postea
// WINSRV_EV_TTY_INPUT al shell. Solo se usa para el console tty.
void tty_receive_char(tty_t *tty, char c);

// [PTY Fase 1] Input desde el master fd de un PTY hacia su slave.
// Diferencias con tty_receive_char:
//   - No hace eco al console (no hay ventana asociada al slave).
//   - No postea WINSRV_EV_TTY_INPUT (ese canal es del console tty).
//   - Solo procesa el byte (canon o raw) y despierta a los lectores.
// Los bytes acaban en el ring buffer del slave y los lee el proceso
// que tiene el slave fd abierto (por ejemplo /bin/sh).
void tty_slave_receive(tty_t *tty, uint8_t byte);

// [PTY Fase 2] Emitir bytes desde el slave al master del PTY. Se
// define en pty.c. tty_slave_receive la llama para el eco.
// El struct tty_pty ya está forward-declared arriba.
void pty_slave_emit(struct tty_pty *pty, const void *buf, size_t size);

void tty_flush_input(tty_t *tty);

int64_t tty_read(tty_t *tty, uint64_t offset, size_t size, void *buf);
int64_t tty_write(tty_t *tty, uint64_t offset, size_t size, const void *buf);
int tty_poll(tty_t *tty, short events);
int64_t tty_ioctl(tty_t *tty, unsigned long req, uint64_t arg);

// [PTY Fase 2] Reset del termios por defecto (canon+echo+isig) y
// winsize. Lo usa pty_alloc para inicializar el slave de cada PTY.
void tty_set_defaults(tty_t *tty);

#endif