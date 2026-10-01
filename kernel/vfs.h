// kernel/vfs.h
#ifndef VFS_H
#define VFS_H

#include "wait.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Bits independientes. Un nodo tiene EXACTAMENTE uno de estos.
// VFS_CHARDEVICE era 0x03 (== FILE|DIRECTORY), lo que hacía que
// `flags & VFS_DIRECTORY` diera true en un chardevice y rompía
// vfs_readdir/vfs_truncate. Ahora es un bit propio.
#define VFS_FILE 0x01
#define VFS_DIRECTORY 0x02
#define VFS_CHARDEVICE 0x04

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_CREAT 0x0040

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// ---------------------------------------------------------------------------
// [3.4.a] Bits POSIX de modo. Mismos valores que Linux x86_64.
// El tipo (S_IFDIR/S_IFREG/S_IFCHR) va en mode; la abstracción rápida
// de Aurora sigue en flags (VFS_FILE/VFS_DIRECTORY/VFS_CHARDEVICE).
// ---------------------------------------------------------------------------
#define S_IFMT 0170000
#define S_IFSOCK 0140000
#define S_IFLNK 0120000
#define S_IFREG 0100000
#define S_IFBLK 0060000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFIFO 0010000

#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IRWXU 00700
#define S_IRWXG 00070
#define S_IRWXO 00007

// [3.4.d] Bits de acceso para access(2) y vfs_check_access.
// Coinciden con Linux x86_64.
#define VFS_F_OK 0
#define VFS_X_OK 1
#define VFS_W_OK 2
#define VFS_R_OK 4

#define MAX_PROCESS_FDS 32

// Longitud máxima de un path normalizado (coincide con name[] de vfs_node).
#define VFS_PATH_MAX 128

typedef struct vfs_node vfs_node_t;
typedef struct vfs_fs_ops vfs_fs_ops_t;

// ---------------------------------------------------------------------------
// [4.1] statfs: información del FS montado en un path.
// ---------------------------------------------------------------------------
struct vfs_statfs {
  uint64_t f_type;    // magic del FS
  uint64_t f_bsize;   // optimal transfer block size
  uint64_t f_blocks;  // total de bloques
  uint64_t f_bfree;   // bloques libres
  uint64_t f_bavail;  // bloques libres para no-privilegiados
  uint64_t f_files;   // inodos totales
  uint64_t f_ffree;   // inodos libres
  uint64_t f_namelen; // longitud máx de nombre
  uint64_t f_frsize;  // fragment size
};

// ---------------------------------------------------------------------------
// vfs_stat: metadatos de un nodo. mtime_sec en epoch UNIX (0 = sin
// soporte, p.ej. tarfs/devfs/procfs).
// ---------------------------------------------------------------------------
typedef struct vfs_stat {
  uint32_t flags;
  size_t size;
  uint32_t inode;
  int64_t mtime_sec;
  // [3.4.a] Copia directa de node->mode/uid/gid. VFS genérico.
  uint32_t mode;
  uint32_t uid;
  uint32_t gid;
  uint32_t rdev; // [4.4] Nuevo
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

  int (*create)(vfs_node_t *dir, const char *name, uint32_t mode);
  int (*mkdir)(vfs_node_t *dir, const char *name, uint32_t mode);
  int (*unlink)(vfs_node_t *dir, const char *name);

  int (*truncate)(vfs_node_t *node, uint64_t new_size);

  int (*readdir)(vfs_node_t *dir, uint64_t index, vfs_dirent_t *out);
  int (*rename)(vfs_node_t *src_dir, const char *src_name, vfs_node_t *dst_dir,
                const char *dst_name);

  // [3.1] Lee el target de un symlink. Devuelve la longitud copiada
  // (sin NUL) o negativo. Si NULL, el VFS usa node->link_target.
  int (*readlink)(vfs_node_t *node, char *buf, size_t bufsize);

  // [3.1] Crea un symlink `name` -> `target` en el directorio `dir`.
  // Solo FS RW lo implementan. tarfs (RO) lo deja NULL.
  int (*symlink)(vfs_node_t *dir, const char *name, const char *target);

  // NUEVOS:
  // Devuelve una máscara con POLLIN (1), POLLOUT (4), POLLERR (8), etc.
  // Si NULL, el VFS asume (POLLIN|POLLOUT) siempre listo (ficheros regulares).
  int (*poll)(vfs_node_t *node, short events);

  // [PTY fix] Wait queue sobre la que dormir cuando poll() no está listo.
  // Si NULL, el VFS usa la wq del file_descriptor_t (comportamiento
  // antiguo). Los PTYs devuelven &pty->m_read_wq del master (o el
  // read_wq del slave tty) para que pty_slave_emit() despierte al
  // poller directamente.
  wait_queue_t *(*poll_wq)(vfs_node_t *node);

  // req es el request de ioctl; arg es un puntero de USERLAND sin validar.
  // El handler de ioctl debe hacer access_ok/copy_to_user.
  // Si NULL, devuelve -ENOTTY.
  int64_t (*ioctl)(vfs_node_t *node, unsigned long req, uint64_t arg);

  // [4.2] Cambia mtime del nodo. `mtime_sec` en epoch (0 = no soportado).
  // Si NULL, vfs_utimes() devuelve 0 (no-op silencioso como Linux en FS
  // sin timestamps).
  int (*utimes)(vfs_node_t *node, int64_t mtime_sec);

  // [3.4.c] Cambia el modo del nodo. Solo el dueño o root pueden
  // llamar; el VFS ya lo comprueba. FS sin soporte dejan NULL.
  int (*chmod)(vfs_node_t *node, uint32_t mode);

  // [3.4.c] Cambia owner/group. uid/gid = (uint32_t)-1 → no cambiar
  // ese campo. Solo root puede llamar. FS sin soporte dejan NULL.
  int (*chown)(vfs_node_t *node, uint32_t uid, uint32_t gid);
} vfs_ops_t;

