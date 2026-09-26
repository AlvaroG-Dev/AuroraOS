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

#endif