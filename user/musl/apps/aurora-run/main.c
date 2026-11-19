// aurora-run — el lanzador de AuroraOS.
//
// Todo lo que se abre como programa (Dock, Aplicaciones, Archivos, la terminal
// con ./programa.exe a traves del kernel) pasa por aqui. aurora-run mira QUE es
// el fichero y elige con que ejecutarlo:
//
//   aurora   app nativa de Aurora (ELF con la nota .note.aurora, o registrada
//            en /usr/share/aurora/apps): se ejecuta tal cual.
//   linux    cualquier otro ELF o script #!: tal cual, con el entorno del
//            escritorio (Wayland/X, tema GTK) que esperan las apps Linux.
//   windows  PE (MZ...PE), .msi, .lnk, .bat/.cmd: aurora-wine (Wine 11).
//   vm       mini-VM para lo que Wine no puede (todavia no existe: se avisa).
//
// Reglas propias (primera coincidencia gana), en /etc/aurora/run.conf y luego
// ~/.config/aurora/run.conf:   <patron-glob> <aurora|linux|windows|vm>
// El patron se compara con la ruta completa y con el nombre del fichero.
//
// Uso: aurora-run [--que] [--terminal] [--con TIPO] PROGRAMA|FICHERO.desktop [args...]
//   --que       solo dice que es y con que se abriria (no ejecuta)
//   --terminal  dentro del Terminal de Aurora (terminal -e ...)
//   --con TIPO  fuerza el tipo (aurora, linux, windows, vm)
//   --binfmt    lo usa el kernel al ejecutar un .exe (execve de un PE)
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define ASYS_THEME_GET (0x1000 + 0x70)
#define WINE_LAUNCHER "/usr/bin/aurora-wine"
#define VM_LAUNCHER "/usr/libexec/aurora/aurora-vm"
#define TERMINAL "/usr/bin/terminal"
#define MAX_ARGS 256

enum kind { K_NONE, K_AURORA, K_LINUX, K_WINDOWS, K_VM };
static const char *const kind_name[] = {"desconocido", "aurora", "linux", "windows", "vm"};
static const char *const kind_desc[] = {
    "no se reconoce el formato",
    "app de Aurora: se ejecuta tal cual",
    "app Linux: se ejecuta tal cual con el entorno del escritorio",
    "app Windows: se abre con Wine (aurora-wine)",
    "mini-VM: para apps que Wine no puede abrir",
};

static int g_verbose;

static enum kind kind_from_name(const char *s) {
  for (int k = K_AURORA; k <= K_VM; k++)
    if (!strcasecmp(s, kind_name[k]))
      return (enum kind)k;
  if (!strcasecmp(s, "wine"))
    return K_WINDOWS;
  return K_NONE;
}

static const char *base_name(const char *p) {
  const char *b = strrchr(p, '/');
  return b ? b + 1 : p;
}

static int has_ext(const char *p, const char *ext) {
  size_t n = strlen(p), e = strlen(ext);
  return n > e && !strcasecmp(p + n - e, ext);
}

