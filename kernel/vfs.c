// kernel/vfs.c
#include "vfs.h"
#include "cpu.h"
#include "gfx/winsrv.h"
#include "heap.h"
#include "klog.h"
#include "process.h"
#include "serial.h"
#include "string.h"
#include "tarfs.h"
#include "tty.h" // [TTY] tty_get_node()
#include <stddef.h>

#define EINTR 4

// --- Operaciones TarFS VFS ---
static int64_t tar_vfs_read(vfs_node_t *node, uint64_t offset, size_t size,
                            void *buf) {
  if (!node || !node->priv || !buf)
    return -1;
  tar_node_t *tn = (tar_node_t *)node->priv;
  if (offset >= tn->size)
    return 0;

  size_t to_read = size;
  if (offset + to_read > tn->size) {
    to_read = tn->size - offset;
  }
  memcpy(buf, tn->data + offset, to_read);
  return (int64_t)to_read;
}

static int64_t tar_vfs_write(vfs_node_t *node, uint64_t offset, size_t size,
                             const void *buf) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buf;
  return -1;
}

static int tar_vfs_open(vfs_node_t *node, int flags) {
  (void)node;
  if ((flags & O_WRONLY) || (flags & O_RDWR))
    return -1;
  return 0;
}

static int tar_vfs_close(vfs_node_t *node) {
  (void)node;
  return 0;
}

static vfs_ops_t tar_ops = {
    .read = tar_vfs_read,
    .write = tar_vfs_write,
    .open = tar_vfs_open,
    .close = tar_vfs_close,
    .readable = NULL,
};

// --- Consola (stdout, stderr) ---
static int64_t console_vfs_read(vfs_node_t *node, uint64_t offset, size_t size,
                                void *buf) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buf;
  return 0;
}

static int64_t console_vfs_write(vfs_node_t *node, uint64_t offset, size_t size,
                                 const void *buf) {
  (void)node;
  (void)offset;
  if (!buf || size == 0)
    return 0;
  const char *str = (const char *)buf;
  unsigned long flags;
  serial_lock_acquire(&flags);
  for (size_t i = 0; i < size; i++) {
    serial_putc_locked(str[i]);
    winsrv_console_output(str[i]); // ← NUEVO
  }
  serial_lock_release(flags);
  return (int64_t)size;
}

static vfs_ops_t console_ops = {
    .read = console_vfs_read,
    .write = console_vfs_write,
    .open = NULL,
    .close = NULL,
    .readable = NULL,
};

static vfs_node_t stdin_node;
static vfs_node_t stdout_node;
static vfs_node_t stderr_node;

void vfs_init(void) {
  memset(&stdin_node, 0, sizeof(stdin_node));
  strcpy(stdin_node.name, "stdin");
  stdin_node.flags = VFS_CHARDEVICE;
  stdin_node.ops = &console_ops;

  memset(&stdout_node, 0, sizeof(stdout_node));
  strcpy(stdout_node.name, "stdout");
  stdout_node.flags = VFS_CHARDEVICE;
  stdout_node.ops = &console_ops;

  memset(&stderr_node, 0, sizeof(stderr_node));
  strcpy(stderr_node.name, "stderr");
  stderr_node.flags = VFS_CHARDEVICE;
  stderr_node.ops = &console_ops;

  LOG_INFO("[VFS] Capa Virtual File System inicializada.");
}

// ---------------------------------------------------------------------------
// Helpers internos
// ---------------------------------------------------------------------------
static void fd_init_wqs(file_descriptor_t *fd) {
  wait_queue_init(&fd->read_wq);
  wait_queue_init(&fd->write_wq);
}

file_descriptor_t *vfs_create_stdio_fd(int stdio_type) {
  file_descriptor_t *fd =
      (file_descriptor_t *)kmalloc(sizeof(file_descriptor_t));
  if (!fd)
    return NULL;
  fd->offset = 0;
  fd->ref_count = 1;
  fd_init_wqs(fd);

  if (stdio_type == 0) {
    fd->node = &stdin_node; // ← vuelve al stdin clásico
    fd->flags = O_RDONLY;
  } else if (stdio_type == 1) {
    fd->node = &stdout_node;
    fd->flags = O_WRONLY;
  } else {
    fd->node = &stderr_node;
    fd->flags = O_WRONLY;
  }
  return fd;
}

vfs_node_t *vfs_lookup(const char *path) {
  if (!path || path[0] == '\0')
    return NULL;

  tar_node_t *tn = tarfs_open(path);
  if (!tn)
    return NULL;

  vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
  if (!node)
    return NULL;

  memset(node, 0, sizeof(vfs_node_t));
  size_t nlen = 0;
  while (tn->name[nlen] && nlen < sizeof(node->name) - 1) {
    node->name[nlen] = tn->name[nlen];
    nlen++;
  }
  node->name[nlen] = '\0';
  node->flags = tn->is_dir ? VFS_DIRECTORY : VFS_FILE;
  node->size = tn->size;
  node->ops = &tar_ops;
  node->priv = tn;
  return node;
}

