// user/apps/cred_test/main.c
//
// [U0/U1] Pruebas de credenciales y permisos POSIX (plan Plan_Usuarios.md).
//
// Sin libc ni crt0, con números de syscall Linux x86_64 (como
// syscall_linux_test). Debe ejecutarse como root: crea ficheros de prueba y
// baja privilegios en procesos hijos (fork) para comprobar lo que ve un
// usuario normal. Salida: una línea [cred_test] PASS/FAIL por prueba y una
// final "[cred_test] RESULT pass=N fail=M"; código de salida = nº de fallos.

#include <stdint.h>

#define SYS_open 2
#define SYS_close 3
#define SYS_access 21
#define SYS_nanosleep 35
#define SYS_fork 57
#define SYS_exit 60
#define SYS_wait4 61
#define SYS_kill 62
#define SYS_mkdir 83
#define SYS_unlink 87
#define SYS_rename 82
#define SYS_chmod 90
#define SYS_chown 92
#define SYS_umask 95
#define SYS_getuid 102
#define SYS_getgid 104
#define SYS_geteuid 107
#define SYS_setuid 105
#define SYS_setgid 106
#define SYS_setresuid 117
#define SYS_getresuid 118
#define SYS_setgroups 116
#define SYS_getgroups 115
#define SYS_setfsuid 122
#define SYS_prctl 157
#define SYS_pause 34
#define SYS_write 1
#define SYS_socket 41
#define SYS_bind 49
#define SYS_capget 125
#define SYS_capset 126
#define SYS_exit_group 231

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0100
#define EPERM 1
#define EACCES 13

static inline long sc(long n, long a, long b, long c, long d) {
  long ret;
  register long r10 __asm__("r10") = d;
  register long r8 __asm__("r8") = 0;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
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
  char b[24];
  int i = 0, neg = v < 0;
  unsigned long u = neg ? (unsigned long)-v : (unsigned long)v;
  char t[24];
  int k = 0;
  if (!u)
    t[k++] = '0';
  while (u) {
    t[k++] = '0' + (char)(u % 10);
    u /= 10;
  }
  if (neg)
    b[i++] = '-';
  while (k)
    b[i++] = t[--k];
  b[i] = 0;
  puts_(b);
}

static int g_pass, g_fail;
static void check(const char *name, long got, long want) {
  if (got == want) {
    g_pass++;
    puts_("[cred_test] PASS ");
    puts_(name);
    puts_("\n");
  } else {
    g_fail++;
    puts_("[cred_test] FAIL ");
    puts_(name);
    puts_(" got=");
    putn(got);
    puts_(" want=");
    putn(want);
    puts_("\n");
  }
}

// Ejecuta fn() en un hijo con uid/gid dados (y sin grupos suplementarios) y
// devuelve su código de salida (fn devuelve 0..127) o -1 si el hijo falla.
static int run_as(unsigned uid, unsigned gid, int (*fn)(void *), void *arg) {
  long pid = sc(SYS_fork, 0, 0, 0, 0);
  if (pid < 0)
    return -1;
  if (pid == 0) {
    sc(SYS_setgroups, 0, 0, 0, 0);
    if (sc(SYS_setgid, gid, 0, 0, 0) != 0)
      sc(SYS_exit, 120, 0, 0, 0);
    if (sc(SYS_setuid, uid, 0, 0, 0) != 0)
      sc(SYS_exit, 121, 0, 0, 0);
    int r = fn(arg);
    sc(SYS_exit, r, 0, 0, 0);
  }
  int status = 0;
  sc(SYS_wait4, pid, (long)&status, 0, 0);
  return (status >> 8) & 0xff;
}

// Los hijos devuelven (errno positivo) o 0 si la operación tuvo éxito.
static int err_of(long r) { return r < 0 ? (int)-r : 0; }

static int c_open_ro(void *a) { return err_of(sc(SYS_open, (long)a, O_RDONLY, 0, 0)); }
static int c_open_wo(void *a) { return err_of(sc(SYS_open, (long)a, O_WRONLY, 0, 0)); }
static int c_access_r(void *a) { return err_of(sc(SYS_access, (long)a, 4, 0, 0)); }
static int c_create(void *a) {
  long fd = sc(SYS_open, (long)a, O_CREAT | O_WRONLY, 0644, 0);
  if (fd >= 0)
    sc(SYS_close, fd, 0, 0, 0);
  return err_of(fd);
}

