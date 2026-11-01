// user/apps/meta_test/main.c
//
// [U2] Persistencia de modo/uid/gid en /data (FAT32). Se ejecuta DOS veces
// (con un reinicio entre medias): si /data/mt/f no existe crea ficheros con
// propietarios y hace sync(); si existe, comprueba que todo sobrevivió.
// Sin libc, números de syscall Linux x86_64. Salida: "[meta_test] RESULT ...".

#include <stdint.h>

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_stat 4
#define SYS_fork 57
#define SYS_exit 60
#define SYS_wait4 61
#define SYS_rename 82
#define SYS_mkdir 83
#define SYS_unlink 87
#define SYS_chmod 90
#define SYS_chown 92
#define SYS_setuid 105
#define SYS_setgid 106
#define SYS_setgroups 116
#define SYS_sync 162
#define SYS_exit_group 231
#define O_WRONLY 1
#define O_CREAT 0100

static inline long sc(long n, long a, long b, long c, long d) {
  long ret;
  register long r10 __asm__("r10") = d;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                   : "rcx", "r11", "memory");
  return ret;
}
static unsigned long slen(const char *s) {
  unsigned long n = 0;
  while (s[n])
    n++;
  return n;
}
static void puts_(const char *s) { sc(SYS_write, 1, (long)s, (long)slen(s), 0); }
static void putn(long v) {
  char b[24], t[24];
  int i = 0, k = 0;
  unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
  if (v < 0)
    b[i++] = '-';
  if (!u)
    t[k++] = '0';
  while (u) {
    t[k++] = '0' + (char)(u % 10);
    u /= 10;
  }
  while (k)
    b[i++] = t[--k];
  b[i] = 0;
  puts_(b);
}
static int g_pass, g_fail;
static void check(const char *name, long got, long want) {
  if (got == want) {
    g_pass++;
    puts_("[meta_test] PASS ");
    puts_(name);
    puts_("\n");
  } else {
    g_fail++;
    puts_("[meta_test] FAIL ");
    puts_(name);
    puts_(" got=");
    putn(got);
    puts_(" want=");
    putn(want);
    puts_("\n");
  }
}
static int touch(const char *p) {
  long fd = sc(SYS_open, (long)p, O_CREAT | O_WRONLY, 0644, 0);
  if (fd < 0)
    return (int)fd;
  sc(SYS_close, fd, 0, 0, 0);
  return 0;
}
// mode(12 bits) | uid<<16... se devuelven por separado
static int st(const char *p, unsigned *mode, unsigned *uid, unsigned *gid) {
  unsigned char buf[144];
  long r = sc(SYS_stat, (long)p, (long)buf, 0, 0);
  if (r < 0)
    return (int)r;
  *mode = *(unsigned *)(buf + 24) & 07777;
  *uid = *(unsigned *)(buf + 28);
  *gid = *(unsigned *)(buf + 32);
  return 0;
}
static void expect(const char *name, const char *p, unsigned m, unsigned u,
                   unsigned g) {
  unsigned am = 0, au = 0, ag = 0;
  int r = st(p, &am, &au, &ag);
  check(name, r == 0 && am == m && au == u && ag == g ? 1 : 0, 1);
  if (r != 0 || am != m || au != u || ag != g) {
    puts_("   got mode=");
    putn(am);
    puts_(" uid=");
    putn(au);
    puts_(" gid=");
    putn(ag);
    puts_(" r=");
    putn(r);
    puts_("\n");
  }
}

