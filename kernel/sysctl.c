// kernel/sysctl.c
#include "sysctl.h"
#include "klog.h"
#include "string.h"
#include "swap.h"
#include "uaccess.h"
#include <stddef.h>


// ---------------------------------------------------------------------------
// Handlers concretos
// ---------------------------------------------------------------------------

// kernel/hostname — usa g_hostname de syscall.c (extern).
extern char g_hostname[64];
static int sysctl_hostname_read(char *buf, size_t cap, size_t *out_len) {
  size_t n = strlen(g_hostname);
  if (n + 1 > cap)
    n = cap - 1;
  memcpy(buf, g_hostname, n);
  buf[n] = '\n';
  *out_len = n + 1;
  return 0;
}
static int sysctl_hostname_write(const char *buf, size_t len) {
  if (len == 0 || len >= sizeof(((char[64]){0})))
    return -EINVAL;
  if (len >= 64)
    return -EINVAL;
  memcpy(g_hostname, buf, len);
  g_hostname[len] = '\0';
  return 0;
}

// kernel/printk — formato Linux-like: "N\n". Acepta varios números; usamos
// el primero como nivel mínimo de log (0=panic .. 5=trace).
static int sysctl_printk_read(char *buf, size_t cap, size_t *out_len) {
  int lvl = (int)klog_get_level();
  int n = 0;
  if (cap < 4)
    return -EINVAL;
  if (lvl >= 10)
    buf[n++] = '0' + (lvl / 10);
  buf[n++] = '0' + (lvl % 10);
  buf[n++] = '\n';
  *out_len = n;
  return 0;
}
static int sysctl_printk_write(const char *buf, size_t len) {
  int v = 0, seen = 0;
  for (size_t i = 0; i < len; i++) {
    char c = buf[i];
    if (c >= '0' && c <= '9') {
      v = v * 10 + (c - '0');
      seen = 1;
      if (v > 5)
        v = 5;
    } else if (seen) {
      break;
    }
  }
  if (!seen)
    return -EINVAL;
  klog_set_level((klog_level_t)v);
  LOG_INFO("[SYSCTL] printk = %d", v);
  return 0;
}

// vm/swap_low_pct — porcentaje del total (0..90).
static int sysctl_swap_low_read(char *buf, size_t cap, size_t *out_len) {
  uint32_t v = swap_get_low_pct();
  int n = 0;
  if (cap < 4)
    return -EINVAL;
  if (v >= 10)
    buf[n++] = '0' + (v / 10);
  buf[n++] = '0' + (v % 10);
  buf[n++] = '\n';
  *out_len = n;
  return 0;
}
static int sysctl_swap_low_write(const char *buf, size_t len) {
  int v = 0, seen = 0;
  for (size_t i = 0; i < len; i++) {
    char c = buf[i];
    if (c >= '0' && c <= '9') {
      v = v * 10 + (c - '0');
      seen = 1;
      if (v > 90)
        v = 90;
    } else if (seen)
      break;
  }
  if (!seen)
    return -EINVAL;
  swap_set_low_pct((uint32_t)v);
  LOG_INFO("[SYSCTL] vm.swap_low_pct = %d", v);
  return 0;
}

static int sysctl_swap_high_read(char *buf, size_t cap, size_t *out_len) {
  uint32_t v = swap_get_high_pct();
  int n = 0;
  if (cap < 4)
    return -EINVAL;
  if (v >= 10)
    buf[n++] = '0' + (v / 10);
  buf[n++] = '0' + (v % 10);
  buf[n++] = '\n';
  *out_len = n;
  return 0;
}
static int sysctl_swap_high_write(const char *buf, size_t len) {
  int v = 0, seen = 0;
  for (size_t i = 0; i < len; i++) {
    char c = buf[i];
    if (c >= '0' && c <= '9') {
      v = v * 10 + (c - '0');
      seen = 1;
      if (v > 95)
        v = 95;
    } else if (seen)
      break;
  }
  if (!seen)
    return -EINVAL;
  swap_set_high_pct((uint32_t)v);
  LOG_INFO("[SYSCTL] vm.swap_high_pct = %d", v);
  return 0;
}

// ---------------------------------------------------------------------------
// Tabla
// ---------------------------------------------------------------------------
static const sysctl_entry_t g_entries[] = {
    {"kernel/hostname", 0644, sysctl_hostname_read, sysctl_hostname_write},
    {"kernel/printk", 0644, sysctl_printk_read, sysctl_printk_write},
    {"vm/swap_low_pct", 0644, sysctl_swap_low_read, sysctl_swap_low_write},
    {"vm/swap_high_pct", 0644, sysctl_swap_high_read, sysctl_swap_high_write},
};
#define SYSCTL_N (sizeof(g_entries) / sizeof(g_entries[0]))

void sysctl_init(void) {
  LOG_INFO("[SYSCTL] %u entradas registradas", (unsigned)SYSCTL_N);
}

const sysctl_entry_t *sysctl_lookup(const char *path_after_sys) {
  if (!path_after_sys)
    return NULL;
  for (size_t i = 0; i < SYSCTL_N; i++) {
    if (strcmp(g_entries[i].path, path_after_sys) == 0)
      return &g_entries[i];
  }
  return NULL;
}

int sysctl_count(void) { return (int)SYSCTL_N; }
const sysctl_entry_t *sysctl_get(int index) {
  if (index < 0 || index >= (int)SYSCTL_N)
    return NULL;
  return &g_entries[index];
}