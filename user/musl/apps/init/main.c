// user/musl/apps/init/main.c
//
// init (PID 1) de Aurora OS.
//
// Responsabilidades:
//   1. Lanzar el terminal gráfico (/apps/terminal) como primer hijo.
//   2. Reapear huérfanos reparentados a PID 1 por el kernel cuando su
//      padre muere (Bloque D en kernel/process.c).
//   3. Relanzar el terminal si muere por crash. Si el usuario lo cierra
//      limpiamente (exit 0), NO respawnear — init se queda como reaper.
//
// NO hace (todavía):
//   - mount(/proc)      -> el kernel ya lo monta.
//   - Leer /etc/inittab -> no existe.
//   - Login / getty     -> el terminal arranca directamente sh.
//   - Doble fork de daemons -> no hay daemons clásicos.
//   - Relanzar el terminal tras un cierre limpio -> hace falta un
//     launcher (taskbar, Ctrl+Alt+T, ...). Pendiente Fase 4.

#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TERMINAL_PATH "/usr/bin/terminal"
#define WAYLAND_PATH "/usr/bin/aurora-wayland"
#define RESPAWN_DELAY_SEC 1

static char *default_envp[] = {
    "PATH=/usr/bin:/usr/sbin:/usr/local/bin:/bin:/sbin",
    "TERM=linux",
    "HOME=/data",
    "USER=root",
    "LOGNAME=root",
    "SHELL=/bin/sh",
    "PWD=/",
    "TMPDIR=/tmp",
    // Apps Wayland: las ventanas las gestiona aurora-wayland (ver abajo).
    "XDG_RUNTIME_DIR=/run/user/0",
    "WAYLAND_DISPLAY=wayland-0",
    "LANG=C.UTF-8",
    "FONTCONFIG_FILE=/usr/share/fontconfig/fonts.conf",
    "XDG_DATA_DIRS=/usr/share",
    // Que ningun toolkit dibuje su propia barra de titulo: la unica es la de
    // Aurora (los que no lo respetan se recortan en aurora-wayland, ver
    // /usr/share/aurora/csd-rules.conf).
    "GTK_CSD=0",
    "QT_QPA_PLATFORM=wayland",
    "QT_WAYLAND_DISABLE_WINDOWDECORATION=1",
    "SDL_VIDEODRIVER=wayland",
    "SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR=0",
    "MOZ_ENABLE_WAYLAND=1",
    "ELECTRON_OZONE_PLATFORM_HINT=wayland",
    // OpenGL/EGL: por software (Mesa llvmpipe sobre wl_shm) salvo que haya
    // una GPU con driver de Linux (ver gpu_env_setup(): quita la linea).
    "EGL_PLATFORM=wayland",
    "LIBGL_ALWAYS_SOFTWARE=1",
    "MESA_SHADER_CACHE_DISABLE=true",
    // GTK3 / Qt5: sin dconf ni bus de accesibilidad, datos en /usr/share y
    // configuracion de GTK en /usr/share/aurora/xdg (ver settings.ini).
    // Apps X11: Xwayland (lo arranca aurora-wayland) escucha en :0.
    "DISPLAY=:0",
    // Recursos de las apps Xt (xterm...): fuente del sistema y colores.
    "XENVIRONMENT=/usr/share/aurora/xdg/Xresources",
    "GDK_BACKEND=wayland",
    "GTK_THEME=Aurora",
    // Modulo que cambia el tema GTK3 en vivo con el modo claro/oscuro.
    "GTK_MODULES=aurora-theme",
    "GTK_PATH=/usr/lib/aurora-gtk",
    "GSETTINGS_BACKEND=memory",
    "GSETTINGS_SCHEMA_DIR=/usr/share/glib-2.0/schemas",
    "GDK_PIXBUF_MODULE_FILE=/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders.cache",
    "NO_AT_BRIDGE=1",
    "QT_PLUGIN_PATH=/usr/lib/x86_64-linux-gnu/qt5/plugins",
    // Tema de plataforma de Aurora: Fusion + paleta + QSS (user/aurora-qt-theme).
    "QT_QPA_PLATFORMTHEME=aurora",
    "XDG_CONFIG_HOME=/tmp/.config",
    "XDG_CONFIG_DIRS=/usr/share/aurora/xdg",
    "XDG_CACHE_HOME=/tmp/.cache",
    "XDG_DATA_HOME=/tmp/.local/share",
    NULL,
};