struct vfs_node {
  char name[VFS_PATH_MAX];
  uint32_t flags;
  size_t size;
  uint32_t inode;
  vfs_ops_t *ops;
  vfs_fs_ops_t *fs;
  void *priv;

  // [3.1] Symlink: destino tal cual.
  int is_symlink;
  char link_target[VFS_PATH_MAX];

  // [4.2] mtime en epoch UNIX (0 si el FS no lo soporta).
  int64_t mtime_sec;

  // [3.4.a] Modo POSIX completo (S_IF* | permisos | setuid/setgid/sticky).
  // Lo rellena cada FS. Sin fallback: si un FS no lo hace, sale 0.
  uint32_t mode;
  uint32_t uid;
  uint32_t gid;

  // [4.4] Device number para char/block devices. Formato Linux
  // (major<<8 | minor) para majors < 256, o (major<<20 | minor) para
  // majors grandes. Lo lee stat(2) para st_rdev. 0 para ficheros
  // regulares y directorios.
  uint32_t rdev;
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

  // [4.1] Rellena `out` con info del FS. Opcional.
  int (*statfs)(void *fs_priv, struct vfs_statfs *out);
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

// [2.2] pivot_root.
//
// Cambia la raíz del VFS: el mount que está exactamente en `new_root`
// pasa a ser `/`, y todo lo que estaba bajo él se reescribe quitando el
// prefijo. El antiguo `/` (y cualquier otro mount fuera de new_root) se
// mueve bajo `put_old` (que debe estar estrictamente bajo new_root).
//
// Semántica Linux-like, adaptada a la tabla de mounts plana de Aurora:
//   new_root = "/data", put_old = "/data/oldroot":
//     - mount "/data"   -> "/"
//     - mount "/"       -> "/oldroot"
//     - mount "/dev"    -> "/oldroot/dev"
//     - mount "/data/x" -> "/x"
//
// Restricciones:
//   - new_root no puede ser "/".
//   - put_old debe estar estrictamente bajo new_root.
//   - put_old debe existir y ser directorio.
//   - debe existir un mount exactamente en new_root.
//
// Devuelve 0 si OK, negativo en error (-EINVAL/-ENOENT/-ENOTDIR/-ENAMETOOLONG).
int vfs_pivot_root(const char *new_root, const char *put_old);

// [BIND] Monta `source_path` sobre `mount_path`. Ambos deben existir en
// el VFS. mount_path debe ser un directorio (o un punto de montaje
// válido). source_path debe ser un directorio.
//
// Uso típico: vfs_mount_bind("/bin", "/initrd/bin") para exponer el
// árbol de busybox en /bin sin duplicar binarios. Después de esto,
// vfs_lookup("/bin/ls") redirige a "/initrd/bin/ls".
//
// Devuelve 0 si OK, -EEXIST si ya hay algo montado en mount_path,
// -ENOENT si source_path no existe, -ENOTDIR si source_path no es
// directorio, -EINVAL si los paths son inválidos, -ENOMEM.
int vfs_mount_bind(const char *mount_path, const char *source_path);

// Devuelve el fs_priv asociado al mount en `path`, o NULL si no existe.
// El puntero es válido hasta que se llame a vfs_umount sobre ese mount.
void *vfs_get_mount_priv(const char *path);

// --- Lookup ---
// Devuelve un nodo nuevo (propiedad del llamante, liberar con
// vfs_node_free) o NULL si no existe. El path se normaliza internamente.
vfs_node_t *vfs_lookup(const char *path);

// [3.1] Igual que vfs_lookup() pero NO sigue el symlink final. Si el
// path apunta a un symlink, devuelve el nodo del symlink con
// is_symlink=1 y link_target relleno. Se usa para lstat() y readlink().
vfs_node_t *vfs_lookup_nofollow(const char *path);

// [3.1] Lee el target de un symlink en `path`. Escribe hasta bufsize-1
// bytes + NUL. Devuelve la longitud SIN NUL, o -EINVAL si no es
// symlink, -ENOENT si no existe.
int vfs_readlink(const char *path, char *buf, size_t bufsize);

// [3.1] Crea un symlink `linkpath` -> `target`. `target` no se
// resuelve ni se valida. Devuelve -EROFS si el FS no lo soporta.
int vfs_symlink(const char *target, const char *linkpath);

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
int vfs_create(const char *path, uint32_t mode);
int vfs_mkdir(const char *path, uint32_t mode);
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

// [pipe] Crea una pareja de nodos VFS conectados por un pipe.
// Declaración aquí para que syscall.c pueda llamarla sin incluir
// kernel/pipe.h directamente.
int vfs_pipe_create(vfs_node_t **read_end, vfs_node_t **write_end);

// poll del nodo. Si node->ops->poll es NULL, devuelve (POLLIN|POLLOUT) siempre.
int vfs_node_poll(vfs_node_t *node, short events);

// ioctl del nodo. Si NULL, devuelve -ENOTTY.
int64_t vfs_node_ioctl(vfs_node_t *node, unsigned long req, uint64_t arg);

// ---------------------------------------------------------------------------
// [3.3.c] Iteración de mounts + readdir con merge de mounts hijas.
// ---------------------------------------------------------------------------

// Callback para vfs_for_each_mount. Devuelve 0 para continuar,
// != 0 para parar. Los punteros solo son válidos durante la llamada.
typedef int (*vfs_mount_iter_cb_t)(const char *path, const char *fs_name,
                                   int is_bind, const char *bind_source,
                                   void *arg);
void vfs_for_each_mount(vfs_mount_iter_cb_t cb, void *arg);

// [3.3.c] Variante de vfs_readdir que opera sobre un nodo ya resuelto.
// El nodo debe tener node->name = path completo (lo garantiza
// vfs_lookup_rec). Esta variante es la que usa k_getdents64().
//
// A diferencia de vfs_readdir(path, ...), esta mergea las mounts cuyo
// padre es el directorio listado, para que `ls /` vea /dev y /proc
// aunque no existan como entries del FS subyacente.
int vfs_readdir_node(vfs_node_t *node, uint64_t index, vfs_dirent_t *out);

// [4.1] Info del FS montado que cubre `path`.
int vfs_statfs(const char *path, struct vfs_statfs *out);

// [4.2] Aplica mtime a un nodo. Path puede ser symlink (no lo sigue).
int vfs_utimes(const char *path, int64_t mtime_sec);

// [3.4.c] Cambia permisos. Comprueba euid==0 o euid==node->uid.
// Los bits S_ISUID/S_ISGID solo los puede poner root (simplificado).
// Devuelve -EPERM / -EROFS / -ENOENT.
int vfs_chmod(const char *path, uint32_t mode);

// [3.4.c] Cambia owner/group. Solo root. uid o gid = (uint32_t)-1
// deja el campo intacto. Devuelve -EPERM / -EROFS / -ENOENT.
int vfs_chown(const char *path, uint32_t uid, uint32_t gid);

// [3.4.c] Variantes sobre un fd abierto (fchmod/fchown).
int vfs_fchmod(file_descriptor_t *fd, uint32_t mode);
int vfs_fchown(file_descriptor_t *fd, uint32_t uid, uint32_t gid);

// [3.4.d] Comprueba si el proceso actual puede acceder al nodo con los
// bits pedidos. mask = combinación de VFS_R_OK|W_OK|X_OK (o VFS_F_OK).
//
// Devuelve 0 si permitido, -EACCES si no, -EINVAL si node==NULL.
// Root (euid==0) bypasea; el bit X de ficheros regulares se comprueba
// aparte en execve (Linux hace lo mismo: root no ejecuta sin ningún x).
int vfs_check_access(vfs_node_t *node, int mask);

#endif