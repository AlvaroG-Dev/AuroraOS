// user/aurora-users/aurora-users.c
//
// Herramientas de cuentas de AuroraOS sobre glibc (sin PAM, sin busybox):
//   aurora-login  aurora-su  aurora-passwd  aurora-useradd  aurora-sudo
// Un único binario "multicall": se comporta según basename(argv[0]).
// Usa getpwnam/getspnam/crypt de glibc y los ficheros /etc/{passwd,group,shadow}.
// su/passwd/sudo/login se instalan setuid-root (modo 4755) en la ISO.
// Ver Plan_Usuarios.md (U3).

#define _GNU_SOURCE
#ifdef AU_NO_CRYPT_H
/* Sin libcrypt-dev en el host: declaración propia (se enlaza libcrypt.so.1). */
extern char *crypt(const char *key, const char *salt);
#else
#include <crypt.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <libgen.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define PASSWD_F "/etc/passwd"
#define GROUP_F "/etc/group"
#define SHADOW_F "/etc/shadow"
#define SKEL_D "/etc/skel"
#define DEFAULT_PATH "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
#include <stdarg.h>
static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

// Rastro en el log del kernel (/dev/kmsg): diagnóstico de sesiones sin consola.
static void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void klog(const char *fmt, ...) {
  char b[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  int fd = open("/dev/kmsg", O_WRONLY);
  if (fd >= 0) {
    ssize_t r = write(fd, b, strlen(b));
    (void)r;
    close(fd);
  }
}

// ---- contraseñas ----------------------------------------------------------

static int read_secret(const char *prompt, char *buf, size_t cap) {
  struct termios t, o;
  int tty = isatty(0);
  fputs(prompt, stderr);
  fflush(stderr);
  if (tty && tcgetattr(0, &o) == 0) {
    t = o;
    t.c_lflag &= ~(tcflag_t)ECHO;
    tcsetattr(0, TCSANOW, &t);
  } else {
    tty = 0;
  }
  char *r = fgets(buf, (int)cap, stdin);
  if (tty) {
    tcsetattr(0, TCSANOW, &o);
    fputc('\n', stderr);
  }
  if (!r)
    return -1;
  size_t n = strlen(buf);
  while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    buf[--n] = 0;
  return 0;
}

static const char *stored_hash(const struct passwd *pw) {
  struct spwd *sp = getspnam(pw->pw_name);
  if (sp && sp->sp_pwdp)
    return sp->sp_pwdp;
  return pw->pw_passwd; // normalmente "x" si no hay shadow
}

// 0 = correcta. Cuentas bloqueadas ("*", "!...", "x") nunca validan.
static int check_password(const struct passwd *pw, const char *plain) {
  const char *h = stored_hash(pw);
  if (!h || !*h || h[0] == '*' || h[0] == '!' || strcmp(h, "x") == 0)
    return -1;
  char *c = crypt(plain, h);
  if (!c)
    return -1;
  return strcmp(c, h) == 0 ? 0 : -1;
}

static int account_locked(const struct passwd *pw) {
  const char *h = stored_hash(pw);
  return !h || !*h || h[0] == '*' || h[0] == '!' || strcmp(h, "x") == 0;
}

// ---- cambio de identidad --------------------------------------------------

static void become(const struct passwd *pw, int login_env) {
  klog("[aurora-users] become %s (uid %u) euid=%u", pw->pw_name, pw->pw_uid, geteuid());
  if (initgroups(pw->pw_name, pw->pw_gid) != 0 && geteuid() == 0)
    die("initgroups: %s", strerror(errno));
  if (setgid(pw->pw_gid) != 0)
    die("setgid: %s", strerror(errno));
  if (setuid(pw->pw_uid) != 0)
    die("setuid: %s", strerror(errno));
  const char *shell = pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/sh";
  // Se conserva el entorno de la sesión (WAYLAND_DISPLAY, XDG_*, TERM...):
  // la sesión gráfica por usuario llega en U5. Solo se fija la identidad.
  if (login_env && !getenv("PATH"))
    setenv("PATH", DEFAULT_PATH, 1);
  setenv("HOME", pw->pw_dir, 1);
  setenv("USER", pw->pw_name, 1);
  setenv("LOGNAME", pw->pw_name, 1);
  setenv("SHELL", shell, 1);
  klog("[aurora-users] identidad cambiada a %s uid=%u", pw->pw_name, getuid());
}

static void exec_shell(const struct passwd *pw, int login, const char *cmd) {
  const char *shell = pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/sh";
  if (login && chdir(pw->pw_dir) != 0) {
    fprintf(stderr, "aviso: sin directorio %s, usando /\n", pw->pw_dir);
    if (chdir("/") != 0) {
    }
  }
  char arg0[128];
  const char *base = strrchr(shell, '/');
  base = base ? base + 1 : shell;
  snprintf(arg0, sizeof(arg0), "%s%s", login ? "-" : "", base);
  if (cmd)
    execl(shell, arg0, "-c", cmd, (char *)NULL);
  else
    execl(shell, arg0, (char *)NULL);
  die("exec %s: %s", shell, strerror(errno));
}

// ---- login ----------------------------------------------------------------

static int cmd_login(int argc, char **argv) {
  const char *force = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
      force = argv[++i];
  }
  klog("[aurora-users] login uid=%u euid=%u force=%s", getuid(), geteuid(), force ? force : "-");
  if (force && geteuid() != 0)
    die("login: -f requiere privilegios de root");
  for (int tries = 0; tries < 5; tries++) {
    char user[64], pass[256];
    struct passwd *pw = NULL;
    if (force) {
      pw = getpwnam(force);
      if (!pw)
        die("login: usuario '%s' no existe", force);
    } else {
      fputs("aurora login: ", stderr);
      fflush(stderr);
      if (!fgets(user, sizeof(user), stdin))
        return 1;
      size_t n = strlen(user);
      while (n && (user[n - 1] == '\n' || user[n - 1] == '\r'))
        user[--n] = 0;
      if (!n)
        continue;
      if (read_secret("Password: ", pass, sizeof(pass)) != 0)
        return 1;
      pw = getpwnam(user);
      if (!pw || check_password(pw, pass) != 0) {
        memset(pass, 0, sizeof(pass));
        sleep(2);
        fputs("Login incorrect\n", stderr);
        continue;
      }
      memset(pass, 0, sizeof(pass));
    }
    become(pw, 1);
    exec_shell(pw, 1, NULL);
  }
  return 1;
}

