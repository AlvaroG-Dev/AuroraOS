// kernel/pty.h
//
// [PTY Fase 2] Par master/slave para terminales.
//
// Un PTY es un canal bidireccional:
//   - El MASTER lo tiene el terminal app. Lee lo que el slave escribe
//     (stdout del shell) y escribe lo que el usuario teclea.
//   - El SLAVE es un tty_t completo (canon, echo, isig). El shell lo
//     tiene como fd 0/1/2. Lee input del usuario y escribe output.
//
// Direcciones:
//   slave→master  bytes que el slave escribe (tty_write del slave).
//                 El terminal los lee con pty_master_read.
//   master→slave  bytes que el terminal escribe (ttyping).
//                 pty_master_write los procesa como input del slave.
//
// El slave es un tty_t embebido. El campo `slave.pty` apunta de vuelta
// al pty_pty_t contenedor. Así tty_write() del slave sabe desviar la
// salida al buffer del master en vez de a winsrv/serial.

#ifndef KERNEL_PTY_H
#define KERNEL_PTY_H

#include "tty.h"
#include <stddef.h>
#include <stdint.h>

#define PTY_MAX 16
#define PTY_M_BUF_SIZE 4096

struct process;

typedef struct tty_pty {
  int in_use;
  uint32_t index;   // N para /dev/pts/N
  int slave_locked; // TIOCSPTLCK: 1 = slave cerrado a open()
  int master_open;  // refcount del master (0 = nadie tiene el fd)
  int slave_open;   // refcount del slave

  // Slave: tty_t completo (canon, echo, isig, winsize, termios).
  // tty.slave.pty apunta a este mismo struct.
  tty_t slave;

  // Master: buffer slave→master. Bytes que el slave escribió y que el
  // terminal aún no ha leído.
  uint8_t m_buf[PTY_M_BUF_SIZE];
  size_t m_head;
  size_t m_tail;
  size_t m_count;
  spinlock_t m_lock;
  wait_queue_t m_read_wq;
} tty_pty_t;

// Constantes Linux para ioctls del master.
#define TIOCGPTN 0x80045430u   // _IOR('T', 0x30, unsigned int)
#define TIOCSPTLCK 0x40045431u // _IOW('T', 0x31, int)

void pty_init(void);

// Reserva un PTY libre. Devuelve NULL si no hay. El slave queda con
// termios por defecto (canon+echo+isig) y winsize 80x24.
tty_pty_t *pty_alloc(void);

// Libera un PTY (lo marca in_use=0). No lo llama el usuario directamente:
// lo llaman pty_release_master/slave cuando el último fd se cierra.
void pty_free(tty_pty_t *pty);

// Llamado desde file_descriptor_t.priv_release al cerrar el último fd
// del master o del slave. Cuando ambos contadores llegan a 0, llama a
// pty_free.
void pty_release_master(void *pty_ptr);
void pty_release_slave(void *pty_ptr);

// Byte a byte: procesa los bytes como input del slave (canon/push).
// Si ECHO está activo, el eco va al master (ver tty_slave_receive).
// Devuelve el número de bytes consumidos.
int64_t pty_master_write(tty_pty_t *pty, const void *buf, size_t size);

// Lee bytes que el slave escribió (stdout). Bloquea hasta que haya algo.
int64_t pty_master_read(tty_pty_t *pty, void *buf, size_t size);

// Llamado desde tty_write() cuando el slave escribe. Copia los bytes
// al buffer slave→master y despierta a quien lea del master.
void pty_slave_emit(struct tty_pty *pty, const void *buf, size_t size);

// Condiciones para poll/wait_event.
bool pty_master_readable(tty_pty_t *pty);
bool pty_slave_readable(tty_pty_t *pty);

// [Fase 3] Abre un fd sobre /dev/ptmx (master) o /dev/pts/N (slave).
//
// El llamante (vfs_open_for_proc) ya ha reservado `fd_num` en la tabla
// de fds del proceso. Estas funciones:
//   1. Allocan el pty (master) o validan el existente (slave).
//   2. Construyen un vfs_node_t sintético con los ops de master/slave.
//   3. Construyen el file_descriptor_t y lo publican en proc->fds[fd_num].
//
// Devuelven 0 en éxito, negativo en error (fd_num queda sin usar).
int pty_open_master_fd(struct process *proc, int fd_num);
int pty_open_slave_fd(struct process *proc, int fd_num, int index);

// [PTY] Comprueba si el slot `idx` está en uso. 1 si sí.
int pty_is_in_use(int idx);

// [JOB] Si `fd` es un fd del slave de un PTY, devuelve el tty_pty_t
// (con el slave dentro). NULL si no lo es.
struct file_descriptor;
struct tty_pty *pty_slave_from_fd(struct file_descriptor *fd);

// [JOB] Da el foreground del slave al pgid indicado.
void pty_set_fg_pgid(struct tty_pty *pty, uint32_t pgid);

#endif