// ¿Hay una GPU con 3D por hardware? Lo dice el driver de Linux que ha
// atado a card0 (DRIVER= en su uevent). Con bochs/simpledrm/virtio o el
// backend nativo no hay y Mesa sigue en llvmpipe; con i915/amdgpu/nouveau
// Mesa elige solo crocus/iris/radeonsi/nouveau segun el PCI id, como en
// Linux, sin pedirle nada al usuario.
static int gpu_has_hw_3d(void) {
  static const char *const hw[] = {"i915", "amdgpu", "radeon", "nouveau", NULL};
  char buf[512];
  int fd = open("/sys/class/drm/card0/device/uevent", O_RDONLY);
  if (fd < 0)
    return 0;
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return 0;
  buf[n] = '\0';
  char *d = strstr(buf, "DRIVER=");
  if (!d)
    return 0;
  d += 7;
  for (int i = 0; hw[i]; i++) {
    size_t l = strlen(hw[i]);
    if (!strncmp(d, hw[i], l) && (d[l] == '\n' || d[l] == '\0'))
      return 1;
  }
  return 0;
}

static void gpu_env_setup(void) {
  if (!gpu_has_hw_3d())
    return;
  for (int i = 0; default_envp[i]; i++) {
    if (!strncmp(default_envp[i], "LIBGL_ALWAYS_SOFTWARE=", 22)) {
      for (int j = i; default_envp[j]; j++)
        default_envp[j] = default_envp[j + 1];
      printf("init: GPU con 3D por hardware, Mesa elige su driver\n");
      return;
    }
  }
}

// [U5b] aur.session=user:<nombre>: el compositor y sus apps corren como ese
// usuario (instalado). Sin el parametro, todo sigue como root (live).
static struct passwd g_sess_pw;
static char g_sess_buf[512];
static int g_sess_ok;
static char g_sess_env[4][160];
static char *g_sess_envp[96];

static void write_session_file(const char *name) {
  int fd = open("/run/aurora-session", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    if (write(fd, name, strlen(name)) < 0) {}
    close(fd);
  }
}

// Usuario de sesion pedido en la linea de comandos (aur.session=user:<n>).
static int session_from_cmdline(char *name, size_t cap) {
  char cl[1024];
  int fd = open("/proc/cmdline", O_RDONLY);
  ssize_t n = fd >= 0 ? read(fd, cl, sizeof(cl) - 1) : -1;
  if (fd >= 0)
    close(fd);
  if (n <= 0)
    return 0;
  cl[n] = '\0';
  char *p = strstr(cl, "aur.session=user:");
  if (!p)
    return 0;
  p += 17;
  size_t l = strcspn(p, " \t\r\n");
  if (l == 0 || l >= cap)
    return 0;
  memcpy(name, p, l);
  name[l] = '\0';
  return 1;
}

static void session_init(const char *name) {
  g_sess_ok = 0;
  if (!name || !*name || !strcmp(name, "root")) {
    write_session_file("root");
    return;
  }
  struct passwd *res = NULL;
  if (getpwnam_r(name, &g_sess_pw, g_sess_buf, sizeof(g_sess_buf), &res) != 0 ||
      !res || res->pw_uid == 0) {
    fprintf(stderr, "init: sesion '%s': usuario no valido, sigo como root\n", name);
    write_session_file("root");
    return;
  }
  g_sess_ok = 1;
  // Entorno de la sesion: igual que default_envp con HOME/USER/XDG propios.
  snprintf(g_sess_env[0], 160, "XDG_RUNTIME_DIR=/run/user/%u", res->pw_uid);
  snprintf(g_sess_env[1], 160, "HOME=%s", res->pw_dir);
  snprintf(g_sess_env[2], 160, "USER=%s", res->pw_name);
  snprintf(g_sess_env[3], 160, "LOGNAME=%s", res->pw_name);
  int j = 0;
  for (int i = 0; default_envp[i] && j < 90; i++) {
    const char *e = default_envp[i];
    if (!strncmp(e, "XDG_RUNTIME_DIR=", 16) || !strncmp(e, "HOME=", 5) ||
        !strncmp(e, "USER=", 5) || !strncmp(e, "LOGNAME=", 8))
      continue;
    g_sess_envp[j++] = default_envp[i];
  }
  for (int i = 0; i < 4; i++)
    g_sess_envp[j++] = g_sess_env[i];
  g_sess_envp[j] = NULL;

  // Directorios de la sesion (los crea root antes de bajar privilegios).
  char rt[64];
  snprintf(rt, sizeof(rt), "/run/user/%u", res->pw_uid);
  mkdir(rt, 0700);
  chown(rt, res->pw_uid, res->pw_gid);
  static const char *const own[] = {"/tmp/.config", "/tmp/.config/gtk-4.0",
                                    "/tmp/.cache", "/tmp/.local",
                                    "/tmp/.local/share", NULL};
  for (int i = 0; own[i]; i++) {
    mkdir(own[i], 0755);
    chown(own[i], res->pw_uid, res->pw_gid);
  }
  write_session_file(res->pw_name);
  printf("init: sesion de usuario %s (uid %u)\n", res->pw_name, res->pw_uid);
}