// ---- su -------------------------------------------------------------------

static const struct passwd *caller_pw(void) { return getpwuid(getuid()); }

static int cmd_su(int argc, char **argv) {
  int login = 0;
  const char *target = "root", *cmd = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-") == 0 || strcmp(argv[i], "-l") == 0)
      login = 1;
    else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
      cmd = argv[++i];
    else if (argv[i][0] != '-')
      target = argv[i];
    else
      die("uso: su [-] [-c orden] [usuario]");
  }
  struct passwd *pw = getpwnam(target);
  if (!pw)
    die("su: usuario '%s' no existe", target);
  if (getuid() != 0) {
    char pass[256];
    if (read_secret("Password: ", pass, sizeof(pass)) != 0)
      return 1;
    int bad = check_password(pw, pass);
    memset(pass, 0, sizeof(pass));
    if (bad) {
      sleep(2);
      die("su: autenticacion fallida");
    }
  }
  become(pw, login);
  exec_shell(pw, login, cmd);
  return 1;
}

// ---- ficheros de cuentas (reescritura atómica) -----------------------------

static char *slurp(const char *path, size_t *len) {
  FILE *f = fopen(path, "r");
  if (!f) {
    *len = 0;
    return strdup("");
  }
  size_t cap = 4096, n = 0;
  char *b = malloc(cap);
  size_t r;
  while ((r = fread(b + n, 1, cap - n - 1, f)) > 0) {
    n += r;
    if (n + 1 >= cap) {
      cap *= 2;
      b = realloc(b, cap);
    }
  }
  fclose(f);
  b[n] = 0;
  *len = n;
  return b;
}

static int write_atomic(const char *path, const char *data, size_t len,
                        mode_t mode) {
  char tmp[256];
  snprintf(tmp, sizeof(tmp), "%s.new", path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0)
    return -1;
  size_t off = 0;
  while (off < len) {
    ssize_t w = write(fd, data + off, len - off);
    if (w <= 0) {
      close(fd);
      unlink(tmp);
      return -1;
    }
    off += (size_t)w;
  }
  fchmod(fd, mode);
  fsync(fd);
  close(fd);
  if (rename(tmp, path) != 0) {
    unlink(tmp);
    return -1;
  }
  return 0;
}

static void append_line(const char *path, const char *line, mode_t mode) {
  size_t n;
  char *old = slurp(path, &n);
  size_t ll = strlen(line);
  char *nw = malloc(n + ll + 2);
  memcpy(nw, old, n);
  if (n && old[n - 1] != '\n')
    nw[n++] = '\n';
  memcpy(nw + n, line, ll);
  n += ll;
  nw[n++] = '\n';
  if (write_atomic(path, nw, n, mode) != 0)
    die("no se pudo escribir %s: %s", path, strerror(errno));
  free(old);
  free(nw);
}

