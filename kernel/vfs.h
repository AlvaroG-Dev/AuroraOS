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

// Longitud máxima de un path normalizado (coincide con name[] de vfs_node).
#define VFS_PATH_MAX 128

typedef struct vfs_node vfs_node_t;
typedef struct vfs_fs_ops vfs_fs_ops_t;

typedef struct vfs_stat {
  uint32_t flags;
  size_t size;
  uint32_t inode;
} vfs_stat_t;

typedef struct vfs_dirent {
  char name[VFS_PATH_MAX];
  uint32_t type; // VFS_FILE o VFS_DIRECTORY
  uint64_t size;
} vfs_dirent_t;

// ---------------------------------------------------------------------------
// Ops a nivel de nodo. Cada FS define las suyas. Semántica:
//   read/write: mismo contrato que read(2)/write(2). Devuelven bytes
//     transferidos, 0 en EOF, o negativo en error.
//   open: valida el acceso según flags. Devuelve 0 si OK.
//   close: libera recursos del nodo al cerrar el último fd. Puede ser NULL.
//   readable: para wait_readable. Puede ser NULL (siempre legible).
// ---------------------------------------------------------------------------
typedef struct vfs_ops {
  int64_t (*read)(vfs_node_t *node, uint64_t offset, size_t size, void *buf);
  int64_t (*write)(vfs_node_t *node, uint64_t offset, size_t size,
                   const void *buf);
  int (*open)(vfs_node_t *node, int flags);
  int (*close)(vfs_node_t *node);
  bool (*readable)(vfs_node_t *node);

  int (*create)(vfs_node_t *dir, const char *name, int flags);
  int (*mkdir)(vfs_node_t *dir, const char *name);
  int (*unlink)(vfs_node_t *dir, const char *name);

  // [PR 4.3] Cambia el tamaño del archivo. Reducir libera clusters;
  // crecer requiere que el FS lo soporte (FAT32 devuelve -EINVAL).
  int (*truncate)(vfs_node_t *node, uint64_t new_size);

  // [PR 4.4] Lee la entry `index` del directorio.
  //   *out->name[0] = '\0'    → fin del directorio (devolver 0)
  //   >=0                     → OK, *out rellenado
  //   <0                      → error
  int (*readdir)(vfs_node_t *dir, uint64_t index, vfs_dirent_t *out);
  int (*rename)(vfs_node_t *src_dir, const char *src_name, vfs_node_t *dst_dir,
                const char *dst_name);
} vfs_ops_t;

struct vfs_node {
  char name[VFS_PATH_MAX]; // path completo (en este diseño simplificado)
  uint32_t flags;
  size_t size;
  uint32_t inode;
  vfs_ops_t *ops;
  vfs_fs_ops_t *fs; // FS que produjo este nodo (para dispatch futuro)
  void *priv;       // datos específicos del FS
};

// ---------------------------------------------------------------------------
// Ops a nivel de FS. Cada implementación de FS (tarfs, fat32, ...) provee
// uno de estos structs y lo registra con vfs_mount.
//
// Semántica de lookup:
//   - path es el path RELATIVO al mount point, empezando por '/'.
//     Para un mount en "/" (root), path es el path normalizado completo.
//     Para un mount en "/mnt/sda1", lookup("/mnt/sda1/foo") invoca
//     fs->lookup(priv, "/foo"). lookup("/mnt/sda1") invoca
//     fs->lookup(priv, "/").
//   - Devuelve un vfs_node_t recién kmalloc'd (que el VFS liberará con
//     kfree cuando el último fd se cierre), o NULL si no existe.
// ---------------------------------------------------------------------------
struct vfs_fs_ops {
  vfs_node_t *(*lookup)(void *fs_priv, const char *path);
  const char *name; // para logs
};

typedef struct file_descriptor {
  vfs_node_t *node;
  uint64_t offset;
  int flags;
  int ref_count;
  wait_queue_t read_wq;
  wait_queue_t write_wq;
} file_descriptor_t;

// --- Init y mount ---
void vfs_init(void);

// Monta un FS en `path`. `path` se normaliza internamente.
// Devuelve 0 si OK, -EEXIST si ya hay un mount ahí, -EINVAL si los
// argumentos son inválidos, -ENOMEM si falla la reserva.
int vfs_mount(const char *path, vfs_fs_ops_t *ops, void *fs_priv);

// Desmonta el FS en `path`. Devuelve 0 si OK, -ENOENT si no existe,
// -EBUSY si hay otro mount anidado bajo ese path.
int vfs_umount(const char *path);

// Devuelve el fs_priv asociado al mount en `path`, o NULL si no existe.
// El puntero es válido hasta que se llame a vfs_umount sobre ese mount.
void *vfs_get_mount_priv(const char *path);

// --- Lookup ---
// Devuelve un nodo nuevo (propiedad del llamante, liberar con
// vfs_node_free) o NULL si no existe. El path se normaliza internamente.
vfs_node_t *vfs_lookup(const char *path);

// [PR 4.2] Operaciones de nombre (namespace).
// Estas funciones resuelven el path, localizan el directorio padre y
// delegan en el FS montado. No requieren un proceso (no tocan fds).
//
// Errores típicos:
//   -ENOENT      directorio padre no existe
//   -ENOTDIR     algún componente intermedio no es directorio
//   -EEXIST      ya existe un archivo/dir con ese nombre
//   -EROFS       el FS es read-only
//   -ENAMETOOLONG nombre no cabe (p.ej. >8.3 en FAT32 sin LFN)
//   -EINVAL      path inválido (raíz, "..", etc.)
int vfs_create(const char *path, int flags);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);

// [PR 4.3] Trunca el archivo en `path` a `new_size` bytes.
//   -ENOENT    si no existe
//   -EISDIR    si es un directorio
//   -EROFS     si el FS no soporta truncate
//   -EINVAL    si new_size es mayor que el actual (FAT32)
int vfs_truncate(const char *path, uint64_t new_size);

// [PR 4.4] Lee la entry `index` del directorio en `path`.
//   -ENOTDIR   si no es directorio
//   -EROFS     si el FS no soporta readdir
//   -ENOENT    si el path no existe
int vfs_readdir(const char *path, uint64_t index, vfs_dirent_t *out);

// [PR RENAME] Renombra o mueve `oldpath` a `newpath`.
//   -ENOENT   src no existe
//   -EEXIST   dst ya existe (no soportamos overwrite todavía)
//   -EXDEV    src y dst en FS distintos
//   -EINVAL   path inválido
int vfs_rename(const char *oldpath, const char *newpath);

// Libera un nodo devuelto por vfs_lookup. Llamar a ops->close y kfree.
void vfs_node_free(vfs_node_t *node);

// Lee el archivo entero en un buffer kmalloc'd. El llamante hace kfree.
// Devuelve 0 si OK, negativo en error (incluido -EISDIR).
int vfs_read_all(const char *path, void **out_buf, size_t *out_size);

// [cwd] Resuelve `in` contra `cwd`. Si `in` es absoluto, ignora `cwd`.
// Si es relativo, une `cwd + "/" + in` y normaliza. Resuelve "." y "..".
// Devuelve 0 si OK, negativo si el path es inválido o no cabe.
int vfs_resolve_path(const char *cwd, const char *in, char *out, size_t outlen);

// --- FDs por proceso ---
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