static char **session_envp(void) { return g_sess_ok ? g_sess_envp : default_envp; }

// aur.mode=installer (ISO instaladora): en lugar del terminal se lanza la app
// grafica del instalador a pantalla completa y no se arranca el servidor Wayland.
static const char *g_term_path = TERMINAL_PATH;
static int g_installer_mode;

static void detect_installer_mode(void) {
  char cl[2048] = "";
  int fd = open("/proc/cmdline", O_RDONLY);
  if (fd >= 0) {
    ssize_t n = read(fd, cl, sizeof(cl) - 1);
    close(fd);
    cl[n > 0 ? n : 0] = 0;
  }
  if (strstr(cl, "aur.mode=installer") && access("/usr/bin/aurora-installer", X_OK) == 0) {
    g_installer_mode = 1;
    g_term_path = "/usr/bin/aurora-installer";
  }
}

// Paquetes pendientes del instalador (/usr/lib/aurora-apt/firstboot-packages):
// se instalan con APT en cuanto haya red, reintentando en segundo plano.
static void run_firstboot_packages(void) {
  if (access("/usr/lib/aurora-apt/firstboot-packages", R_OK) != 0 ||
      access("/usr/bin/aurora-pkg", X_OK) != 0)
    return;
  pid_t pid = fork();
  if (pid == 0) {
    char *argv[] = {"bash", "-c",
                    "for i in $(seq 1 60); do "
                    "aurora-pkg check >/dev/null 2>&1 && "
                    "aurora-pkg install $(cat /usr/lib/aurora-apt/firstboot-packages) "
                    "&& rm -f /usr/lib/aurora-apt/firstboot-packages && exit 0; "
                    "sleep 20; done",
                    NULL};
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) {
      dup2(fd, 1);
      dup2(fd, 2);
    }
    execve("/bin/bash", argv, default_envp);
    _exit(127);
  }
}

static pid_t spawn_terminal(void) {
  pid_t pid = fork();
  if (pid < 0) {
    perror("init: fork");
    return -1;
  }
  if (pid == 0) {
    char *argv[] = {(char *)g_term_path, NULL};
    execve(g_term_path, argv, session_envp());
    perror("init: execve terminal");
    _exit(127);
  }
  return pid;
}

// Servidor Wayland nativo: cada ventana Wayland es una ventana de Aurora.
static pid_t spawn_wayland(void) {
  pid_t pid = fork();
  if (pid < 0) {
    perror("init: fork");
    return -1;
  }
  if (pid == 0) {
    if (access(WAYLAND_PATH, X_OK) != 0)
      _exit(127);
    // Su salida es solo diagnostico: a /dev/kmsg (serie), no a una tty.
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) {
      dup2(fd, 1);
      dup2(fd, 2);
      if (fd > 2)
        close(fd);
    }
    char *argv[] = {(char *)WAYLAND_PATH, NULL};
    if (g_sess_ok) {
      // [U5b] compositor sin root; los nodos /dev dan acceso por grupo.
      if (initgroups(g_sess_pw.pw_name, g_sess_pw.pw_gid) != 0 ||
          setgid(g_sess_pw.pw_gid) != 0 || setuid(g_sess_pw.pw_uid) != 0) {
        fprintf(stderr, "init: no pude bajar privilegios del compositor\n");
        _exit(127);
      }
    }
    execve(WAYLAND_PATH, argv, session_envp());
    _exit(127);
  }
  return pid;
}

// Si existe /etc/fstab con entradas, monta lo que falte (mount -a). El /data
// de arranque lo monta el kernel; mount -a salta lo ya montado. Sin fstab o con
// error no pasa nada: solo se deja constancia en /dev/kmsg.
static void run_fstab(void) {
  if (access("/etc/fstab", R_OK) != 0 || access("/usr/bin/mount", X_OK) != 0)
    return;
  // Los discos aparecen despues (los drivers de usuario arrancan en paralelo):
  // se reintenta en segundo plano hasta ~60 s. mount -a salta lo ya montado.
  pid_t pid = fork();
  if (pid == 0) {
    char *argv[] = {"sh", "-c",
                    "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 "
                    "21 22 23 24 25 26 27 28 29 30; do "
                    "/usr/bin/mount -a > /dev/null 2>&1 && exit 0; sleep 2; done; "
                    "/usr/bin/mount -a > /dev/kmsg 2>&1",
                    NULL};
    execve("/bin/bash", argv, default_envp);
    _exit(127);
  }
}

