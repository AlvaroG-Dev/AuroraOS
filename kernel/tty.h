// kernel/tty.h
#ifndef KERNEL_TTY_H
#define KERNEL_TTY_H

#include "spinlock.h"
#include "vfs.h"
#include "wait.h"
#include <stddef.h>
#include <stdint.h>

// TTY mínimo con line discipline.
//
// Modelo:
//   - El driver de teclado llama a tty_receive_char() con caracteres ASCII
//     ya traducidos.
//   - El TTY acumula caracteres en line_buf hasta ver '\n'. En ese momento
//     marca la línea como completa y despierta a los lectores.
//   - tty_read() copia la línea completa al buffer del llamante y limpia
//     line_buf. Si no hay línea, bloquea.
//   - El nodo VFS que expone tty.c tiene ops->read que llama a tty_read().
//     Eso hace que read(0, buf, n) en el shell se bloquee hasta que haya
//     una línea completa.

#define TTY_LINE_MAX 256

typedef struct tty {
  spinlock_t lock;
  wait_queue_t read_wq;

  char line_buf[TTY_LINE_MAX]; // línea en construcción
  size_t line_len;

  char done_buf[TTY_LINE_MAX]; // línea completa lista para leer
  size_t done_len;
  int has_line;
} tty_t;

// Inicializa el TTY y crea su nodo VFS.
void tty_init(void);

// Devuelve el TTY principal. No NULL tras tty_init.
tty_t *tty_default(void);

// Devuelve el nodo VFS asociado a /dev/tty (o NULL si tty_init no corrió).
vfs_node_t *tty_get_node(void);

// Llamado desde el driver de teclado con un carácter ASCII ya traducido.
void tty_receive_char(tty_t *tty, char c);

// Lee una línea completa. Bloquea si no hay.
// Retorna el número de bytes copiados, o negativo si fue cancelada.
int tty_read(tty_t *tty, char *buf, size_t n);

// ¿Hay una línea completa lista?
int tty_has_line(tty_t *tty);

#endif