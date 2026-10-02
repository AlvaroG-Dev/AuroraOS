// kernel/pipe.c
//
// Pipe anónimo. Dos nodos VFS (read end / write end) que comparten un
// mismo pipe_t. El buffer es un ring de PIPE_BUF_SIZE bytes.
//
// Semántica:
//   - read: si no hay datos y quedan writers, bloquea. Si no hay datos
//     y no quedan writers, devuelve 0 (EOF).
//   - write: si no hay hueco y quedan readers, bloquea. Si no quedan
//     readers, devuelve -EPIPE (el shell lo trata como error).
//
// refcount:
//   - pipe->readers y pipe->writers cuentan nodos VFS vivos, no
//     holders del fd. El refcount del file_descriptor_t (gestionado por
//     vfs_close_for_proc) garantiza que un nodo solo se libera cuando el
//     último holder del fd cierra. En ese momento pipe_close decrementa
//     el contador correspondiente exactamente una vez.
//   - Cuando ambos contadores llegan a 0, se libera pipe_t.

#include "pipe.h"
#include "heap.h"
#include "klog.h"
#include "process.h"
#include "sched.h"
#include "spinlock.h"
#include "string.h"
#include "uaccess.h"
#include "wait.h"
#include <stddef.h>

#ifndef EPIPE
#define EPIPE 32
#endif

typedef struct pipe {
  uint8_t buf[PIPE_BUF_SIZE];
  size_t head;  // índice de escritura
  size_t tail;  // índice de lectura
  size_t count; // bytes en el buffer
  int readers;
  int writers;
  int refs; // extremos VFS vivos (2 al crear). Controla el kfree(pipe_t).
  wait_queue_t read_wq;
  wait_queue_t write_wq;
  spinlock_t lock;
} pipe_t;

typedef struct {
  pipe_t *pipe;
  int is_write; // 0 = read end, 1 = write end
} pipe_end_t;

// ---------------------------------------------------------------------------
// Condiciones de despertar
// ---------------------------------------------------------------------------
static bool pipe_has_data(void *arg) {
  pipe_t *p = (pipe_t *)arg;

  /*
   * wait_common() evalúa la condición mientras mantiene wq->lock.
   * No debemos tomar p->lock aquí: además de introducir una segunda
   * jerarquía de locks en el camino de wait/wake, la condición solo
   * necesita una observación consistente de estos campos.
   *
   * Los escritores actualizan count/writers bajo p->lock. Las cargas
   * acquire hacen explícita la publicación de esos cambios entre CPUs;
   * el camino que realmente consume datos vuelve a comprobar count bajo
   * p->lock antes de tocar el ring.
   */
  size_t count = __atomic_load_n(&p->count, __ATOMIC_ACQUIRE);
  int writers = __atomic_load_n(&p->writers, __ATOMIC_ACQUIRE);
  return count > 0 || writers == 0;
}

static bool pipe_has_space(void *arg) {
  pipe_t *p = (pipe_t *)arg;

  /* Esta función también se ejecuta con wq->lock cogido por
   * wait_common(), así que nunca toma p->lock. */
  size_t count = __atomic_load_n(&p->count, __ATOMIC_ACQUIRE);
  int readers = __atomic_load_n(&p->readers, __ATOMIC_ACQUIRE);
  return count < PIPE_BUF_SIZE || readers == 0;
}
// ---------------------------------------------------------------------------
// Ops
// ---------------------------------------------------------------------------
static int64_t pipe_read(vfs_node_t *node, uint64_t offset, size_t size,
                         void *buf) {
  (void)offset;
  pipe_end_t *end = (pipe_end_t *)node->priv;
  if (!end || !buf)
    return -EINVAL;
  pipe_t *p = end->pipe;
  if (end->is_write)
    return -EINVAL;
  if (size == 0)
    return 0;

  for (;;) {
    int rc = wait_event_interruptible(&p->read_wq, pipe_has_data, p);
    if (rc < 0)
      return rc;

    unsigned long flags = spin_lock_irqsave(&p->lock);
    if (p->count > 0) {
      size_t to_read = size;
      if (to_read > p->count)
        to_read = p->count;
      for (size_t i = 0; i < to_read; i++) {
        ((uint8_t *)buf)[i] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
      }
      __atomic_fetch_sub(&p->count, to_read, __ATOMIC_RELEASE);
      spin_unlock_irqrestore(&p->lock, flags);

      wake_up_all(&p->write_wq);
      return (int64_t)to_read;
    }
    if (p->writers == 0) {
      spin_unlock_irqrestore(&p->lock, flags);
      return 0; // EOF
    }
    // Carrera: alguien se llevó los datos. Volver a dormir.
    spin_unlock_irqrestore(&p->lock, flags);
  }
}

