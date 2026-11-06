// user/musl/apps/aurora-boot/main.c
//
// aurora-boot: /usr/bin/init del initrd MINIMO del sistema instalado.
// El kernel arranca con ese initrd como "/" (tarfs); este programa busca la
// raiz indicada en la linea de comandos del kernel, la monta, hace pivot_root
// y ejecuta el init real (/usr/bin/init) que vive en el ext2.
//
//   root=/dev/sda2 | root=UUID=<uuid> | root=LABEL=<etiqueta>
//   rootwait=<segundos>   (por defecto 45: los USB tardan en enumerarse)
//
// Tras el pivot los montajes del kernel (/proc, /dev, /sys...) quedan bajo
// /oldroot (vfs_pivot_root); se vuelven a enlazar en su sitio con un bind.
// Sin raiz valida no hay init que ejecutar: se informa por /dev/kmsg y se
// espera (el initrd no lleva shell).

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_pivot_root
#define SYS_pivot_root 155
#endif

static void say(const char *fmt, const char *a) {
  char buf[256];
  snprintf(buf, sizeof(buf), fmt, a ? a : "");
  int fd = open("/dev/kmsg", O_WRONLY);
  if (fd >= 0) {
    write(fd, buf, strlen(buf));
    close(fd);
  }
  fprintf(stderr, "%s\n", buf);
}

static int cmdline_get(const char *key, char *out, size_t cap) {
  char cl[2048];
  int fd = open("/proc/cmdline", O_RDONLY);
  if (fd < 0)
    return 0;
  ssize_t n = read(fd, cl, sizeof(cl) - 1);
  close(fd);
  if (n <= 0)
    return 0;
  cl[n] = 0;
  size_t kl = strlen(key);
  for (char *p = cl; *p;) {
    while (*p == ' ' || *p == '\n')
      p++;
    char *e = p;
    while (*e && *e != ' ' && *e != '\n')
      e++;
    if ((size_t)(e - p) > kl && !strncmp(p, key, kl) && p[kl] == '=') {
      size_t l = (size_t)(e - p) - kl - 1;
      if (l >= cap)
        l = cap - 1;
      memcpy(out, p + kl + 1, l);
      out[l] = 0;
      return 1;
    }
    p = e;
  }
  return 0;
}

// Superbloque ext2: magic 0xEF53 @1080, uuid @1128 (16 B), label @1144 (16 B).
static int read_sb(const char *dev, char uuid[37], char label[17]) {
  unsigned char sb[1024];
  int fd = open(dev, O_RDONLY);
  if (fd < 0)
    return -1;
  ssize_t n = pread(fd, sb, sizeof(sb), 1024);
  close(fd);
  if (n != (ssize_t)sizeof(sb) || sb[56] != 0x53 || sb[57] != 0xEF)
    return -1;
  const unsigned char *u = sb + 104;
  snprintf(uuid, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10],
           u[11], u[12], u[13], u[14], u[15]);
  memcpy(label, sb + 120, 16);
  label[16] = 0;
  return 0;
}

// Devuelve en dev "/dev/<nombre>" del primer ext2 que casa; 0 si lo encuentra.
static int find_root(const char *spec, char *dev, size_t cap) {
  if (!strncmp(spec, "/dev/", 5)) {
    char u[37], l[17];
    if (read_sb(spec, u, l) != 0)
      return -1;
    snprintf(dev, cap, "%s", spec);
    return 0;
  }
  int by_uuid = !strncmp(spec, "UUID=", 5);
  int by_label = !strncmp(spec, "LABEL=", 6);
  if (!by_uuid && !by_label)
    return -1;
  const char *want = spec + (by_uuid ? 5 : 6);
  FILE *f = fopen("/proc/partitions", "r");
  if (!f)
    return -1;
  char line[256];
  int found = -1;
  while (found != 0 && fgets(line, sizeof(line), f)) {
    unsigned ma, mi;
    unsigned long long blocks;
    char name[64];
    if (sscanf(line, "%u %u %llu %63s", &ma, &mi, &blocks, name) != 4)
      continue;
    char path[80], u[37], l[17];
    snprintf(path, sizeof(path), "/dev/%s", name);
    if (read_sb(path, u, l) != 0)
      continue;
    if (by_uuid ? !strcasecmp(u, want) : !strcmp(l, want)) {
      snprintf(dev, cap, "%s", path);
      found = 0;
    }
  }
  fclose(f);
  return found;
}

#define MAXMNT 32
static char g_mnt[MAXMNT][128];
static int g_nmnt;

static void save_mounts(void) {
  FILE *f = fopen("/proc/mounts", "r");
  if (!f)
    return;
  char line[512];
  while (g_nmnt < MAXMNT && fgets(line, sizeof(line), f)) {
    char src[128], tgt[128];
    if (sscanf(line, "%127s %127s", src, tgt) != 2)
      continue;
    if (!strcmp(tgt, "/") || !strcmp(tgt, "/newroot"))
      continue;
    strcpy(g_mnt[g_nmnt++], tgt);
  }
  fclose(f);
}

int main(void) {
  char spec[160] = "", dev[96] = "", wait_s[16] = "";
  int wait = 45;
  if (cmdline_get("rootwait", wait_s, sizeof(wait_s)) && atoi(wait_s) > 0)
    wait = atoi(wait_s);
  if (!cmdline_get("root", spec, sizeof(spec))) {
    say("[aurora-boot] falta root= en la linea de comandos del kernel%s", "");
    goto fail;
  }
  say("[aurora-boot] buscando raiz %s", spec);
  int ok = -1;
  for (int t = 0; t < wait * 2 && ok != 0; t++) {
    ok = find_root(spec, dev, sizeof(dev));
    if (ok != 0)
      usleep(500000);
  }
  if (ok != 0) {
    say("[aurora-boot] no se encontro la raiz %s", spec);
    goto fail;
  }
  mkdir("/newroot", 0755);
  if (mount(dev, "/newroot", "ext2", 0, NULL) != 0) {
    say("[aurora-boot] no se pudo montar %s como ext2", dev);
    goto fail;
  }
  save_mounts();
  mkdir("/newroot/oldroot", 0755);
  if (syscall(SYS_pivot_root, "/newroot", "/newroot/oldroot") != 0) {
    say("[aurora-boot] pivot_root fallo (errno=%s)", strerror(errno));
    goto fail;
  }
  for (int i = 0; i < g_nmnt; i++) {
    char src[160];
    snprintf(src, sizeof(src), "/oldroot%s", g_mnt[i]);
    mkdir(g_mnt[i], 0755);
    mount(src, g_mnt[i], NULL, MS_BIND, NULL);
  }
  say("[aurora-boot] raiz %s montada; ejecutando /usr/bin/init", dev);
  char *argv[] = {"init", NULL};
  char *envp[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/root",
                  "TERM=linux", NULL};
  execve("/usr/bin/init", argv, envp);
  say("[aurora-boot] execve /usr/bin/init fallo (%s)", strerror(errno));
fail:
  for (;;)
    sleep(60);
}