static void vfs_node_free(vfs_node_t *node) {
  if (!node)
    return;
  if (node == &stdin_node || node == &stdout_node || node == &stderr_node)
    return;
  if (node->ops && node->ops->close)
    node->ops->close(node);
  kfree(node);
}

int vfs_open_for_proc(void *proc_ptr, const char *path, int flags) {
  process_t *proc = (process_t *)proc_ptr;
  if (!proc || !path)
    return -1;

  int free_fd = -1;
  for (int i = 0; i < MAX_PROCESS_FDS; i++) {
    if (!proc->fds[i]) {
      free_fd = i;
      break;
    }
  }
  if (free_fd == -1)
    return -1;

  vfs_node_t *node = vfs_lookup(path);
  if (!node)
    return -1;

  if (node->ops && node->ops->open) {
    if (node->ops->open(node, flags) != 0) {
      kfree(node);
      return -1;
    }
  }

  file_descriptor_t *fd_entry =
      (file_descriptor_t *)kmalloc(sizeof(file_descriptor_t));
  if (!fd_entry) {
    vfs_node_free(node);
    return -1;
  }

  fd_entry->node = node;
  fd_entry->offset = 0;
  fd_entry->flags = flags;
  fd_entry->ref_count = 1;
  fd_init_wqs(fd_entry);

  proc->fds[free_fd] = fd_entry;
  return free_fd;
}

int vfs_close_for_proc(void *proc_ptr, int fd) {
  process_t *proc = (process_t *)proc_ptr;
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -1;

  file_descriptor_t *f = proc->fds[fd];
  proc->fds[fd] = NULL;

  f->ref_count--;
  if (f->ref_count <= 0) {
    vfs_node_free(f->node);
    kfree(f);
  }
  return 0;
}

int64_t vfs_read_for_proc(void *proc_ptr, int fd, void *buf, size_t count) {
  process_t *proc = (process_t *)proc_ptr;
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] || !buf)
    return -1;

  file_descriptor_t *f = proc->fds[fd];
  if (!f->node || !f->node->ops || !f->node->ops->read)
    return -1;

  stac();
  int64_t bytes = f->node->ops->read(f->node, f->offset, count, buf);
  clac();

  if (bytes > 0)
    f->offset += (uint64_t)bytes;
  return bytes;
}

int64_t vfs_write_for_proc(void *proc_ptr, int fd, const void *buf,
                           size_t count) {
  process_t *proc = (process_t *)proc_ptr;
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] || !buf)
    return -1;

  file_descriptor_t *f = proc->fds[fd];
  if (!f->node || !f->node->ops || !f->node->ops->write)
    return -1;

  stac();
  int64_t bytes = f->node->ops->write(f->node, f->offset, count, buf);
  clac();

  if (bytes > 0)
    f->offset += (uint64_t)bytes;
  return bytes;
}

int64_t vfs_seek_for_proc(void *proc_ptr, int fd, int64_t offset, int whence) {
  process_t *proc = (process_t *)proc_ptr;
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -1;

  file_descriptor_t *f = proc->fds[fd];
  if (!f->node)
    return -1;

  uint64_t new_off = f->offset;
  if (whence == SEEK_SET) {
    if (offset < 0)
      return -1;
    new_off = (uint64_t)offset;
  } else if (whence == SEEK_CUR) {
    if (offset < 0 && (uint64_t)(-offset) > f->offset)
      return -1;
    new_off = f->offset + offset;
  } else if (whence == SEEK_END) {
    if (offset < 0 && (uint64_t)(-offset) > f->node->size)
      return -1;
    new_off = f->node->size + offset;
  } else {
    return -1;
  }
  f->offset = new_off;
  return (int64_t)new_off;
}

int vfs_fstat_for_proc(void *proc_ptr, int fd, vfs_stat_t *st) {
  process_t *proc = (process_t *)proc_ptr;
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] || !st)
    return -1;

  file_descriptor_t *f = proc->fds[fd];
  if (!f->node)
    return -1;

  stac();
  st->flags = f->node->flags;
  st->size = f->node->size;
  st->inode = f->node->inode;
  clac();
  return 0;
}

// ---------------------------------------------------------------------------
// API de wait queue por fd (genérica; el TTY no la necesita pero sirve
// para futuros nodos que no bloqueen en read)
// ---------------------------------------------------------------------------
static bool fd_node_readable(void *arg) {
  file_descriptor_t *f = (file_descriptor_t *)arg;
  if (!f || !f->node)
    return true;
  if (!f->node->ops || !f->node->ops->readable)
    return true;
  return f->node->ops->readable(f->node);
}

int vfs_wait_readable(void *proc_ptr, int fd) {
  process_t *proc = (process_t *)proc_ptr;
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -1;

  file_descriptor_t *f = proc->fds[fd];
  if (!f->node || !f->node->ops || !f->node->ops->readable)
    return 0;

  return wait_event_interruptible(&f->read_wq, fd_node_readable, f);
}

void vfs_notify_readable(file_descriptor_t *fd) {
  if (!fd)
    return;
  wake_up_all(&fd->read_wq);
}

void vfs_notify_writable(file_descriptor_t *fd) {
  if (!fd)
    return;
  wake_up_all(&fd->write_wq);
}