// ---- passwd ---------------------------------------------------------------

static void make_salt(char *out, size_t cap) {
  static const char al[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./";
  unsigned char rnd[16];
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0 || read(fd, rnd, sizeof(rnd)) != (ssize_t)sizeof(rnd)) {
    for (int i = 0; i < 16; i++)
      rnd[i] = (unsigned char)(rand() ^ (int)time(NULL) ^ (i * 31));
  }
  if (fd >= 0)
    close(fd);
  size_t n = (size_t)snprintf(out, cap, "$6$");
  for (int i = 0; i < 16 && n + 2 < cap; i++)
    out[n++] = al[rnd[i] & 63];
  out[n++] = '$';
  out[n] = 0;
}

static int set_shadow_hash(const char *user, const char *hash) {
  size_t n;
  char *old = slurp(SHADOW_F, &n);
  char *out = malloc(n + strlen(hash) + 256);
  size_t o = 0;
  int found = 0;
  long days = (long)(time(NULL) / 86400);
  char *save = NULL;
  for (char *ln = strtok_r(old, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
    size_t ul = strlen(user);
    if (strncmp(ln, user, ul) == 0 && ln[ul] == ':') {
      // user:hash:lastchg:min:max:warn:inact:expire:flag
      char *f3 = strchr(ln + ul + 1, ':');
      const char *rest = f3 ? strchr(f3 + 1, ':') : NULL; // tras lastchg
      o += (size_t)sprintf(out + o, "%s:%s:%ld%s\n", user, hash, days,
                           rest ? rest : ":0:99999:7:::");
      found = 1;
    } else {
      o += (size_t)sprintf(out + o, "%s\n", ln);
    }
  }
  if (!found)
    o += (size_t)sprintf(out + o, "%s:%s:%ld:0:99999:7:::\n", user, hash, days);
  int rc = write_atomic(SHADOW_F, out, o, 0600);
  free(old);
  free(out);
  return rc;
}

static int cmd_passwd(int argc, char **argv) {
  const struct passwd *me = caller_pw();
  const char *target = argc > 1 ? argv[1] : (me ? me->pw_name : NULL);
  if (!target)
    die("passwd: usuario desconocido");
  struct passwd *pw = getpwnam(target);
  if (!pw)
    die("passwd: usuario '%s' no existe", target);
  if (getuid() != 0) {
    if (getuid() != pw->pw_uid)
      die("passwd: solo root puede cambiar la contraseña de otros");
    if (!account_locked(pw)) {
      char cur[256];
      if (read_secret("Contraseña actual: ", cur, sizeof(cur)) != 0)
        return 1;
      int bad = check_password(pw, cur);
      memset(cur, 0, sizeof(cur));
      if (bad) {
        sleep(2);
        die("passwd: contraseña incorrecta");
      }
    }
  }
  char p1[256], p2[256];
  if (read_secret("Nueva contraseña: ", p1, sizeof(p1)) != 0 ||
      read_secret("Repite la contraseña: ", p2, sizeof(p2)) != 0)
    return 1;
  if (strcmp(p1, p2) != 0)
    die("passwd: no coinciden");
  if (strlen(p1) < 1)
    die("passwd: contraseña vacia");
  char salt[40];
  make_salt(salt, sizeof(salt));
  char *h = crypt(p1, salt);
  if (!h || h[0] == '*')
    die("passwd: crypt fallo");
  char hash[256];
  snprintf(hash, sizeof(hash), "%s", h);
  memset(p1, 0, sizeof(p1));
  memset(p2, 0, sizeof(p2));
  if (set_shadow_hash(pw->pw_name, hash) != 0)
    die("passwd: no se pudo escribir %s: %s", SHADOW_F, strerror(errno));
  sync();
  fprintf(stderr, "passwd: contraseña de %s actualizada\n", pw->pw_name);
  return 0;
}

// ---- useradd --------------------------------------------------------------

static int mkdir_p(const char *path, mode_t mode) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      mkdir(tmp, 0755);
      *p = '/';
    }
  }
  return mkdir(tmp, mode) == 0 || errno == EEXIST ? 0 : -1;
}

