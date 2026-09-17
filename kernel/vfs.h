// kernel/vfs.h
#ifndef VFS_H
#define VFS_H

#include "wait.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VFS_FILE 0x01
#define VFS_DIRECTORY 0x02
#define VFS_CHARDEVICE 0x03

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_CREAT 0x0040

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define MAX_PROCESS_FDS 32

typedef struct vfs_node vfs_node_t;

typedef struct vfs_stat {
  uint32_t flags;
  size_t size;
  uint32_t inode;
} vfs_stat_t;

typedef struct vfs_ops {
  int64_t (*read)(vfs_node_t *node, uint64_t offset, size_t size, void *buf);
  int64_t (*write)(vfs_node_t *node, uint64_t offset, size_t size,
                   const void *buf);
  int (*open)(vfs_node_t *node, int flags);
  int (*close)(vfs_node_t *node);
  bool (*readable)(vfs_node_t *node);
} vfs_ops_t;

struct vfs_node {
  char name[128];
  uint32_t flags;
  size_t size;
  uint32_t inode;
  vfs_ops_t *ops;
  void *priv;
};

typedef struct file_descriptor {
  vfs_node_t *node;
  uint64_t offset;
  int flags;
  int ref_count;
  wait_queue_t read_wq;
  wait_queue_t write_wq;
} file_descriptor_t;

void vfs_init(void);
vfs_node_t *vfs_lookup(const char *path);

int vfs_open_for_proc(void *proc_ptr, const char *path, int flags);
int vfs_close_for_proc(void *proc_ptr, int fd);
int64_t vfs_read_for_proc(void *proc_ptr, int fd, void *buf, size_t count);
int64_t vfs_write_for_proc(void *proc_ptr, int fd, const void *buf,
                           size_t count);
int64_t vfs_seek_for_proc(void *proc_ptr, int fd, int64_t offset, int whence);
int vfs_fstat_for_proc(void *proc_ptr, int fd, vfs_stat_t *st);

int vfs_wait_readable(void *proc_ptr, int fd);
void vfs_notify_readable(file_descriptor_t *fd);
void vfs_notify_writable(file_descriptor_t *fd);

file_descriptor_t *vfs_create_stdio_fd(int stdio_type);

#endif