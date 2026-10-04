// user/musl/apps/terminal/main.c
//
// Terminal 2D: matriz de celdas + parser ANSI completo + scrollback.

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

// Input buffering.
static uint8_t g_in_buf[64];
static int g_in_len = 0;

// [FIX #10b] Fase del cursor animado (ms dentro de un ciclo de 2000 ms).
static uint64_t g_cursor_phase = 0;

// ---------------------------------------------------------------------------
// [FIX #6] Decode de eventos MOUSE.
// Formato acordado con el compositor:
//   bits 0-7   : button (0=left, 1=middle, 2=right, 64=wheel_up, 65=wheel_down)
//   bit  8     : release (1 = soltar)
//   bit  9     : motion  (1 = movimiento con botón pulsado)
//   bits 16-18 : mods    (1=shift, 2=alt, 4=ctrl)
// ---------------------------------------------------------------------------
static inline int mouse_button(uint32_t d)  { return (int)(d & 0xFF); }
static inline int mouse_pressed(uint32_t d) { return ((d >> 8) & 1) == 0; }
static inline int mouse_motion(uint32_t d)  { return (int)((d >> 9) & 1); }
static inline int mouse_mods(uint32_t d)    { return (int)((d >> 16) & 0x7); }

// ---------------------------------------------------------------------------
// Detección de PgUp/PgDn.
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
// [FIX #6] Convierte evento de ratón a celda del terminal y actúa.
// ---------------------------------------------------------------------------
static void handle_mouse_event(const winsrv_event_t *ev) {
  int col = (ev->x - PAD_X) / g_cell_w;
  int row = (ev->y - PAD_Y) / g_cell_h;
  if (col < 0 || col >= COLS || row < 0 || row >= ROWS)
    return;

  int button  = mouse_button(ev->data);
  int pressed = mouse_pressed(ev->data);
  int motion  = mouse_motion(ev->data);
  int mods    = mouse_mods(ev->data);

  // Wheel: si el shell no pidió mouse tracking, scrollear scrollback local.
  if ((button == 64 || button == 65) && !g_term.mode_mouse) {
    int lines = 3;
    terminal_scroll(&g_term, button == 64 ? +lines : -lines);
    return;
  }

  if (!g_term.mode_mouse)
    return;

  // Wheel: pasar siempre al PTY (aunque sea release=1).
  // Motion sin botón: ignorar (no implementamos any-event tracking).
  if (motion && !pressed)
    return;

  terminal_mouse_event(&g_term, col, row, button, pressed, mods);
  flush_response();
}