static int is_file(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

// Como execvp: nombre sin '/' se busca en PATH.
static int resolve(const char *name, char *out, size_t n) {
  if (strchr(name, '/')) {
    snprintf(out, n, "%s", name);
    return is_file(out);
  }
  const char *path = getenv("PATH");
  if (!path || !*path)
    path = "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:/usr/games";
  char buf[1024];
  snprintf(buf, sizeof(buf), "%s", path);
  for (char *save = NULL, *d = strtok_r(buf, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
    snprintf(out, n, "%s/%s", *d ? d : ".", name);
    if (is_file(out))
      return 1;
  }
  return 0;
}

// ---- Reconocer el formato ----

// ELF con la seccion .note.aurora (la ponen las apps nativas: user/musl).
static int elf_has_aurora_note(int fd) {
  unsigned char eh[64];
  if (pread(fd, eh, sizeof(eh), 0) != (ssize_t)sizeof(eh) || eh[4] != 2 /* ELFCLASS64 */)
    return 0;
  uint64_t shoff;
  uint16_t shentsize, shnum, shstrndx;
  memcpy(&shoff, eh + 0x28, 8);
  memcpy(&shentsize, eh + 0x3A, 2);
  memcpy(&shnum, eh + 0x3C, 2);
  memcpy(&shstrndx, eh + 0x3E, 2);
  if (!shoff || shentsize < 64 || !shnum || shnum > 4096 || shstrndx >= shnum)
    return 0;
  unsigned char sh[64];
  if (pread(fd, sh, 64, (off_t)(shoff + (uint64_t)shstrndx * shentsize)) != 64)
    return 0;
  uint64_t stroff, strsz;
  memcpy(&stroff, sh + 0x18, 8);
  memcpy(&strsz, sh + 0x20, 8);
  if (strsz > 1 << 20)
    return 0;
  char *names = malloc(strsz + 1);
  if (!names || pread(fd, names, strsz, (off_t)stroff) != (ssize_t)strsz) {
    free(names);
    return 0;
  }
  names[strsz] = 0;
  int found = 0;
  for (unsigned i = 0; i < shnum && !found; i++) {
    if (pread(fd, sh, 64, (off_t)(shoff + (uint64_t)i * shentsize)) != 64)
      break;
    uint32_t nm, type;
    memcpy(&nm, sh, 4);
    memcpy(&type, sh + 4, 4);
    if (type == 7 /* SHT_NOTE */ && nm < strsz && !strcmp(names + nm, ".note.aurora"))
      found = 1;
  }
  free(names);
  return found;
}

// ¿Lo registra /usr/share/aurora/apps/*.desktop (Exec=) como app nativa?
static int registered_native(const char *path) {
  static const char *const dir = "/usr/share/aurora/apps";
  // Las nativas se registran como <nombre-del-binario>.desktop.
  char d[1100];
  snprintf(d, sizeof(d), "%s/%s.desktop", dir, base_name(path));
  FILE *f = fopen(d, "r");
  if (!f)
    return 0;
  char line[512];
  int ok = 0;
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "Exec=", 5))
      continue;
    char *e = line + 5, *sp = strpbrk(e, " \t\r\n");
    if (sp)
      *sp = 0;
    char rp[512];
    ok = resolve(e, rp, sizeof(rp)) && !strcmp(rp, path);
    break;
  }
  fclose(f);
  return ok;
}

static enum kind detect(const char *path) {
  if (has_ext(path, ".msi") || has_ext(path, ".lnk") || has_ext(path, ".bat") ||
      has_ext(path, ".cmd"))
    return K_WINDOWS;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return K_NONE;
  unsigned char h[64];
  ssize_t n = pread(fd, h, sizeof(h), 0);
  enum kind k = K_NONE;
  if (n >= 4 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') {
    k = (elf_has_aurora_note(fd) || registered_native(path)) ? K_AURORA : K_LINUX;
  } else if (n >= 2 && h[0] == '#' && h[1] == '!') {
    k = K_LINUX; // script: su interprete decide
  } else if (n >= 64 && h[0] == 'M' && h[1] == 'Z') {
    uint32_t pe;
    memcpy(&pe, h + 0x3C, 4);
    unsigned char sig[4];
    // PE (Win32/Win64) o, si no, MZ de DOS/NE de 16 bits: Wine abre ambos.
    (void)(pe < (1u << 24) && pread(fd, sig, 4, pe) == 4 && !memcmp(sig, "PE\0\0", 4));
    k = K_WINDOWS;
  } else if (has_ext(path, ".exe") || has_ext(path, ".com")) {
    k = K_WINDOWS;
  }
  close(fd);
  return k;
}

