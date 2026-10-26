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

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
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
    // OpenGL/EGL por software (Mesa llvmpipe sobre wl_shm, sin DRM).
    "EGL_PLATFORM=wayland",
    "LIBGL_ALWAYS_SOFTWARE=1",
    "MESA_SHADER_CACHE_DISABLE=true",
    // GTK3 / Qt5: sin dconf ni bus de accesibilidad, datos en /usr/share y
    // configuracion de GTK en /usr/share/aurora/xdg (ver settings.ini).
    "GDK_BACKEND=wayland",
    "GTK_THEME=Adwaita",
    "GSETTINGS_BACKEND=memory",
    "GSETTINGS_SCHEMA_DIR=/usr/share/glib-2.0/schemas",
    "GDK_PIXBUF_MODULE_FILE=/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders.cache",
    "NO_AT_BRIDGE=1",
    "QT_PLUGIN_PATH=/usr/lib/x86_64-linux-gnu/qt5/plugins",
    "XDG_CONFIG_HOME=/tmp/.config",
    "XDG_CONFIG_DIRS=/usr/share/aurora/xdg",
    "XDG_CACHE_HOME=/tmp/.cache",
    "XDG_DATA_HOME=/tmp/.local/share",
    NULL,
};

static pid_t spawn_terminal(void) {
  pid_t pid = fork();
  if (pid < 0) {
    perror("init: fork");
    return -1;
  }
  if (pid == 0) {
    char *argv[] = {(char *)TERMINAL_PATH, NULL};
    execve(TERMINAL_PATH, argv, default_envp);
    perror("init: execve " TERMINAL_PATH);
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
    execve(WAYLAND_PATH, argv, default_envp);
    _exit(127);
  }
  return pid;
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
    if (pid > 0)
      waitpid(pid, NULL, 0);
  }
  fclose(f);
}

int main(void) {
  // En modo de pruebas (boot-tests) cada test arranca su propio servidor.
  int testing = access("/system/boot-tests", F_OK) == 0;
  run_boot_tests();
  pid_t term = spawn_terminal();
  pid_t wl = testing ? -1 : spawn_wayland();
  int wl_restarts = 0;

  for (;;) {
    int status = 0;
    pid_t pid = waitpid(-1, &status, 0);

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