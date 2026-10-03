// user/musl/apps/init/main.c
//
// init (PID 1) de Aurora OS.
//
// Responsabilidades:
//   1. Lanzar el terminal gráfico (/apps/shell) como primer hijo.
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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TERMINAL_PATH "/apps/terminal"
#define RESPAWN_DELAY_SEC 1

static char *default_envp[] = {
    "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/",
    "TERM=linux",
    "HOME=/data",
    "USER=root",
    "LOGNAME=root",
    "SHELL=/bin/sh",
    "PWD=/",
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

int main(void) {
  pid_t term = spawn_terminal();

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

    if (pid == term) {
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