static void copy_skel(const char *home, uid_t uid, gid_t gid) {
  FILE *d = NULL;
  (void)d;
  // copia superficial de ficheros regulares de /etc/skel
  char cmd[512];
  (void)cmd;
  FILE *ls = NULL;
  (void)ls;
  struct stat st;
  if (stat(SKEL_D, &st) != 0)
    return;
  // sin opendir recursivo: lista simple
  extern int skel_walk(const char *, const char *, uid_t, gid_t);
  skel_walk(SKEL_D, home, uid, gid);
}

#include <dirent.h>
int skel_walk(const char *src, const char *dst, uid_t uid, gid_t gid) {
  DIR *d = opendir(src);
  if (!d)
    return -1;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    char s[512], t[512];
    snprintf(s, sizeof(s), "%s/%s", src, e->d_name);
    snprintf(t, sizeof(t), "%s/%s", dst, e->d_name);
    struct stat st;
    if (stat(s, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode)) {
      mkdir(t, st.st_mode & 0777);
      chown(t, uid, gid);
      skel_walk(s, t, uid, gid);
    } else if (S_ISREG(st.st_mode)) {
      FILE *in = fopen(s, "r"), *out = fopen(t, "w");
      if (in && out) {
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof(b), in)) > 0)
          fwrite(b, 1, n, out);
      }
      if (in)
        fclose(in);
      if (out)
        fclose(out);
      chmod(t, st.st_mode & 0777);
      chown(t, uid, gid);
    }
  }
  closedir(d);
  return 0;
}

// Añade `user` como miembro del grupo `grp` (línea name:x:gid:miembros).
static void group_add_member(const char *grp, const char *user) {
  size_t n;
  char *old = slurp(GROUP_F, &n);
  char *out = malloc(n + strlen(user) + 8);
  size_t o = 0;
  int found = 0;
  char *save = NULL;
  for (char *ln = strtok_r(old, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
    size_t gl = strlen(grp);
    if (strncmp(ln, grp, gl) == 0 && ln[gl] == ':') {
      size_t l = strlen(ln);
      int has_members = l && ln[l - 1] != ':';
      o += (size_t)sprintf(out + o, "%s%s%s\n", ln, has_members ? "," : "", user);
      found = 1;
    } else {
      o += (size_t)sprintf(out + o, "%s\n", ln);
    }
  }
  if (!found)
    fprintf(stderr, "useradd: aviso, el grupo '%s' no existe\n", grp);
  else if (write_atomic(GROUP_F, out, o, 0644) != 0)
    die("no se pudo escribir %s", GROUP_F);
  free(old);
  free(out);
}

static int cmd_useradd(int argc, char **argv) {
  if (getuid() != 0)
    die("useradd: requiere root");
  int mk_home = 0;
  long uid = -1, gid = -1;
  const char *home = NULL, *shell = "/bin/sh", *groups = NULL, *comment = "";
  const char *name = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-m"))
      mk_home = 1;
    else if (!strcmp(argv[i], "-u") && i + 1 < argc)
      uid = atol(argv[++i]);
    else if (!strcmp(argv[i], "-g") && i + 1 < argc) {
      const char *g = argv[++i];
      struct group *gr = getgrnam(g);
      gid = gr ? (long)gr->gr_gid : atol(g);
    } else if (!strcmp(argv[i], "-G") && i + 1 < argc)
      groups = argv[++i];
    else if (!strcmp(argv[i], "-d") && i + 1 < argc)
      home = argv[++i];
    else if (!strcmp(argv[i], "-s") && i + 1 < argc)
      shell = argv[++i];
    else if (!strcmp(argv[i], "-c") && i + 1 < argc)
      comment = argv[++i];
    else if (argv[i][0] != '-')
      name = argv[i];
    else
      die("uso: useradd [-m] [-u uid] [-g grupo] [-G g1,g2] [-d home] [-s shell] [-c nombre] usuario");
  }
  if (!name || !*name)
    die("useradd: falta el nombre de usuario");
  for (const char *c = name; *c; c++)
    if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' ||
          *c == '-'))
      die("useradd: nombre invalido (use a-z 0-9 _ -)");
  if (getpwnam(name))
    die("useradd: '%s' ya existe", name);
  if (uid < 0) {
    uid = 1000;
    while (getpwuid((uid_t)uid))
      uid++;
  } else if (getpwuid((uid_t)uid)) {
    die("useradd: uid %ld en uso", uid);
  }
  int own_group = 0;
  if (gid < 0) {
    gid = uid;
    while (getgrgid((gid_t)gid))
      gid++;
    own_group = 1;
  }
  char homebuf[256];
  if (!home) {
    struct stat st;
    snprintf(homebuf, sizeof(homebuf), "%s/%s",
             stat("/data", &st) == 0 ? "/data/home" : "/home", name);
    home = homebuf;
  }
  char line[512];
  if (own_group) {
    snprintf(line, sizeof(line), "%s:x:%ld:", name, gid);
    append_line(GROUP_F, line, 0644);
  }
  snprintf(line, sizeof(line), "%s:x:%ld:%ld:%s:%s:%s", name, uid, gid, comment,
           home, shell);
  append_line(PASSWD_F, line, 0644);
  snprintf(line, sizeof(line), "%s:!:%ld:0:99999:7:::", name,
           (long)(time(NULL) / 86400));
  append_line(SHADOW_F, line, 0600);
  if (groups) {
    char *g = strdup(groups), *save = NULL;
    for (char *t = strtok_r(g, ",", &save); t; t = strtok_r(NULL, ",", &save))
      group_add_member(t, name);
    free(g);
  }
  if (mk_home) {
    if (mkdir_p(home, 0700) != 0)
      fprintf(stderr, "useradd: no se pudo crear %s: %s\n", home, strerror(errno));
    chown(home, (uid_t)uid, (gid_t)gid);
    chmod(home, 0700);
    copy_skel(home, (uid_t)uid, (gid_t)gid);
  }
  sync();
  fprintf(stderr, "useradd: creado %s (uid=%ld gid=%ld home=%s); use passwd para fijar la contraseña\n",
          name, uid, gid, home);
  return 0;
}

