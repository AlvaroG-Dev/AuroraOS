#ifndef USER_FILE_H
#define USER_FILE_H

#include "../syscall.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

int open(const char *path, int flags);
int close(int fd);
int64_t read(int fd, void *buf, size_t count);
int64_t write(int fd, const void *buf, size_t count);
int64_t lseek(int fd, int64_t offset, int whence);
int fstat(int fd, stat_t *st);

// [PR 4.4] Wrappers de operaciones de namespace.
// dirent_t viene de syscall.h.
int mkdir(const char *path);
int unlink(const char *path);
int readdir(const char *path, uint64_t index, dirent_t *out);

void puts(const char *str);
void printf(const char *fmt, ...);

// [PR A] Crea un fichero vacío. Devuelve 0 si OK, negativo si falla.
int create(const char *path);

// [PR A] Copia src a dst. Devuelve 0 si OK, -1 si falla.
// Reutiliza un buffer de 4 KB en pila; no usa heap.
int copy_file(const char *src, const char *dst);

// [PR RENAME] Renombra o mueve dentro del mismo FS.
// Devuelve 0 si OK, negativo si falla.
int rename_path(const char *oldpath, const char *newpath);

int chdir(const char *path);
int getcwd(char *buf, size_t size);

// [libc] printf-family formateado a buffer.
// Soporta %s %d %i %u %x %X %c %p %% y width con cero a la izquierda
// (%02x, %08x...). Modificadores l, ll, z.
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

// [pipe] Crea un pipe. fds[0]=read, fds[1]=write. 0 si OK.
int pipe(int fds[2]);
int dup2(int oldfd, int newfd);

// ---------------------------------------------------------------------------
// [PTY Fase 4] I/O multiplexado y control de dispositivo.
//
// Estos wrappers van directos a las syscalls correspondientes. Los
// necesita el terminal app para gestionar el master fd de un PTY sin
// bloquearse: poll() para saber cuándo hay datos, ioctl() para obtener
// el número de PTY (TIOCGPTN) y desbloquear el slave (TIOCSPTLCK).
// ---------------------------------------------------------------------------
int poll(struct pollfd *fds, unsigned int nfds, int timeout_ms);
int ioctl(int fd, unsigned long req, void *arg);
int fcntl(int fd, int cmd, int arg);

// Constantes ioctl (Linux x86_64). Layout _IOC: dir|size|type|nr.
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TCFLSH 0x540B
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGSID 0x5429
#define TIOCGPTN 0x80045430   // _IOR('T', 0x30, unsigned int)
#define TIOCSPTLCK 0x40045431 // _IOW('T', 0x31, int)

struct winsize {
  unsigned short ws_row;
  unsigned short ws_col;
  unsigned short ws_xpixel;
  unsigned short ws_ypixel;
};

#endif