// user/musl/apps/terminal/main.c
//
// Terminal 2D: matriz de celdas + parser ANSI completo + scrollback.
// El shell corre en el PTY, este proceso solo pinta.

#define syscall aurora_syscall
#define puts    aurora_puts_userlib
#define printf  aurora_printf_userlib

#include "../../../lib/env.h"
#include "../../../lib/file.h"
#include "../../../lib/font_system.h"
#include "../../../lib/malloc.h"
#include "../../../lib/process.h"
#include "../../../lib/string.h"
#include "../../../syscall.h"

#undef puts
#undef printf

#include "terminal.c"
#include "../../../lib/font_system.c"

extern void _exit(int status) __attribute__((noreturn));

// ---------------------------------------------------------------------------
// Geometría y estilo.
#define COLS 100
#define ROWS 30
#define PAD_X 12
#define PAD_Y 10
#define TERM_TITLEBAR_H 32

static int g_cell_w = 8;
static int g_cell_h = 20;
static int g_font_h = 14;

static uint32_t *g_pixels = NULL;
static int g_cw = 0;
static int g_ch = 0;
static int g_win = -1;
static int g_master = -1;
static int g_child = -1;

static terminal_t g_term;

// Input buffering: agrupamos los bytes que llegan en el mismo drain
// para poder detectar secuencias multi-byte (PgUp/PgDn) antes de
// decidir si van al PTY o son comandos de scrollback.
static uint8_t g_in_buf[64];
static int g_in_len = 0;

// ---------------------------------------------------------------------------
// Detección de las secuencias VT estándar de PgUp/PgDn.
//   PgUp = ESC [ 5 ~
//   PgDn = ESC [ 6 ~
//
// Devuelve 1 si es una de ellas, y rellena *out_delta con las líneas
// de scroll (positivo = hacia atrás). Si el compositor envía otras
// secuencias, ajustar aquí.
// ---------------------------------------------------------------------------
static int match_scroll(const uint8_t *b, int n, int *out_delta, int rows) {
  if (n == 4 && b[0] == 0x1b && b[1] == '[' && b[3] == '~') {
    if (b[2] == '5') { *out_delta = +(rows - 2); return 1; }
    if (b[2] == '6') { *out_delta = -(rows - 2); return 1; }
  }
  return 0;
}

// ---------------------------------------------------------------------------
static void flush_to_window(void) {
  if (!g_term.any_dirty)
    return;
  term_rect_t r = terminal_render(&g_term, g_pixels, g_cw,
                                  PAD_X, PAD_Y,
                                  g_cell_w, g_cell_h,
                                  font_system_mono());
  if (r.w <= 0 || r.h <= 0) {
    terminal_clear_dirty(&g_term);
    return;
  }
  sys_win_blit(g_win, r.x, r.y, r.w, r.h, r.x, r.y, g_cw, g_pixels);
  terminal_clear_dirty(&g_term);
}

static void flush_response(void) {
  if (g_term.response_len > 0 && g_master >= 0) {
    write(g_master, g_term.response, g_term.response_len);
    g_term.response_len = 0;
  }
}

// ---------------------------------------------------------------------------
// Procesa el input acumulado: scrollback, Home/End en scrollback, o PTY.
// ---------------------------------------------------------------------------
static void process_input(const uint8_t *buf, int n) {
  // PgUp / PgDn.
  int delta = 0;
  if (match_scroll(buf, n, &delta, ROWS)) {
    terminal_scroll(&g_term, delta);
    return;
  }

  // Home / End (ESC [ H  y  ESC [ F).
  if (n == 3 && buf[0] == 0x1b && buf[1] == '[') {
    // Home: ir al top del history SIEMPRE que haya history.
    // Si no hay history, se pasa al shell para que readline lo use
    // como "inicio de línea" (Ctrl+A también sirve).
    //
    // Antes esto solo se activaba si ya estabas scrolleado, lo cual
    // obligaba a pulsar PgUp primero. Ahora funciona directo.
    if (buf[2] == 'H') {
      if (g_term.history_count > 0) {
        terminal_scroll(&g_term, g_term.history_count);
        return;
      }
    }
    // End: volver al live view SIEMPRE que estés scrolleado.
    // Si no lo estás, se pasa al shell (readline End = fin de línea).
    if (buf[2] == 'F') {
      if (terminal_is_scrolled(&g_term)) {
        terminal_scroll_to_bottom(&g_term);
        return;
      }
    }
  }

  // Cualquier otro input va al PTY y devuelve al live view.
  if (g_master >= 0)
    write(g_master, buf, (size_t)n);
  terminal_scroll_to_bottom(&g_term);
}