// E7.4: repara AURORADATA si el kernel la monto en solo lectura por estar sucia.
static void run_autofsck(void) {
  if (access("/usr/sbin/aurora-fsck", X_OK) != 0)
    return;
  pid_t pid = fork();
  if (pid == 0) {
    char *argv[] = {"sh", "-c",
                    "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do "
                    "ls /dev/sd? /dev/nvme?n? /dev/vd? > /dev/null 2>&1 && break; "
                    "sleep 2; done; sleep 5; "
                    "/usr/sbin/aurora-fsck > /dev/kmsg 2>&1",
                    NULL};
    execve("/bin/bash", argv, default_envp);
    _exit(127);
  }
}

// Si existe /system/boot-tests, ejecuta cada linea con sh y manda su salida a
// /dev/kmsg (serie). Sirve para tests automaticos en QEMU sin teclado.
static void run_boot_tests(void) {
  FILE *f = fopen("/system/boot-tests", "r");
  if (!f)
    return;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = 0;
    if (!n || line[0] == '#')
      continue;
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "{ %s; } > /dev/kmsg 2>&1", line);
    pid_t pid = fork();
    if (pid == 0) {
      char *argv[] = {"bash", "-c", cmd, NULL};
      execve("/bin/bash", argv, default_envp);
      _exit(127);
    }
    if (pid > 0) {
      // Una senal (p. ej. SIGCHLD de run_fstab) no debe solapar las lineas.
      while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
    }
  }
  fclose(f);
}

// libadwaita ignora GTK_THEME y solo lee $XDG_CONFIG_HOME/gtk-4.0/gtk.css (que
// vive en tmpfs): se enlaza el gtk.css de Aurora para que las apps GTK4 se vean
// con su paleta.
static void setup_gtk4_config(void) {
  mkdir("/tmp/.config", 0755);
  mkdir("/tmp/.config/gtk-4.0", 0755);
  if (symlink("/usr/share/aurora/xdg/gtk-4.0/gtk.css",
              "/tmp/.config/gtk-4.0/gtk.css") < 0 &&
      errno != EEXIST)
    perror("init: gtk-4.0/gtk.css");
}

// ---- [U7.4] cambio de usuario: reinicio de sesion --------------------------
static volatile sig_atomic_t g_switch;

static int pid_ppid_uid(int pid, int *ppid, long *uid) {
  char path[48], buf[1024];
  snprintf(path, sizeof(path), "/proc/%d/status", pid);
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return -1;
  buf[n] = '\0';
  char *pp = strstr(buf, "PPid:");
  char *up = strstr(buf, "Uid:");
  if (!pp || !up)
    return -1;
  *ppid = atoi(pp + 5);
  *uid = atol(up + 4);
  return 0;
}

