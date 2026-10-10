// kernel/sysctl.c
#include "sysctl.h"
#include "klog.h"
#include "net/inet_socket.h"
#include "net/socket.h"
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

// ---------------------------------------------------------------------------
// Parser/emisor compartidos para enteros. El VFS nos pasa el buffer de
// write SIN '\n' final y sin padding, y el buffer de read lo formateamos
// nosotros con '\n' al final (convención Linux sysctl).
// ---------------------------------------------------------------------------
static int parse_u64_simple(const char *buf, size_t len, uint64_t *out) {
  uint64_t v = 0;
  int seen = 0;
  for (size_t i = 0; i < len; i++) {
    char c = buf[i];
    if (c >= '0' && c <= '9') {
      uint64_t nv = v * 10 + (uint64_t)(c - '0');
      if (nv < v)
        return -EINVAL;
      v = nv;
      seen = 1;
    } else if (seen) {
      break;
    }
  }
  if (!seen)
    return -EINVAL;
  *out = v;
  return 0;
}

static int emit_u64(char *buf, size_t cap, size_t *out_len, uint64_t v) {
  if (cap < 24)
    return -EINVAL;
  char tmp[24];
  int n = 0;
  if (v == 0) {
    tmp[n++] = '0';
  } else {
    while (v) {
      tmp[n++] = (char)('0' + (v % 10));
      v /= 10;
    }
  }
  size_t o = 0;
  while (n > 0)
    buf[o++] = tmp[--n];
  buf[o++] = '\n';
  *out_len = o;
  return 0;
}

// ---------------------------------------------------------------------------
// kernel/pid_max
// ---------------------------------------------------------------------------
static uint32_t g_pid_max = 4194304; // Linux default
static int sysctl_pid_max_read(char *buf, size_t cap, size_t *out_len) {
  return emit_u64(buf, cap, out_len, g_pid_max);
}
static int sysctl_pid_max_write(const char *buf, size_t len) {
  uint64_t v;
  if (parse_u64_simple(buf, len, &v) != 0)
    return -EINVAL;
  if (v == 0 || v > 0x7FFFFFFFULL)
    return -EINVAL;
  g_pid_max = (uint32_t)v;
  return 0;
}

// ---------------------------------------------------------------------------
// kernel/threads-max
// ---------------------------------------------------------------------------
static uint32_t g_threads_max = 65536;
static int sysctl_threads_max_read(char *buf, size_t cap, size_t *out_len) {
  return emit_u64(buf, cap, out_len, g_threads_max);
}
static int sysctl_threads_max_write(const char *buf, size_t len) {
  uint64_t v;
  if (parse_u64_simple(buf, len, &v) != 0)
    return -EINVAL;
  if (v == 0 || v > 0x7FFFFFFFULL)
    return -EINVAL;
  g_threads_max = (uint32_t)v;
  return 0;
}

// ---------------------------------------------------------------------------
// kernel/osrelease — read-only. Debe coincidir con uname().
// ---------------------------------------------------------------------------
static int sysctl_osrelease_read(char *buf, size_t cap, size_t *out_len) {
  static const char v[] = "0.1.0\n";
  size_t n = sizeof(v) - 1;
  if (cap < n)
    return -EINVAL;
  memcpy(buf, v, n);
  *out_len = n;
  return 0;
}

// ---------------------------------------------------------------------------
// kernel/random/boot_id — se genera una vez en el primer read.
// ---------------------------------------------------------------------------
static char g_boot_id[37] = {0};
static void gen_uuid_like(char out[37], uint64_t salt) {
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 36; i++) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      out[i] = '-';
      continue;
    }
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t r = ((uint64_t)hi << 32) | lo;
    r ^= salt * 0x9E3779B97F4A7C15ULL;
    r ^= (r << 13);
    r ^= (r >> 7);
    r ^= (r << 17);
    out[i] = hex[r & 0xF];
  }
  out[36] = '\0';
}
static int sysctl_boot_id_read(char *buf, size_t cap, size_t *out_len) {
  if (cap < 37)
    return -EINVAL;
  if (g_boot_id[0] == 0)
    gen_uuid_like(g_boot_id, 0xB007B007CAFEBABEULL);
  memcpy(buf, g_boot_id, 36);
  buf[36] = '\n';
  *out_len = 37;
  return 0;
}

// ---------------------------------------------------------------------------
// kernel/random/uuid — se genera en cada read.
// ---------------------------------------------------------------------------
static int sysctl_uuid_read(char *buf, size_t cap, size_t *out_len) {
  if (cap < 37)
    return -EINVAL;
  char u[37];
  gen_uuid_like(u, 0x5EED5EED5EED5EEDULL);
  memcpy(buf, u, 36);
  buf[36] = '\n';
  *out_len = 37;
  return 0;
}

// ---------------------------------------------------------------------------
// vm/overcommit_memory
// ---------------------------------------------------------------------------
static int32_t g_overcommit_memory = 0;
static int sysctl_overcommit_read(char *buf, size_t cap, size_t *out_len) {
  return emit_u64(buf, cap, out_len, (uint64_t)g_overcommit_memory);
}
static int sysctl_overcommit_write(const char *buf, size_t len) {
  uint64_t v;
  if (parse_u64_simple(buf, len, &v) != 0)
    return -EINVAL;
  if (v > 2)
    return -EINVAL;
  g_overcommit_memory = (int32_t)v;
  return 0;
}

// ---------------------------------------------------------------------------
// vm/max_map_count
// ---------------------------------------------------------------------------
static uint32_t g_max_map_count = 65530; // Linux default
static int sysctl_max_map_count_read(char *buf, size_t cap, size_t *out_len) {
  return emit_u64(buf, cap, out_len, g_max_map_count);
}
static int sysctl_max_map_count_write(const char *buf, size_t len) {
  uint64_t v;
  if (parse_u64_simple(buf, len, &v) != 0)
    return -EINVAL;
  if (v == 0 || v > 0x7FFFFFFFULL)
    return -EINVAL;
  g_max_map_count = (uint32_t)v;
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
    {"kernel/pid_max", 0644, sysctl_pid_max_read, sysctl_pid_max_write},
    {"kernel/threads-max", 0644, sysctl_threads_max_read,
     sysctl_threads_max_write},
    {"kernel/osrelease", 0444, sysctl_osrelease_read, NULL},
    {"kernel/random/uuid", 0444, sysctl_uuid_read, NULL},
    {"kernel/random/boot_id", 0444, sysctl_boot_id_read, NULL},
    {"vm/overcommit_memory", 0644, sysctl_overcommit_read,
     sysctl_overcommit_write},
    {"vm/max_map_count", 0644, sysctl_max_map_count_read,
     sysctl_max_map_count_write},
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