// Reglas del usuario y del sistema: "<patron> <tipo>".
static enum kind rules_lookup_file(const char *conf, const char *path) {
  FILE *f = fopen(conf, "r");
  if (!f)
    return K_NONE;
  char line[512];
  enum kind k = K_NONE;
  while (k == K_NONE && fgets(line, sizeof(line), f)) {
    char *p = line;
    while (isspace((unsigned char)*p))
      p++;
    if (!*p || *p == '#')
      continue;
    char pat[384], typ[32];
    if (sscanf(p, "%383s %31s", pat, typ) != 2)
      continue;
    if (!fnmatch(pat, path, 0) || !fnmatch(pat, base_name(path), FNM_CASEFOLD))
      k = kind_from_name(typ);
  }
  fclose(f);
  return k;
}

static enum kind rules_lookup(const char *path) {
  char user[512];
  const char *home = getenv("HOME");
  if (home && *home) {
    snprintf(user, sizeof(user), "%s/.config/aurora/run.conf", home);
    enum kind k = rules_lookup_file(user, path);
    if (k != K_NONE)
      return k;
  }
  return rules_lookup_file("/etc/aurora/run.conf", path);
}

// ---- Entorno del escritorio (el que pone el Dock) ----
static void desktop_env(void) {
  setenv("XDG_RUNTIME_DIR", "/run/user/0", 0);
  setenv("WAYLAND_DISPLAY", "wayland-0", 0);
  setenv("DISPLAY", ":0", 0);
  long t = syscall(ASYS_THEME_GET, 0, 0, 0, 0, 0);
  int light = t >= 0 && (t & 1);
  if (!getenv("AURORA_THEME"))
    setenv("AURORA_THEME", light ? "light" : "dark", 1);
  if (!getenv("GTK_THEME"))
    setenv("GTK_THEME", !strcmp(getenv("AURORA_THEME"), "light") ? "Aurora-light" : "Aurora", 1);
}

// .desktop: Exec= (sin los %f/%u) y Terminal=.
static int read_desktop(const char *file, char *exec, size_t n, int *terminal) {
  FILE *f = fopen(file, "r");
  if (!f)
    return 0;
  char line[1024];
  int in_entry = 0;
  exec[0] = 0;
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = 0;
    if (line[0] == '[') {
      in_entry = !strcmp(line, "[Desktop Entry]");
      continue;
    }
    if (!in_entry)
      continue;
    if (!strncmp(line, "Exec=", 5)) {
      size_t o = 0;
      for (const char *p = line + 5; *p && o + 1 < n; p++) {
        if (*p == '%' && p[1]) {
          p++;
          continue;
        }
        if (*p == '"')
          continue;
        exec[o++] = *p;
      }
      exec[o] = 0;
    } else if (!strncmp(line, "Terminal=", 9)) {
      *terminal = !strcasecmp(line + 9, "true");
    }
  }
  fclose(f);
  return exec[0] != 0;
}

static void die_msg(const char *path, const char *why) {
  fprintf(stderr, "aurora-run: %s: %s\n", path, why);
}