// ---- [U6] capabilities ----
struct cap_hdr { unsigned version; int pid; };
struct cap_dat { unsigned eff, prm, inh; };
static int capbind(unsigned short port) {
  long fd = sc(SYS_socket, 2, 1, 0, 0);
  if (fd < 0)
    return (int)-fd;
  struct { unsigned short fam, port; unsigned addr; char z[8]; } sa;
  char *q = (char *)&sa;
  for (unsigned i = 0; i < sizeof(sa); i++)
    q[i] = 0;
  sa.fam = 2;
  sa.port = (unsigned short)((port >> 8) | (port << 8));
  long r = sc(SYS_bind, fd, (long)&sa, 16, 0);
  sc(SYS_close, fd, 0, 0, 0);
  return err_of(r);
}
static int c_cap_none(void *a) {
  (void)a;
  struct cap_hdr h = {0x20080522u, 0};
  struct cap_dat d[2];
  if (sc(SYS_capget, (long)&h, (long)d, 0, 0) != 0)
    return 1;
  return (d[0].eff | d[0].prm | d[1].eff | d[1].prm) ? 2 : 0;
}
static int c_bind80(void *a) { (void)a; return capbind(80); }
// Conserva permitidas con KEEPCAPS, las recupera con capset y las gasta.
static int c_keepcaps(void *a) {
  (void)a;
  sc(SYS_prctl, 8, 1, 0, 0);
  if (sc(SYS_setuid, 1000, 0, 0, 0) != 0)
    return 100;
  struct cap_hdr h = {0x20080522u, 0};
  struct cap_dat d[2];
  sc(SYS_capget, (long)&h, (long)d, 0, 0);
  if (d[0].eff != 0 || !(d[0].prm & (1u << 10)))
    return 101; // eff debe estar vacio y prm conservado
  if (capbind(80) != EACCES)
    return 102;
  d[0].eff = d[0].prm = (1u << 10); // CAP_NET_BIND_SERVICE
  d[0].inh = 0;
  d[1].eff = d[1].prm = d[1].inh = 0;
  if (sc(SYS_capset, (long)&h, (long)d, 0, 0) != 0)
    return 103;
  if (capbind(80) != 0)
    return 104;
  if (err_of(sc(SYS_chown, (long)"/tmp/ct/own1000", 0, 0, 0)) != EPERM)
    return 105; // sin CAP_CHOWN no hay chown
  if (err_of(sc(SYS_setuid, 0, 0, 0, 0)) != EPERM)
    return 106;
  // no puede ampliar: pedir otra capacidad permitida que ya no tiene
  d[0].eff = d[0].prm = (1u << 10) | (1u << 0);
  if (err_of(sc(SYS_capset, (long)&h, (long)d, 0, 0)) != EPERM)
    return 107;
  return 0;
}
static int c_unlink(void *a) { return err_of(sc(SYS_unlink, (long)a, 0, 0, 0)); }
static int c_setuid_root(void *a) {
  (void)a;
  return err_of(sc(SYS_setuid, 0, 0, 0, 0));
}
static int c_chmod(void *a) { return err_of(sc(SYS_chmod, (long)a, 0777, 0, 0)); }
static int c_chown_self(void *a) {
  // un no-root no puede regalar/robar ficheros
  return err_of(sc(SYS_chown, (long)a, 1000, 1000, 0));
}
static int c_nnp(void *a) {
  (void)a;
  if (sc(SYS_prctl, 39, 0, 0, 0) != 0)
    return 1;
  if (sc(SYS_prctl, 38, 1, 0, 0) != 0)
    return 2;
  if (sc(SYS_prctl, 39, 0, 0, 0) != 1)
    return 3;
  long pid = sc(SYS_fork, 0, 0, 0, 0);
  if (pid == 0) // el hijo hereda el flag
    sc(SYS_exit, sc(SYS_prctl, 39, 0, 0, 0) == 1 ? 0 : 4, 0, 0, 0);
  int st = 0;
  sc(SYS_wait4, pid, (long)&st, 0, 0);
  return (st >> 8) & 0xff;
}

static void touch(const char *p, unsigned mode, unsigned uid, unsigned gid) {
  long fd = sc(SYS_open, (long)p, O_CREAT | O_WRONLY, mode, 0);
  if (fd >= 0)
    sc(SYS_close, fd, 0, 0, 0);
  sc(SYS_chmod, (long)p, mode, 0, 0);
  sc(SYS_chown, (long)p, uid, gid, 0);
}

static void msleep(long ms) {
  long ts[2] = {ms / 1000, (ms % 1000) * 1000000L};
  sc(SYS_nanosleep, (long)ts, 0, 0, 0);
}