// ---------------------------------------------------------------------------
// Procesa el input acumulado: scrollback, Home/End, o PTY.
// ---------------------------------------------------------------------------
static void process_input(const uint8_t *buf, int n) {
  // [FIX] En alternate screen no interceptamos NADA. PgUp/PgDn/Home/End
  // son para la app (top scrollea con ellos, vim los usa para moverse,
  // less también). Pasamos todo al PTY sin tocar el scrollback local.
  if (g_term.using_alternate) {
    if (g_master >= 0)
      write(g_master, buf, (size_t)n);
    return;
  }

  int delta = 0;
  if (match_scroll(buf, n, &delta, ROWS)) {
    terminal_scroll(&g_term, delta);
    return;
  }

  if (n == 3 && buf[0] == 0x1b && buf[1] == '[') {
    if (buf[2] == 'H') {
      if (g_term.history_count > 0) {
        terminal_scroll(&g_term, g_term.history_count);
        return;
      }
    }
    if (buf[2] == 'F') {
      if (terminal_is_scrolled(&g_term)) {
        terminal_scroll_to_bottom(&g_term);
        return;
      }
    }
  }

  g_cursor_phase = 0;

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
// [FIX #3] Bracketed paste (helper listo para Ctrl+Shift+V).
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

// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  const font_aa_t *font = font_system_mono();
  if (font) {
    g_font_h = font->height;
    g_cell_w = font->glyphs['M'].advance;
    if (g_cell_w <= 0)
      g_cell_w = 8;
    g_cell_h = g_font_h + 4;
  }

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

  // Fondo inicial: solo lo usamos para el primer blit. El gradiente se
  // aplica celda a celda en el render.
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

  if (!environ || !environ[0]) {
    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/", 1);
    setenv("TERM", "xterm-256color", 1);
    setenv("HOME", "/data", 1);
    setenv("USER", "root", 1);
    setenv("LOGNAME", "root", 1);
    setenv("SHELL", "/bin/sh", 1);
    setenv("PWD", "/", 1);
  }

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

  char *sh_argv[] = {"/bin/sh", NULL};
  spawn_fds_t fds = {slave, slave, slave};
  int child = spawn_args_fds("/bin/sh", sh_argv, 1, &fds);
  close(slave);
  if (child < 0)
    enter_dead_mode();
  g_child = child;

  int child_pgid = child;
  ioctl(master, TIOCSPGRP, &child_pgid);

  while (1) {
    // 1. Eventos de la ventana.
    winsrv_event_t ev;
    while (sys_win_poll_event(win, &ev, 0) > 0) {
      switch (ev.type) {
      case WINSRV_EV_TTY_INPUT:
        if (g_in_len < (int)sizeof(g_in_buf))
          g_in_buf[g_in_len++] = (uint8_t)ev.x;
        break;

      case WINSRV_EV_MOUSE:
        handle_mouse_event(&ev);
        break;

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

      case WINSRV_EV_RESIZE: {
        int new_cw = ev.x;
        int new_ch = ev.y;
        if (new_cw <= 2 * PAD_X || new_ch <= 2 * PAD_Y)
          break;

        int new_cols = (new_cw - 2 * PAD_X) / g_cell_w;
        int new_rows = (new_ch - 2 * PAD_Y) / g_cell_h;
        if (new_cols < 1) new_cols = 1;
        if (new_rows < 1) new_rows = 1;

        if (new_cols == g_term.primary.cols &&
            new_rows == g_term.primary.rows &&
            new_cw == g_cw && new_ch == g_ch)
          break;

        uint32_t *new_pixels = (uint32_t *)malloc(
            (size_t)new_cw * new_ch * sizeof(uint32_t));
        if (!new_pixels)
          break;

        const uint32_t k_bg = 0xFF000000u | 0x002B36u;
        for (int i = 0; i < new_cw * new_ch; i++)
          new_pixels[i] = k_bg;

        free(g_pixels);
        g_pixels = new_pixels;
        g_cw = new_cw;
        g_ch = new_ch;

        // Resize de la grid. Preserva el contenido visible top-left.
        terminal_resize(&g_term, new_cols, new_rows);

        // Forzar render inmediato (sin esperar al loop, que puede tardar
        // si el cursor está en fase hold y no marca dirty).
        terminal_render(&g_term, g_pixels, g_cw,
                        PAD_X, PAD_Y, g_cell_w, g_cell_h,
                        font_system_mono());
        terminal_clear_dirty(&g_term);

        // Blit del buffer COMPLETO. Cubre:
        //   1. Las celdas (con el contenido preservado).
        //   2. El padding PAD_X/PAD_Y, que el kernel rellenó con
        //      WIN11_SURFACE_CARD al hacer window_resize y que hay que
        //      sobrescribir con el bg del terminal.
        sys_win_blit(win, 0, 0, g_cw, g_ch, 0, 0, g_cw, g_pixels);

        // Avisar al shell. ioctl(TIOCSWINSZ) ya envía SIGWINCH al
        // foreground pgroup del PTY; no hace falta el kill explícito.
        // Si lo dejábamos, el shell recibía SIGWINCH dos veces y podía
        // imprimir el prompt dos veces.
        struct winsize ws2 = {(unsigned short)new_rows,
                              (unsigned short)new_cols, 0, 0};
        ioctl(g_master, TIOCSWINSZ, &ws2);
        break;
      }

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

    // 2. Input acumulado.
    if (g_in_len > 0) {
      process_input(g_in_buf, g_in_len);
      g_in_len = 0;
    }

    // 3. Drenar el PTY.
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

    // 4. [FIX #10b] Animación del cursor.
    //
    // Patrón "hold + fade corto", como kitty/xterm/GNOME Terminal:
    //   - 450 ms opaco (alpha=255)
    //   -  80 ms fade out (255 -> 0)
    //   - 450 ms invisible (alpha=0)
    //   -  80 ms fade in (0 -> 255)
    // Ciclo total: 1060 ms. El cursor está visible ~53% del tiempo,
    // pero las transiciones son rápidas (5 frames cada una) y no se
    // percibe "flotando" como con una onda triangular lenta.
    g_cursor_phase += 16;
    if (g_cursor_phase >= 1060)
      g_cursor_phase = 0;

    uint8_t alpha;
    if (g_cursor_phase < 450) {
      alpha = 255;
    } else if (g_cursor_phase < 530) {
      alpha = (uint8_t)(255 - ((g_cursor_phase - 450) * 255) / 80);
    } else if (g_cursor_phase < 980) {
      alpha = 0;
    } else {
      alpha = (uint8_t)(((g_cursor_phase - 980) * 255) / 80);
    }
    terminal_set_cursor_alpha(&g_term, alpha);

    // 5. Render.
    flush_to_window();

    // 6. Reap child.
    if (g_child > 0) {
      int st = 0;
      if (waitpid(g_child, &st, WNOHANG) == g_child)
        g_child = -1;
    }

    // 7. Esperar 16 ms.
    struct pollfd pfd = {g_master, POLLIN, 0};
    poll(&pfd, 1, 16);
  }
  return 0;
}