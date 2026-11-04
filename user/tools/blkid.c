// blkid minimo para AuroraOS: tipo, etiqueta y UUID de ext2/3/4, FAT12/16/32,
// exFAT, NTFS, ISO9660 y swap. Uso: blkid [-s TAG] [-o value|full] [-L etiq]
// [-U uuid] [dispositivo...]. Sin argumentos recorre /dev/sd*, nvme*, vd*, loop*.
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct info { char type[16], label[80], uuid[48]; };

static void trim(char *s) {
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\0'))
    s[--n] = 0;
}
static void uuid_fmt(const unsigned char *u, char *out) {
  snprintf(out, 48, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10],
           u[11], u[12], u[13], u[14], u[15]);
}
static int rd(int fd, off_t off, void *buf, size_t n) {
  return pread(fd, buf, n, off) == (ssize_t)n ? 0 : -1;
}
static void cpy(char *dst, size_t dn, const void *src, size_t n) {
  if (n >= dn) n = dn - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
  trim(dst);
}

static int probe(const char *path, struct info *in) {
  memset(in, 0, sizeof(*in));
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  unsigned char b[2048];
  int ok = 0;
  if (rd(fd, 1024, b, 1024) == 0 && b[56] == 0x53 && b[57] == 0xEF) { // ext
    unsigned compat = b[92] | b[93] << 8 | b[94] << 16 | (unsigned)b[95] << 24;
    unsigned incompat = b[96] | b[97] << 8 | b[98] << 16 | (unsigned)b[99] << 24;
    unsigned rocompat = b[100] | b[101] << 8 | b[102] << 16 | (unsigned)b[103] << 24;
    if ((incompat & 0x40) || (incompat & 0x80) || (rocompat & 0x20) || (rocompat & 0x40))
      strcpy(in->type, "ext4");
    else if (compat & 0x4)
      strcpy(in->type, "ext3");
    else
      strcpy(in->type, "ext2");
    cpy(in->label, sizeof(in->label), b + 120, 16);
    uuid_fmt(b + 104, in->uuid);
    ok = 1;
  } else if (rd(fd, 0, b, 512) == 0) {
    if (!memcmp(b + 3, "NTFS    ", 8)) {
      strcpy(in->type, "ntfs");
      snprintf(in->uuid, sizeof(in->uuid), "%02X%02X%02X%02X%02X%02X%02X%02X",
               b[79], b[78], b[77], b[76], b[75], b[74], b[73], b[72]);
      ok = 1;
    } else if (!memcmp(b + 3, "EXFAT   ", 8)) {
      strcpy(in->type, "exfat");
      ok = 1;
    } else if (b[510] == 0x55 && b[511] == 0xAA &&
               (!memcmp(b + 82, "FAT32   ", 8) || !memcmp(b + 54, "FAT16   ", 8) ||
                !memcmp(b + 54, "FAT12   ", 8) || !memcmp(b + 54, "FAT     ", 8))) {
      int f32 = !memcmp(b + 82, "FAT32   ", 8);
      strcpy(in->type, "vfat");
      cpy(in->label, sizeof(in->label), b + (f32 ? 71 : 43), 11);
      const unsigned char *s = b + (f32 ? 67 : 39);
      snprintf(in->uuid, sizeof(in->uuid), "%02X%02X-%02X%02X", s[3], s[2], s[1], s[0]);
      ok = 1;
    }
  }
  if (!ok && rd(fd, 32768, b, 2048) == 0 && !memcmp(b + 1, "CD001", 5)) {
    strcpy(in->type, "iso9660");
    cpy(in->label, sizeof(in->label), b + 40, 32);
    ok = 1;
  }
  if (!ok && rd(fd, 4096 - 10, b, 10) == 0 && !memcmp(b, "SWAPSPACE2", 10)) {
    unsigned char sw[1100];
    strcpy(in->type, "swap");
    if (rd(fd, 1024, sw, 80) == 0) {
      uuid_fmt(sw + 12, in->uuid);
      cpy(in->label, sizeof(in->label), sw + 28, 16);
    }
    ok = 1;
  }
  close(fd);
  return ok ? 0 : 1;
}

static const char *only_tag; static const char *outfmt = "full";
static void show(const char *dev, const struct info *in) {
  const char *tags[3] = {"LABEL", "UUID", "TYPE"};
  const char *vals[3] = {in->label, in->uuid, in->type};
  if (!strcmp(outfmt, "device")) { printf("%s\n", dev); return; }
  int value = !strcmp(outfmt, "value");
  if (!value) printf("%s:", dev);
  int first = 1;
  for (int i = 0; i < 3; i++) {
    if (!vals[i][0] || (only_tag && strcasecmp(only_tag, tags[i]))) continue;
    if (value) printf("%s\n", vals[i]);
    else printf(" %s=\"%s\"", tags[i], vals[i]);
    first = 0;
  }
  (void)first;
  if (!value) printf("\n");
}

int main(int argc, char **argv) {
  const char *want_label = NULL, *want_uuid = NULL;
  int i = 1;
  for (; i < argc && argv[i][0] == '-'; i++) {
    if (!strcmp(argv[i], "-s") && i + 1 < argc) only_tag = argv[++i];
    else if (!strcmp(argv[i], "-o") && i + 1 < argc) outfmt = argv[++i];
    else if (!strcmp(argv[i], "-L") && i + 1 < argc) want_label = argv[++i];
    else if (!strcmp(argv[i], "-U") && i + 1 < argc) want_uuid = argv[++i];
    else if (!strcmp(argv[i], "--")) { i++; break; }
    else { fprintf(stderr, "uso: blkid [-s TAG] [-o value|full|device] [-L etiqueta] [-U uuid] [dispositivo...]\n"); return 4; }
  }
  if (want_label || want_uuid) outfmt = "device";
  char *dflt[256]; int nd = 0;
  glob_t g; memset(&g, 0, sizeof(g));
  if (i >= argc) {
    const char *pats[] = {"/dev/sd*", "/dev/nvme*n*", "/dev/vd*", "/dev/loop*", "/dev/sr*", NULL};
    for (int p = 0; pats[p]; p++) glob(pats[p], p ? GLOB_APPEND : 0, NULL, &g);
    for (size_t k = 0; k < g.gl_pathc && nd < 255; k++) dflt[nd++] = g.gl_pathv[k];
  }
  int found = 0, n = i < argc ? argc - i : nd;
  for (int k = 0; k < n; k++) {
    const char *dev = i < argc ? argv[i + k] : dflt[k];
    struct info in;
    int r = probe(dev, &in);
    if (r != 0) { if (i < argc && r < 0) perror(dev); continue; }
    if (want_label && strcmp(want_label, in.label)) continue;
    if (want_uuid && strcasecmp(want_uuid, in.uuid)) continue;
    show(dev, &in);
    found = 1;
  }
  return found ? 0 : 2;
}