static int k_target_pid;
static int c_chgrp_own(void *a) { return err_of(sc(SYS_chown, (long)a, -1, 1000, 0)); }
static int c_chgrp_other(void *a) { return err_of(sc(SYS_chown, (long)a, -1, 2000, 0)); }
static int c_chown_uid_other(void *a) { return err_of(sc(SYS_chown, (long)a, 1001, -1, 0)); }
static int c_tgkill0(void *a) {
  (void)a;
  return err_of(sc(234, k_target_pid, k_target_pid, 0, 0));
}
static unsigned stat_mode(const char *p) {
  unsigned char buf[144];
  if (sc(4, (long)p, (long)buf, 0, 0) < 0)
    return 0xffffffffu;
  return *(unsigned *)(buf + 24);
}
static int c_kill0(void *a) {
  (void)a;
  return err_of(sc(SYS_kill, k_target_pid, 0, 0, 0));
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
  puts_("[cred_test] begin\n");
  if (sc(SYS_getuid, 0, 0, 0, 0) != 0) {
    puts_("[cred_test] debe ejecutarse como root\n");
    sc(SYS_exit_group, 1, 0, 0, 0);
  }

  const char *dir = "/tmp/ct";
  sc(SYS_unlink, (long)"/tmp/ct/priv", 0, 0, 0);
  sc(SYS_unlink, (long)"/tmp/ct/own1000", 0, 0, 0);
  sc(SYS_unlink, (long)"/tmp/ct/newfile", 0, 0, 0);
  sc(SYS_mkdir, (long)dir, 0755, 0, 0);
  sc(SYS_chmod, (long)dir, 0755, 0, 0);

  touch("/tmp/ct/priv", 0600, 0, 0);        // root 0600
  touch("/tmp/ct/own1000", 0600, 1000, 1000); // de uid 1000

  // --- U0: reglas actuales ---
  check("root_ve_fichero_privado", err_of(sc(SYS_open, (long)"/tmp/ct/priv", O_RDONLY, 0, 0)), 0);
  check("uid1000_no_lee_privado_root", run_as(1000, 1000, c_open_ro, "/tmp/ct/priv"), EACCES);
  check("uid1000_access_R_privado_root", run_as(1000, 1000, c_access_r, "/tmp/ct/priv"), EACCES);
  check("uid1000_lee_su_fichero", run_as(1000, 1000, c_open_ro, "/tmp/ct/own1000"), 0);
  check("uid1001_no_lee_fichero_de_1000", run_as(1001, 1001, c_open_ro, "/tmp/ct/own1000"), EACCES);
  check("uid1000_no_crea_en_dir_de_root", run_as(1000, 1000, c_create, "/tmp/ct/newfile"), EACCES);
  check("uid1000_no_borra_en_dir_de_root", run_as(1000, 1000, c_unlink, "/tmp/ct/priv"), EACCES);
  check("noroot_setuid0_eperm", run_as(1000, 1000, c_setuid_root, 0), EPERM);
  check("noroot_chmod_ajeno_eperm", run_as(1000, 1000, c_chmod, "/tmp/ct/priv"), EPERM);
  check("noroot_chown_ajeno_eperm", run_as(1000, 1000, c_chown_self, "/tmp/ct/priv"), EPERM);

  // Acceso por grupo: fichero de root con grupo 1000 y modo 0060.
  touch("/tmp/ct/grp", 0060, 0, 1000);
  check("gid_primario_lee_por_grupo", run_as(1500, 1000, c_open_ro, "/tmp/ct/grp"), 0);
  check("otro_grupo_no_lee", run_as(1500, 1500, c_open_ro, "/tmp/ct/grp"), EACCES);
  sc(SYS_unlink, (long)"/tmp/ct/grp", 0, 0, 0);

  // --- U1.1: fsuid ---
  touch("/tmp/ct/fsu", 0600, 0, 0);
  {
    long old = sc(SYS_setfsuid, 1000, 0, 0, 0); // root baja su fsuid
    check("setfsuid_devuelve_anterior", old, 0);
    check("fsuid_1000_no_abre_privado", err_of(sc(SYS_open, (long)"/tmp/ct/fsu", O_RDONLY, 0, 0)), EACCES);
    sc(SYS_setfsuid, 0, 0, 0, 0);
    check("fsuid_0_restaurado_abre", err_of(sc(SYS_open, (long)"/tmp/ct/fsu", O_RDONLY, 0, 0)), 0);
  }
  // access() usa uid REAL: uid=0, euid=1000 => access ok, open (fsuid) falla.
  {
    long pid = sc(SYS_fork, 0, 0, 0, 0);
    if (pid == 0) {
      sc(SYS_setresuid, 0, 1000, 0, 0);
      int a = err_of(sc(SYS_access, (long)"/tmp/ct/fsu", 4, 0, 0));
      int o = err_of(sc(SYS_open, (long)"/tmp/ct/fsu", O_RDONLY, 0, 0));
      sc(SYS_exit, (a == 0 && o == EACCES) ? 0 : 1, 0, 0, 0);
    }
    int st = 0;
    sc(SYS_wait4, pid, (long)&st, 0, 0);
    check("access_usa_uid_real_open_usa_fsuid", (st >> 8) & 0xff, 0);
  }
  sc(SYS_unlink, (long)"/tmp/ct/fsu", 0, 0, 0);

  // --- U1.2: sticky ---
  sc(SYS_mkdir, (long)"/tmp/ct/st", 01777, 0, 0);
  sc(SYS_chmod, (long)"/tmp/ct/st", 01777, 0, 0);
  check("tmp_es_sticky_1777", run_as(1000, 1000, c_create, "/tmp/ct/st/f1000"), 0);
  check("sticky_otro_no_borra", run_as(1001, 1001, c_unlink, "/tmp/ct/st/f1000"), EPERM);
  check("sticky_dueno_borra", run_as(1000, 1000, c_unlink, "/tmp/ct/st/f1000"), 0);
  run_as(1000, 1000, c_create, "/tmp/ct/st/g1000");
  check("sticky_root_borra", err_of(sc(SYS_unlink, (long)"/tmp/ct/st/g1000", 0, 0, 0)), 0);
  check("tmp_raiz_escribible_por_usuario", run_as(1000, 1000, c_create, "/tmp/ct_user_file"), 0);
  sc(SYS_unlink, (long)"/tmp/ct_user_file", 0, 0, 0);

  // --- U1.3: kill ---
  {
    long a = sc(SYS_fork, 0, 0, 0, 0);
    if (a == 0) {
      sc(SYS_setgroups, 0, 0, 0, 0);
      sc(SYS_setgid, 1001, 0, 0, 0);
      sc(SYS_setuid, 1001, 0, 0, 0);
      for (;;)
        msleep(100);
    }
    msleep(200); // dejar que A baje a uid 1001
    k_target_pid = (int)a;
    check("kill0_otro_uid_eperm", run_as(1000, 1000, c_kill0, 0), EPERM);
    check("kill0_mismo_uid_ok", run_as(1001, 1001, c_kill0, 0), 0);
    check("kill0_root_ok", err_of(sc(SYS_kill, a, 0, 0, 0)), 0);
    check("tgkill0_otro_uid_eperm", run_as(1000, 1000, c_tgkill0, 0), EPERM);
    sc(SYS_kill, a, 9, 0, 0);
    int st = 0;
    sc(SYS_wait4, a, (long)&st, 0, 0);
  }

  // --- U1.5: chown fino ---
  check("dueno_cambia_grupo_a_suyo", run_as(1000, 1000, c_chgrp_own, "/tmp/ct/own1000"), 0);
  check("dueno_no_cambia_a_grupo_ajeno", run_as(1000, 1000, c_chgrp_other, "/tmp/ct/own1000"), EPERM);
  check("dueno_no_regala_fichero", run_as(1000, 1000, c_chown_uid_other, "/tmp/ct/own1000"), EPERM);
  touch("/tmp/ct/suid", 04755, 0, 0);
  sc(SYS_chmod, (long)"/tmp/ct/suid", 04755, 0, 0);
  check("setuid_presente_antes", (long)(stat_mode("/tmp/ct/suid") & 04000), 04000);
  sc(SYS_chown, (long)"/tmp/ct/suid", 1000, -1, 0);
  check("chown_borra_setuid", (long)(stat_mode("/tmp/ct/suid") & 04000), 0);
  sc(SYS_unlink, (long)"/tmp/ct/suid", 0, 0, 0);

  // --- U1.4: no_new_privs ---
  check("prctl_no_new_privs_set_get_herencia", run_as(1000, 1000, c_nnp, 0), 0);

  // --- U6: capabilities ---
  check("root_bind80_ok", capbind(80), 0);
  check("uid1000_sin_caps", run_as(1000, 1000, c_cap_none, 0), 0);
  check("uid1000_bind80_eacces", run_as(1000, 1000, c_bind80, 0), EACCES);
  {
    long kp = sc(SYS_fork, 0, 0, 0, 0);
    if (kp == 0)
      sc(SYS_exit, c_keepcaps(0), 0, 0, 0);
    int kst = 0;
    sc(SYS_wait4, kp, (long)&kst, 0, 0);
    check("keepcaps_capset_bind80", (kst >> 8) & 0xff, 0);
  }

  // limpieza
  sc(SYS_unlink, (long)"/tmp/ct/priv", 0, 0, 0);
  sc(SYS_unlink, (long)"/tmp/ct/own1000", 0, 0, 0);

  puts_("[cred_test] RESULT pass=");
  putn(g_pass);
  puts_(" fail=");
  putn(g_fail);
  puts_("\n");
  sc(SYS_exit_group, g_fail > 120 ? 120 : g_fail, 0, 0, 0);
  for (;;)
    __asm__ volatile("hlt");
}