static int64_t pipe_write(vfs_node_t *node, uint64_t offset, size_t size,
                          const void *buf) {
  (void)offset;
  pipe_end_t *end = (pipe_end_t *)node->priv;
  if (!end || !buf)
    return -EINVAL;
  pipe_t *p = end->pipe;
  if (!end->is_write)
    return -EINVAL;
  if (size == 0)
    return 0;

  for (;;) {
    int rc = wait_event_interruptible(&p->write_wq, pipe_has_space, p);
    if (rc < 0)
      return rc;

    unsigned long flags = spin_lock_irqsave(&p->lock);
    if (p->readers == 0) {
      spin_unlock_irqrestore(&p->lock, flags);
      return -EPIPE;
    }
    size_t avail = PIPE_BUF_SIZE - p->count;
    if (avail > 0) {
      size_t to_write = size;
      if (to_write > avail)
        to_write = avail;
      for (size_t i = 0; i < to_write; i++) {
        p->buf[p->head] = ((const uint8_t *)buf)[i];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
      }
      __atomic_fetch_add(&p->count, to_write, __ATOMIC_RELEASE);
      spin_unlock_irqrestore(&p->lock, flags);

      wake_up_all(&p->read_wq);
      return (int64_t)to_write;
    }
    spin_unlock_irqrestore(&p->lock, flags);
  }
}

static int pipe_open(vfs_node_t *node, int flags) {
  (void)node;
  (void)flags;
  return 0;
}

static int pipe_close(vfs_node_t *node) {
  pipe_end_t *end = (pipe_end_t *)node->priv;
  if (!end)
    return 0;
  pipe_t *p = end->pipe;
  int is_write = end->is_write;

  unsigned long flags = spin_lock_irqsave(&p->lock);
  if (is_write)
    __atomic_fetch_sub(&p->writers, 1, __ATOMIC_RELEASE);
  else
    __atomic_fetch_sub(&p->readers, 1, __ATOMIC_RELEASE);
  int new_readers = p->readers;
  int new_writers = p->writers;
  spin_unlock_irqrestore(&p->lock, flags);

  LOG_TRACE("[PIPE-CLOSE] is_write=%d -> readers=%d writers=%d", is_write,
            new_readers, new_writers);

  kfree(end);
  node->priv = NULL;

  // Despertar al otro extremo para que vea EOF o -EPIPE.
  // p sigue vivo: todavía tenemos nuestra referencia (refs).
  wake_up_all(&p->read_wq);
  wake_up_all(&p->write_wq);

  // Solo el último extremo en llegar aquí libera pipe_t. Se hace
  // DESPUÉS de los wake_up, así nadie toca p tras el kfree.
  if (__atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL) == 0)
    kfree(p);
  return 0;
}

static vfs_ops_t pipe_read_ops = {
    .read = pipe_read,
    .write = NULL,
    .open = pipe_open,
    .close = pipe_close,
    .readable = NULL,
};

static vfs_ops_t pipe_write_ops = {
    .read = NULL,
    .write = pipe_write,
    .open = pipe_open,
    .close = pipe_close,
    .readable = NULL,
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
int vfs_pipe_create(vfs_node_t **read_end, vfs_node_t **write_end) {
  if (!read_end || !write_end)
    return -EINVAL;

  pipe_t *p = (pipe_t *)kzalloc(sizeof(pipe_t));
  pipe_end_t *re = (pipe_end_t *)kzalloc(sizeof(pipe_end_t));
  pipe_end_t *we = (pipe_end_t *)kzalloc(sizeof(pipe_end_t));
  vfs_node_t *rn = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
  vfs_node_t *wn = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
  if (!p || !re || !we || !rn || !wn) {
    kfree(p);
    kfree(re);
    kfree(we);
    kfree(rn);
    kfree(wn);
    return -ENOMEM;
  }

  wait_queue_init(&p->read_wq);
  wait_queue_init(&p->write_wq);
  spin_init(&p->lock);
  p->readers = 1;
  p->writers = 1;
  p->refs = 2; // [FIX] un ref por extremo

  re->pipe = p;
  re->is_write = 0;
  we->pipe = p;
  we->is_write = 1;

  rn->name[0] = 'p';
  rn->name[1] = '\0';
  rn->flags = VFS_FILE;
  rn->size = 0;
  rn->inode = 0;
  rn->ops = &pipe_read_ops;
  rn->fs = NULL;
  rn->priv = re;

  wn->name[0] = 'p';
  wn->name[1] = '\0';
  wn->flags = VFS_FILE;
  wn->size = 0;
  wn->inode = 0;
  wn->ops = &pipe_write_ops;
  wn->fs = NULL;
  wn->priv = we;

  *read_end = rn;
  *write_end = wn;
  return 0;
}