// ---- sudo -----------------------------------------------------------------

static int in_group(const struct passwd *pw, const char *grp) {
  struct group *g = getgrnam(grp);
  if (!g)
    return 0;
  if (pw->pw_gid == g->gr_gid)
    return 1;
  for (char **m = g->gr_mem; m && *m; m++)
    if (strcmp(*m, pw->pw_name) == 0)
      return 1;
  return 0;
}

static int cmd_sudo(int argc, char **argv) {
  const char *target = "root";
  int i = 1;
  if (i < argc && !strcmp(argv[i], "-u") && i + 1 < argc) {
    target = argv[i + 1];
    i += 2;
  }
  if (i >= argc)
    die("uso: sudo [-u usuario] orden [argumentos]");
  const struct passwd *me = caller_pw();
  if (!me)
    die("sudo: usuario desconocido (uid %u)", getuid());
  struct passwd me_copy = *me;
  char me_name[64];
  snprintf(me_name, sizeof(me_name), "%s", me->pw_name);
  me_copy.pw_name = me_name;
  if (getuid() != 0) {
    if (!in_group(&me_copy, "wheel"))
      die("sudo: %s no pertenece al grupo wheel", me_name);
    if (access("/etc/sudo-nopasswd", F_OK) != 0) {
      char pass[256];
      if (read_secret("Password: ", pass, sizeof(pass)) != 0)
        return 1;
      struct passwd *again = getpwnam(me_name);
      int bad = !again || check_password(again, pass);
      memset(pass, 0, sizeof(pass));
      if (bad) {
        sleep(2);
        die("sudo: autenticacion fallida");
      }
    }
  }
  struct passwd *pw = getpwnam(target);
  if (!pw)
    die("sudo: usuario '%s' no existe", target);
  if (initgroups(pw->pw_name, pw->pw_gid) != 0 || setgid(pw->pw_gid) != 0 ||
      setuid(pw->pw_uid) != 0)
    die("sudo: no se pudo cambiar de usuario: %s", strerror(errno));
  setenv("HOME", pw->pw_dir, 1);
  setenv("USER", pw->pw_name, 1);
  setenv("LOGNAME", pw->pw_name, 1);
  execvp(argv[i], argv + i);
  die("sudo: %s: %s", argv[i], strerror(errno));
}

int main(int argc, char **argv) {
  char *a0 = strdup(argv[0]);
  const char *b = basename(a0);
  if (!strcmp(b, "aurora-login") || !strcmp(b, "login"))
    return cmd_login(argc, argv);
  if (!strcmp(b, "aurora-su") || !strcmp(b, "su"))
    return cmd_su(argc, argv);
  if (!strcmp(b, "aurora-passwd") || !strcmp(b, "passwd"))
    return cmd_passwd(argc, argv);
  if (!strcmp(b, "aurora-useradd") || !strcmp(b, "useradd"))
    return cmd_useradd(argc, argv);
  if (!strcmp(b, "aurora-sudo") || !strcmp(b, "sudo"))
    return cmd_sudo(argc, argv);
  fprintf(stderr, "aurora-users: llamar como aurora-login|su|passwd|useradd|sudo (era '%s')\n", b);
  return 2;
}