#define O_APPEND 02000
#define O_RDONLY 0
static const char k_line[] = "mtuser:x:2000:2000::/:/bin/false\n";
static int etc_has_line(void) {
  long fd = sc(SYS_open, (long)"/etc/passwd", O_RDONLY, 0, 0);
  if (fd < 0)
    return 0;
  static char buf[8192];
  long n = sc(SYS_read, fd, (long)buf, sizeof(buf) - 1, 0);
  sc(SYS_close, fd, 0, 0, 0);
  if (n <= 0)
    return 0;
  buf[n] = 0;
  for (long i = 0; i + 6 < n; i++)
    if (buf[i] == 'm' && buf[i + 1] == 't' && buf[i + 2] == 'u' &&
        buf[i + 3] == 's' && buf[i + 4] == 'e' && buf[i + 5] == 'r')
      return 1;
  return 0;
}
static int etc_remove_line(void) {
  long fd = sc(SYS_open, (long)"/etc/passwd", O_RDONLY, 0, 0);
  if (fd < 0)
    return -1;
  static char buf[8192], out[8192];
  long n = sc(SYS_read, fd, (long)buf, sizeof(buf) - 1, 0);
  sc(SYS_close, fd, 0, 0, 0);
  long o = 0, i = 0;
  while (i < n) {
    long j = i;
    while (j < n && buf[j] != '\n')
      j++;
    int mt = (j - i >= 6 && buf[i] == 'm' && buf[i + 1] == 't' && buf[i + 2] == 'u' &&
              buf[i + 3] == 's' && buf[i + 4] == 'e' && buf[i + 5] == 'r');
    if (!mt)
      for (long k = i; k <= j && k < n; k++)
        out[o++] = buf[k];
    i = j + 1;
  }
  fd = sc(SYS_open, (long)"/etc/passwd", O_WRONLY | 01000 /*O_TRUNC*/, 0, 0);
  if (fd < 0)
    return -1;
  sc(SYS_write, fd, (long)out, o, 0);
  sc(SYS_close, fd, 0, 0, 0);
  return 0;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
  unsigned m, u, g;
  if (st("/data", &m, &u, &g) != 0) {
    puts_("[meta_test] SKIP /data no montado\n");
    sc(SYS_exit_group, 0, 0, 0, 0);
  }
  if (st("/data/mt/f", &m, &u, &g) != 0) {
    puts_("[meta_test] PHASE1 crear\n");
    sc(SYS_mkdir, (long)"/data/mt", 0755, 0, 0);
    touch("/data/mt/f");
    sc(SYS_chmod, (long)"/data/mt/f", 0640, 0, 0);
    sc(SYS_chown, (long)"/data/mt/f", 1000, 1001, 0);
    sc(SYS_mkdir, (long)"/data/mt/d", 0755, 0, 0);
    sc(SYS_chmod, (long)"/data/mt/d", 0750, 0, 0);
    sc(SYS_chown, (long)"/data/mt/d", 1000, 1000, 0);
    sc(SYS_chmod, (long)"/data/mt", 0777, 0, 0); // que uid 1000 cree dentro
    long pid = sc(SYS_fork, 0, 0, 0, 0);
    if (pid == 0) {
      sc(SYS_setgroups, 0, 0, 0, 0);
      sc(SYS_setgid, 1000, 0, 0, 0);
      sc(SYS_setuid, 1000, 0, 0, 0);
      sc(SYS_exit, touch("/data/mt/own") == 0 ? 0 : 1, 0, 0, 0);
    }
    int stt = 0;
    sc(SYS_wait4, pid, (long)&stt, 0, 0);
    check("hijo_uid1000_crea_en_data", (stt >> 8) & 0xff, 0);
    expect("fase1_f", "/data/mt/f", 0640, 1000, 1001);
    expect("fase1_own_dueno_creador", "/data/mt/own", 0644 & ~022u, 1000, 1000);
    {
      long fd = sc(SYS_open, (long)"/etc/passwd", O_WRONLY | O_APPEND, 0, 0);
      check("etc_passwd_append", fd >= 0 ? 0 : fd, 0);
      if (fd >= 0) {
        sc(SYS_write, fd, (long)k_line, sizeof(k_line) - 1, 0);
        sc(SYS_close, fd, 0, 0, 0);
      }
    }
    sc(SYS_sync, 0, 0, 0, 0);
    puts_("[meta_test] PHASE1 hecha, reiniciar y volver a ejecutar\n");
  } else {
    puts_("[meta_test] PHASE2 comprobar tras reinicio\n");
    check("etc_passwd_persiste", etc_has_line(), 1);
    etc_remove_line();
    expect("etc_shadow_0600", "/etc/shadow", 0600, 0, 0);
    expect("fase2_f", "/data/mt/f", 0640, 1000, 1001);
    expect("fase2_d", "/data/mt/d", 0750, 1000, 1000);
    expect("fase2_own", "/data/mt/own", 0644, 1000, 1000);
    check("rename_f_g", sc(SYS_rename, (long)"/data/mt/f", (long)"/data/mt/g", 0, 0), 0);
    expect("rename_conserva_meta", "/data/mt/g", 0640, 1000, 1001);
    sc(SYS_unlink, (long)"/data/mt/g", 0, 0, 0);
    sc(SYS_unlink, (long)"/data/mt/own", 0, 0, 0);
    sc(SYS_unlink, (long)"/data/mt/d", 0, 0, 0);
    sc(SYS_sync, 0, 0, 0, 0);
  }
  puts_("[meta_test] RESULT pass=");
  putn(g_pass);
  puts_(" fail=");
  putn(g_fail);
  puts_("\n");
  sc(SYS_exit_group, g_fail, 0, 0, 0);
  for (;;)
    __asm__ volatile("hlt");
}