// Mata la sesion actual: descendientes del terminal y del compositor y, si la
// sesion no es root, todo lo que corra con ese uid (nunca los .aur de root).
static void kill_session(pid_t term, pid_t wl) {
  static int pids[1024], ppids[1024];
  static long uids[1024];
  int cnt = 0;
  for (int round = 0; round < 2; round++) {
    cnt = 0;
    DIR *d = opendir("/proc");
    struct dirent *de;
    while (d && (de = readdir(d)) && cnt < 1024) {
      int pid = atoi(de->d_name);
      if (pid <= 1)
        continue;
      int pp;
      long u;
      if (pid_ppid_uid(pid, &pp, &u) == 0) {
        pids[cnt] = pid;
        ppids[cnt] = pp;
        uids[cnt] = u;
        cnt++;
      }
    }
    if (d)
      closedir(d);
    // marcar descendientes de term/wl
    static char mark[1024];
    memset(mark, 0, sizeof(mark));
    for (int i = 0; i < cnt; i++)
      if (pids[i] == term || pids[i] == wl)
        mark[i] = 1;
    for (int pass = 0; pass < 8; pass++)
      for (int i = 0; i < cnt; i++)
        if (!mark[i])
          for (int j = 0; j < cnt; j++)
            if (mark[j] && ppids[i] == pids[j]) {
              mark[i] = 1;
              break;
            }
    for (int i = 0; i < cnt; i++) {
      int hit = mark[i] || (g_sess_ok && uids[i] == (long)g_sess_pw.pw_uid);
      if (hit)
        kill(pids[i], round == 0 ? SIGTERM : SIGKILL);
    }
    if (round == 0) {
      for (int w = 0; w < 20; w++) {
        usleep(100000);
        while (waitpid(-1, NULL, WNOHANG) > 0) {}
      }
    }
  }
  while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

static void read_switch_request(char *name, size_t cap) {
  name[0] = '\0';
  int fd = open("/run/aurora-session-request", O_RDONLY);
  if (fd < 0)
    return;
  ssize_t n = read(fd, name, cap - 1);
  close(fd);
  if (n <= 0) { // peticion a medio escribir: se reintenta en la proxima vuelta
    name[0] = '\0';
    return;
  }
  unlink("/run/aurora-session-request");
  name[n] = '\0';
  name[strcspn(name, "\r\n ")] = '\0';
}

int main(void) {
  setup_gtk4_config();
  gpu_env_setup();
  {
    char sname[64];
    session_init(session_from_cmdline(sname, sizeof(sname)) ? sname : NULL);
  }
  // En modo de pruebas (boot-tests) cada test arranca su propio servidor.
  detect_installer_mode();
  int testing = access("/system/boot-tests", F_OK) == 0 && !g_sess_ok;
  if (g_installer_mode)
    testing = 1; // sin Wayland ni pruebas: solo el instalador
  run_fstab();
  if (!testing)
    run_firstboot_packages();
  if (!testing)
    run_autofsck();
  run_boot_tests();
  pid_t term = spawn_terminal();
  pid_t wl = testing ? -1 : spawn_wayland();
  int wl_restarts = 0;

  for (;;) {
    int status = 0;
    // Sondeo (no bloqueante): ademas de reapear hijos, vigila la peticion de
    // cambio de sesion que deja aurora-switch en /run (una senal a PID 1 no es
    // fiable aqui: el kernel la trata con la accion por defecto).
    pid_t pid = waitpid(-1, &status, WNOHANG);
    {
      static struct timespec last;
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      long gap = (now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000;
      if (last.tv_sec && gap > 2000)
        fprintf(stderr, "init: bucle detenido %ld ms\n", gap);
      last = now;
    }
    if (pid == 0) {
      if (access("/run/aurora-session-request", F_OK) == 0)
        g_switch = 1;
      else {
        usleep(200000);
        continue;
      }
      pid = -1;
      errno = EINTR;
    }

    if (g_switch) {
      g_switch = 0;
      char want[64];
      read_switch_request(want, sizeof(want));
      if (want[0]) {
        fprintf(stderr, "init: cambio de sesion a '%s'\n", want);
        kill_session(term, wl);
        term = -1;
        wl = -1;
        session_init(want);
        wl_restarts = 0;
        term = spawn_terminal();
        wl = testing ? -1 : spawn_wayland();
      }
      if (pid < 0)
        continue;
    }
    if (pid < 0) {
      if (errno == EINTR)
        continue;
      if (errno == ECHILD) {
        sleep(1);
        continue;
      }
      perror("init: waitpid");
      sleep(1);
      continue;
    }

    if (pid == wl) {
      // El servidor Wayland nunca debe morir; si cae, se relanza (con tope).
      fprintf(stderr, "init: aurora-wayland termino (status=0x%x)\n", status);
      wl = -1;
      if (wl_restarts++ < 5) {
        sleep(RESPAWN_DELAY_SEC);
        wl = spawn_wayland();
      }
    } else if (pid == term) {
      // Decidir si respawnear:
      //
      //   Salida limpia (exit 0): el usuario cerró la ventana.
      //     No respawnear. El sistema sigue vivo, idle.
      //
      //   Salida con código != 0: crash del terminal.
      //     Respawnear, para que el usuario recupere el shell.
      //
      //   Muerto por señal: algo lo mató desde fuera (kill -9
      //   desde otra app, OOM, etc.). Tratamos como crash.
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr,
                "init: terminal closed cleanly, staying alive as reaper\n");
        term = -1;
      } else {
        fprintf(stderr,
                "init: terminal died abnormally (status=0x%x), respawning\n",
                status);
        sleep(RESPAWN_DELAY_SEC);
        term = spawn_terminal();
      }
    }
    // huérfanos reapeados por el kernel: nada más que hacer.
  }
}