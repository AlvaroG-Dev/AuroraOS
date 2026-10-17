/* bootloader/bios/config.c
 *
 * Parser del aurora.conf en 32-bit PM. Mismo formato que el de UEFI.
 * Los paths se normalizan a la forma que el reader FAT16 entiende:
 *   - '/' como separador
 *   - mayúsculas
 *   - sin prefijo si el fichero no lo lleva
 */
#include "bios.h"

/* ---------- helpers internos ---------- */

static void cfg_trim(const char **s, uint32_t *len) {
  while (*len > 0 && ((*s)[0] == ' ' || (*s)[0] == '\t')) {
    (*s)++;
    (*len)--;
  }
  while (*len > 0 && ((*s)[*len - 1] == ' ' || (*s)[*len - 1] == '\t' ||
                      (*s)[*len - 1] == '\r')) {
    (*len)--;
  }
}

static void cfg_normalize_path(const char *in, uint32_t in_len, char *out,
                               uint32_t out_max) {
  uint32_t o = 0;
  if (in_len == 0 || (in[0] != '/' && in[0] != '\\')) {
    if (o + 1 < out_max)
      out[o++] = '/';
  }
  for (uint32_t i = 0; i < in_len && o + 1 < out_max; i++) {
    char c = in[i];
    if (c == '\\')
      c = '/';
    if (c >= 'a' && c <= 'z')
      c -= 32;
    out[o++] = c;
  }
  out[o] = 0;
}

static int cfg_key_is(const char *line, uint32_t len, const char *want) {
  uint32_t want_len = str_len(want);
  uint32_t eq = 0;
  while (eq < len && line[eq] != '=')
    eq++;
  if (eq >= len)
    return 0;
  uint32_t ke = eq;
  while (ke > 0 && (line[ke - 1] == ' ' || line[ke - 1] == '\t'))
    ke--;
  if (ke != want_len)
    return 0;
  return str_eq_n(line, want, want_len);
}

static void cfg_extract_value(const char *line, uint32_t len, const char **out,
                              uint32_t *out_len) {
  uint32_t eq = 0;
  while (eq < len && line[eq] != '=')
    eq++;
  if (eq >= len) {
    *out = line + len;
    *out_len = 0;
    return;
  }
  const char *v = line + eq + 1;
  uint32_t vlen = len - eq - 1;
  cfg_trim(&v, &vlen);
  *out = v;
  *out_len = vlen;
}

static void cfg_apply(bios_config_t *cfg, const char *line, uint32_t len) {
  if (cfg_key_is(line, len, "kernel")) {
    const char *v;
    uint32_t vl;
    cfg_extract_value(line, len, &v, &vl);
    if (vl > 0) {
      cfg_normalize_path(v, vl, cfg->kernel_path, sizeof(cfg->kernel_path));
      cfg->have_kernel = 1;
    }
  } else if (cfg_key_is(line, len, "initrd")) {
    const char *v;
    uint32_t vl;
    cfg_extract_value(line, len, &v, &vl);
    if (vl > 0) {
      cfg_normalize_path(v, vl, cfg->initrd_path, sizeof(cfg->initrd_path));
      cfg->have_initrd = 1;
    }
  } else if (cfg_key_is(line, len, "cmdline")) {
    const char *v;
    uint32_t vl;
    cfg_extract_value(line, len, &v, &vl);
    if (vl > 0) {
      if (vl >= sizeof(cfg->cmdline))
        vl = sizeof(cfg->cmdline) - 1;
      copy_mem(cfg->cmdline, v, vl);
      cfg->cmdline[vl] = 0;
      cfg->have_cmdline = 1;
    }
  } else if (cfg_key_is(line, len, "timeout")) {
    const char *v;
    uint32_t vl;
    cfg_extract_value(line, len, &v, &vl);
    int t = 0;
    for (uint32_t i = 0; i < vl && v[i] >= '0' && v[i] <= '9'; i++)
      t = t * 10 + (v[i] - '0');
    cfg->timeout = t;
  }
}

static int cfg_try_file(uint8_t drive, const char *path, bios_config_t *cfg) {
  fat_file_t f;
  if (fat16_find(path, &f) != 0)
    return -1;
  if (f.size == 0 || f.size > 4096)
    return -1;

  static char buf[4096];
  if (fat16_read(drive, &f, 0, f.size, buf) != (int)f.size)
    return -1;

  uint32_t i = 0;
  while (i < f.size) {
    uint32_t line_start = i;
    while (i < f.size && buf[i] != '\n')
      i++;
    uint32_t line_end = i;
    if (i < f.size)
      i++;

    const char *p = buf + line_start;
    uint32_t len = line_end - line_start;
    cfg_trim(&p, &len);
    if (len == 0 || p[0] == '#')
      continue;

    cfg_apply(cfg, p, len);
  }
  return 0;
}

int bios_config_load(uint8_t drive, const void *unused, bios_config_t *out) {
  (void)unused;

  zero_mem(out, sizeof(*out));
  str_copy(out->kernel_path, "/AURORA/KERNEL.ELF", sizeof(out->kernel_path));
  str_copy(out->initrd_path, "/AURORA/INITRD.TAR", sizeof(out->initrd_path));
  out->timeout = 3;

  static const char *candidates[] = {
      "/AURORA/AURORA.CONF",
      "/AURORA/AURORA.CONF",
      "/AURORA/AURORA.CONF",
  };

  for (int i = 0; i < 3; i++) {
    if (cfg_try_file(drive, candidates[i], out) == 0) {
      bios_log("[CFG] aurora.conf cargado (kernel=%s)\n",
               out->have_kernel ? out->kernel_path : "(default)");
      return 0;
    }
  }
  bios_log("[CFG] aurora.conf no encontrado, usando defaults\n");
  return 0;
}