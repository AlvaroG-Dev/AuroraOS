// kernel/sysctl.h
#ifndef SYSCTL_H
#define SYSCTL_H

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// sysctl: /proc/sys/*.
//
// Estructura plana de entradas (path + handler). Cada entrada puede ser:
//   - read-only: solo read()
//   - read-write: read() + write()
//
// El path es relativo a /proc/sys/ (p. ej. "kernel/hostname").
//
// El buffer de read() se copia tal cual (el handler ya formatea con '\n').
// El buffer de write() viene SIN '\n' final y sin padding.
// ---------------------------------------------------------------------------

typedef struct sysctl_entry {
  const char *path; // "kernel/hostname", "vm/swap_low_pct"
  uint32_t mode;    // 0444 o 0644
  int (*read)(char *buf, size_t cap, size_t *out_len);
  int (*write)(const char *buf, size_t len);
} sysctl_entry_t;

void sysctl_init(void);
const sysctl_entry_t *sysctl_lookup(const char *path_after_sys);
int sysctl_count(void);
const sysctl_entry_t *sysctl_get(int index);

#endif