// ---------------------------------------------------------------------------
static void enter_dead_mode(void) __attribute__((noreturn));

static void enter_dead_mode(void) {
  for (;;) {
    winsrv_event_t ev;
    if (sys_win_poll_event(g_win, &ev, 1) > 0) {
      if (ev.type == WINSRV_EV_CLOSE) {
        sys_win_destroy(g_win);
        free(g_pixels);
        _exit(0);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// [FIX #3] Bracketed paste.
//
// El shell puede activar el modo ?2004h para que el terminal le avise
// cuando el texto viene de un "paste" (no de teclado). Cuando está
// activo, el texto se envuelve en ESC[200~ ... ESC[201~, lo que evita
// que un paste accidental ejecute comandos línea a línea.
//
// Esta función NO se llama de momento: está lista para cuando
// implementes Ctrl+Shift+V con clipboard. El llamante debe leer el
// texto del clipboard y llamar aquí con el buffer.
//
// Marcada como `unused` para que GCC no se queje mientras no la uses.
static void __attribute__((unused))
term_paste(const char *text, size_t len) {
  if (!text || len == 0 || g_master < 0)
    return;
  if (g_term.mode_bracketed_paste) {
    write(g_master, "\033[200~", 6);
    write(g_master, text, len);
    write(g_master, "\033[201~", 6);
  } else {
    write(g_master, text, len);
  }
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  // --- Métricas reales de la fuente ---
  const font_aa_t *font = font_system_mono();
  if (font) {
    g_font_h = font->height;
    g_cell_w = font->glyphs['M'].advance;
    if (g_cell_w <= 0)
      g_cell_w = 8;
    g_cell_h = g_font_h + 4; // 4 px de leading
  }

  // --- Ventana dimensionada al contenido + padding + titlebar ---
  int win_w = COLS * g_cell_w + 2 * PAD_X;
  int win_h = ROWS * g_cell_h + 2 * PAD_Y;

  int win = sys_win_create(80, 60, win_w, win_h + TERM_TITLEBAR_H, "Terminal");
  if (win < 0)
    return 1;
  sys_win_set_icon(win, "/system/icons/terminal-window-dark.bmp");

  g_win = win;
  g_cw = win_w;
  g_ch = win_h;

  g_pixels = (uint32_t *)malloc(g_cw * g_ch * sizeof(uint32_t));
  if (!g_pixels) {
    sys_win_destroy(win);
    return 1;
  }

  const uint32_t k_bg = 0xFF000000u | 0x002B36u;
  for (int i = 0; i < g_cw * g_ch; i++)
    g_pixels[i] = k_bg;

  if (terminal_init(&g_term, COLS, ROWS) != 0) {
    free(g_pixels);
    sys_win_destroy(win);
    return 1;
  }

  sys_win_blit(win, 0, 0, g_cw, g_ch, 0, 0, g_cw, g_pixels);
  terminal_clear_dirty(&g_term);

  // --- ENV defaults ---
  if (!environ || !environ[0]) {
    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/", 1);
    setenv("TERM", "xterm-256color", 1);
    setenv("HOME", "/data", 1);
    setenv("USER", "root", 1);
    setenv("LOGNAME", "root", 1);
    setenv("SHELL", "/bin/sh", 1);
    setenv("PWD", "/", 1);
  }

  // --- PTY ---
  int master = open("/dev/ptmx", O_RDWR);
  if (master < 0)
    enter_dead_mode();
  g_master = master;

  int pty_n = -1;
  if (ioctl(master, TIOCGPTN, &pty_n) < 0)
    enter_dead_mode();

  int unlock = 0;
  ioctl(master, TIOCSPTLCK, &unlock);

  char slave_path[32];
  snprintf(slave_path, sizeof(slave_path), "/dev/pts/%d", pty_n);
  int slave = open(slave_path, O_RDWR);
  if (slave < 0)
    enter_dead_mode();

  struct winsize ws = {ROWS, COLS, 0, 0};
  ioctl(master, TIOCSWINSZ, &ws);

  // --- Spawn shell ---
  char *sh_argv[] = {"/bin/sh", NULL};
  spawn_fds_t fds = {slave, slave, slave};
  int child = spawn_args_fds("/bin/sh", sh_argv, 1, &fds);
  close(slave);
  if (child < 0)
    enter_dead_mode();
  g_child = child;

  int child_pgid = child;
  ioctl(master, TIOCSPGRP, &child_pgid);

  // --- Loop principal ---
  int blink_count = 0;

  while (1) {
    // 1. Eventos de la ventana.
    winsrv_event_t ev;
    while (sys_win_poll_event(win, &ev, 0) > 0) {
      switch (ev.type) {
      case WINSRV_EV_TTY_INPUT:
        if (g_in_len < (int)sizeof(g_in_buf))
          g_in_buf[g_in_len++] = (uint8_t)ev.x;
        break;

      // [FIX #4] Focus events. Si el shell ha activado ?1004h, se
      // envían ESC[I (focus in) / ESC[O (focus out) al PTY cuando la
      // ventana gana o pierde el foco. Los usan vim, emacs y algunas
      // TUIs para pausar timers o repaints.
      case WINSRV_EV_FOCUS:
        if (g_term.mode_focus_events && g_master >= 0) {
          const char seq[] = "\033[I";
          write(g_master, seq, 3);
        }
        break;
      case WINSRV_EV_BLUR:
        if (g_term.mode_focus_events && g_master >= 0) {
          const char seq[] = "\033[O";
          write(g_master, seq, 3);
        }
        break;

      case WINSRV_EV_CLOSE:
        if (g_child > 0) kill(g_child, SIGKILL);
        if (g_master >= 0) close(g_master);
        terminal_shutdown(&g_term);
        free(g_pixels);
        sys_win_destroy(win);
        _exit(0);

      default:
        break;
      }
    }

    // 2. Procesar input acumulado (scrollback, Home/End o PTY).
    if (g_in_len > 0) {
      process_input(g_in_buf, g_in_len);
      g_in_len = 0;
    }

    // 3. Drenar el PTY master.
    for (;;) {
      struct pollfd pfd = {g_master, POLLIN, 0};
      int pr = poll(&pfd, 1, 0);
      if (pr <= 0 || !(pfd.revents & POLLIN))
        break;
      char buf[512];
      int64_t n = read(g_master, buf, sizeof(buf));
      if (n <= 0)
        break;
      terminal_feed(&g_term, (const uint8_t *)buf, (size_t)n);
      flush_response();
    }

    // 4. Blink cada ~500 ms (32 iteraciones × 16 ms).
    blink_count++;
    if (blink_count >= 32) {
      blink_count = 0;
      terminal_blink_toggle(&g_term);
    }

    // 5. Render + blit del bbox dirty.
    flush_to_window();

    // 6. Reap child.
    if (g_child > 0) {
      int st = 0;
      if (waitpid(g_child, &st, WNOHANG) == g_child)
        g_child = -1;
    }

    // 7. Esperar 16 ms o hasta input del PTY.
    struct pollfd pfd = {g_master, POLLIN, 0};
    poll(&pfd, 1, 16);
  }
  return 0;
}