int main(int argc, char **argv) {
  int que = 0, terminal = 0, binfmt = 0;
  enum kind force = K_NONE;
  int i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
    if (!strcmp(argv[i], "--que") || !strcmp(argv[i], "--what"))
      que = 1;
    else if (!strcmp(argv[i], "--terminal"))
      terminal = 1;
    else if (!strcmp(argv[i], "--binfmt"))
      binfmt = 1;
    else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
      g_verbose = 1;
    else if (!strcmp(argv[i], "--con") && i + 1 < argc)
      force = kind_from_name(argv[++i]);
    else if (!strcmp(argv[i], "--")) {
      i++;
      break;
    } else if (!strcmp(argv[i], "--help")) {
      printf("Uso: aurora-run [--que] [--terminal] [--con aurora|linux|windows|vm] "
             "PROGRAMA [args...]\n");
      return 0;
    }
  }
  if (i >= argc) {
    fprintf(stderr, "Uso: aurora-run [--que] [--terminal] [--con TIPO] PROGRAMA [args...]\n");
    return 2;
  }
  if (getenv("AURORA_RUN_VERBOSE"))
    g_verbose = 1;

  // Un .desktop: se lanza su Exec= (y sus argumentos van detras).
  char dexec[1024];
  if (has_ext(argv[i], ".desktop") && is_file(argv[i])) {
    if (!read_desktop(argv[i], dexec, sizeof(dexec), &terminal)) {
      die_msg(argv[i], "el .desktop no tiene Exec=");
      return 127;
    }
    static char *nargv[MAX_ARGS + 8];
    int nc = 0;
    nargv[nc++] = argv[0];
    if (que)
      nargv[nc++] = "--que";
    if (terminal)
      nargv[nc++] = "--terminal";
    nargv[nc++] = "--";
    for (char *s = NULL, *t = strtok_r(dexec, " \t", &s); t && nc < MAX_ARGS; t = strtok_r(NULL, " \t", &s))
      nargv[nc++] = t;
    for (int j = i + 1; j < argc && nc < MAX_ARGS + 6; j++)
      nargv[nc++] = argv[j];
    nargv[nc] = NULL;
    execv("/proc/self/exe", nargv);
    execv("/usr/bin/aurora-run", nargv);
    return 127;
  }

  char path[1024];
  if (!resolve(argv[i], path, sizeof(path))) {
    // "notepad", "winecfg": programas de Wine que no estan en PATH.
    if (force == K_WINDOWS) {
      snprintf(path, sizeof(path), "%s", argv[i]);
    } else {
      die_msg(argv[i], "no se encuentra");
      return 127;
    }
  }

  enum kind k = force;
  const char *why = "--con";
  if (k == K_NONE) {
    k = rules_lookup(path);
    why = "regla de run.conf";
  }
  if (k == K_NONE) {
    k = detect(path);
    why = "formato del fichero";
  }
  // Un PE que una regla manda a "aurora"/"linux" volveria aqui por el kernel.
  if (binfmt && (k == K_AURORA || k == K_LINUX))
    k = K_WINDOWS;

  if (que || g_verbose) {
    printf("%s: %s (%s; %s)\n", path, kind_name[k], kind_desc[k], why);
    fflush(stdout);
    if (que)
      return k == K_NONE ? 1 : 0;
  }

  static char *nargv[MAX_ARGS + 8];
  int nc = 0;
  if (terminal) {
    nargv[nc++] = TERMINAL;
    nargv[nc++] = "-e";
  }
  const char *prog;
  switch (k) {
  case K_AURORA:
  case K_LINUX:
    if (k == K_LINUX)
      desktop_env();
    prog = path;
    nargv[nc++] = path;
    break;
  case K_WINDOWS:
    desktop_env();
    if (!is_file(WINE_LAUNCHER)) {
      die_msg(path, "es una app Windows y esta ISO no trae Wine (WITH_WINE=1)");
      return 126;
    }
    prog = WINE_LAUNCHER;
    nargv[nc++] = WINE_LAUNCHER;
    nargv[nc++] = path;
    break;
  case K_VM:
    if (!is_file(VM_LAUNCHER)) {
      die_msg(path, "la mini-VM de Aurora todavia no esta disponible");
      return 126;
    }
    desktop_env();
    prog = VM_LAUNCHER;
    nargv[nc++] = VM_LAUNCHER;
    nargv[nc++] = path;
    break;
  default:
    die_msg(path, "no es un programa que Aurora sepa abrir");
    return 126;
  }
  for (int j = i + 1; j < argc && nc < MAX_ARGS + 6; j++)
    nargv[nc++] = argv[j];
  nargv[nc] = NULL;
  if (terminal)
    prog = TERMINAL;
  execv(prog, nargv);
  fprintf(stderr, "aurora-run: %s: %s\n", prog, strerror(errno));
  return errno == ENOENT ? 127 : 126;
}
