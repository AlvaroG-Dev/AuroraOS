// user/apps/shell/main.c
//
// Terminal gráfico de Aurora. Abre un PTY, lanza /bin/sh con el slave
// como stdio, y reenvía todo el I/O. No tiene lógica de shell propia.

#include "../../lib/env.h"
#include "../../lib/file.h"
#include "../../lib/font_system.h"
#include "../../lib/malloc.h"
#include "../../lib/process.h"
#include "../../lib/string.h"
#include "../../syscall.h"

#define WIN_W 820
#define WIN_H 600
#define TITLEBAR_HEIGHT 32

#define COLS 100
#define ROWS_VISIBLE 31
#define ROWS_BUFFER 512
#define LINE_HEIGHT 18
#define CHAR_WIDTH 8

// ---------------------------------------------------------------------------
// Buffer de líneas + atributos
// ---------------------------------------------------------------------------
typedef struct {
  char chars[COLS];
  uint8_t attrs[COLS];
  uint16_t len;
} line_t;

typedef struct {
  line_t lines[ROWS_BUFFER];
  int head;
  int count;
  int cursor_col;
  int scroll_offset;
} console_buf_t;

static console_buf_t g_buf;

static uint32_t *g_pixels = NULL;
static int g_cw = 0;
static int g_ch = 0;
static int g_win = -1;
static int g_needs_redraw = 0;

static int g_ansi_state = 0;
static int g_ansi_params[8];
static int g_ansi_nparams = 0;
static int g_ansi_cur = 0;
static int g_ansi_has_cur = 0;

static uint8_t g_cur_fg = 7;
static uint8_t g_cur_bold = 0;
static int g_ansi_at_home = 0;

static const uint32_t ansi_palette[16] = {
    0xFF073642, 0xFFDC322F, 0xFF859900, 0xFFB58900, 0xFF268BD2, 0xFFD33682,
    0xFF2AA198, 0xFFEEE8D5, 0xFF002B36, 0xFFCB4B16, 0xFF586E75, 0xFF657B83,
    0xFF839496, 0xFF6C71C4, 0xFF93A1A1, 0xFFFDF6E3,
};

static uint32_t ansi_color(uint8_t attr) {
  int fg = attr & 0x0F;
  int bold = (attr >> 7) & 1;
  if (bold && fg < 8)
    fg += 8;
  return ansi_palette[fg & 0x0F];
}

static void ansi_reset(void) {
  g_ansi_state = 0;
  g_ansi_nparams = 0;
  g_ansi_cur = 0;
  g_ansi_has_cur = 0;
}

// ---------------------------------------------------------------------------
// Región sucia
// ---------------------------------------------------------------------------
#define DIRTY_EMPTY_X0 0x7FFFFFFF
#define DIRTY_EMPTY_Y0 0x7FFFFFFF

static int g_dirty_x0 = DIRTY_EMPTY_X0;
static int g_dirty_y0 = DIRTY_EMPTY_Y0;
static int g_dirty_x1 = 0;
static int g_dirty_y1 = 0;

static void dirty_reset(void) {
  g_dirty_x0 = DIRTY_EMPTY_X0;
  g_dirty_y0 = DIRTY_EMPTY_Y0;
  g_dirty_x1 = 0;
  g_dirty_y1 = 0;
}
static int dirty_empty(void) {
  return g_dirty_x0 >= g_dirty_x1 || g_dirty_y0 >= g_dirty_y1;
}
static void dirty_add(int x, int y, int w, int h) {
  if (x < g_dirty_x0)
    g_dirty_x0 = x;
  if (y < g_dirty_y0)
    g_dirty_y0 = y;
  if (x + w > g_dirty_x1)
    g_dirty_x1 = x + w;
  if (y + h > g_dirty_y1)
    g_dirty_y1 = y + h;
}

static int cursor_row_visible(void) {
  int bottom = g_buf.count - 1 - g_buf.scroll_offset;
  if (bottom < 0)
    bottom = 0;
  int top = bottom - ROWS_VISIBLE + 1;
  if (top < 0)
    top = 0;
  int logical = g_buf.count - 1;
  if (logical < top || logical > bottom)
    return -1;
  return logical - top;
}

static void mark_cursor_row_dirty(void) {
  int r = cursor_row_visible();
  if (r < 0)
    return;
  dirty_add(0, r * LINE_HEIGHT, COLS * CHAR_WIDTH + 8, LINE_HEIGHT);
}

// ---------------------------------------------------------------------------
// Buffer ops
// ---------------------------------------------------------------------------
static void buf_new_line(void) {
  mark_cursor_row_dirty();
  int next;
  if (g_buf.count < ROWS_BUFFER) {
    next = g_buf.count;
    g_buf.count++;
  } else {
    next = g_buf.head;
    g_buf.head = (g_buf.head + 1) % ROWS_BUFFER;
    dirty_add(0, 0, COLS * CHAR_WIDTH + 8, ROWS_VISIBLE * LINE_HEIGHT);
  }
  memset(&g_buf.lines[next], 0, sizeof(line_t));
  g_buf.cursor_col = 0;
  mark_cursor_row_dirty();
}

static int buf_line_idx(int logical) {
  if (g_buf.count < ROWS_BUFFER)
    return logical;
  return (g_buf.head + logical) % ROWS_BUFFER;
}

static void buf_init(void) {
  memset(&g_buf, 0, sizeof(g_buf));
  g_buf.cursor_col = 0;
  g_buf.scroll_offset = 0;
  g_cur_fg = 7;
  g_cur_bold = 0;
  g_ansi_at_home = 0;
  ansi_reset();
  dirty_add(0, 0, g_cw, g_ch);
  buf_new_line();
}

static void buf_putchar(char c) {
  if (g_ansi_state == 0) {
    if (c == 0x1B) {
      g_ansi_state = 1;
      return;
    }
  } else if (g_ansi_state == 1) {
    if (c == '[') {
      g_ansi_state = 2;
      g_ansi_nparams = 0;
      g_ansi_cur = 0;
      g_ansi_has_cur = 0;
      return;
    }
    if (c == 'c') {
      buf_init();
      ansi_reset();
      return;
    }
    ansi_reset();
    g_ansi_at_home = 0;
    return;
  } else {
    if (c >= '0' && c <= '9') {
      g_ansi_cur = g_ansi_cur * 10 + (c - '0');
      g_ansi_has_cur = 1;
      return;
    }
    if (c == ';') {
      if (g_ansi_nparams < 8)
        g_ansi_params[g_ansi_nparams++] = g_ansi_has_cur ? g_ansi_cur : 0;
      g_ansi_cur = 0;
      g_ansi_has_cur = 0;
      return;
    }
    if (c == '?') {
      g_ansi_has_cur = 0;
      return;
    }

    if (g_ansi_nparams < 8)
      g_ansi_params[g_ansi_nparams++] = g_ansi_has_cur ? g_ansi_cur : 0;

    int idx = buf_line_idx(g_buf.count - 1);
    line_t *l = &g_buf.lines[idx];

    switch (c) {
    case 'D': {
      int n = g_ansi_params[0];
      if (n < 1)
        n = 1;
      g_buf.cursor_col = (g_buf.cursor_col >= n) ? g_buf.cursor_col - n : 0;
      mark_cursor_row_dirty();
      break;
    }
    case 'C': {
      int n = g_ansi_params[0];
      if (n < 1)
        n = 1;
      g_buf.cursor_col += n;
      if (g_buf.cursor_col > (int)l->len)
        g_buf.cursor_col = (int)l->len;
      mark_cursor_row_dirty();
      break;
    }
    case 'G': {
      int n = g_ansi_params[0];
      if (n < 1)
        n = 1;
      g_buf.cursor_col = n - 1;
      if (g_buf.cursor_col > (int)l->len)
        g_buf.cursor_col = (int)l->len;
      mark_cursor_row_dirty();
      break;
    }
    case 'H':
      g_buf.cursor_col = 0;
      mark_cursor_row_dirty();
      break;
    case 'K': {
      int n = g_ansi_params[0];
      if (n == 0) {
        l->len = (uint16_t)g_buf.cursor_col;
      } else if (n == 1) {
        for (int i = 0; i < g_buf.cursor_col && i < COLS; i++) {
          l->chars[i] = ' ';
          l->attrs[i] = (uint8_t)((g_cur_fg & 0x0F) | (g_cur_bold ? 0x80 : 0));
        }
      } else if (n == 2) {
        l->len = 0;
        g_buf.cursor_col = 0;
      }
      mark_cursor_row_dirty();
      break;
    }
    case 'J': {
      int n = g_ansi_params[0];
      int full = (n == 2) || (n == 3) || (n == 0 && g_ansi_at_home);
      if (full) {
        buf_init();
      } else if (n == 1) {
        for (int i = 0; i < g_buf.cursor_col && i < COLS; i++) {
          l->chars[i] = ' ';
          l->attrs[i] = (uint8_t)((g_cur_fg & 0x0F) | (g_cur_bold ? 0x80 : 0));
        }
        mark_cursor_row_dirty();
      } else {
        l->len = (uint16_t)g_buf.cursor_col;
        mark_cursor_row_dirty();
      }
      break;
    }
    case 'P': {
      int n = g_ansi_params[0];
      if (n < 1)
        n = 1;
      int start = g_buf.cursor_col;
      int end = start + n;
      if (end > (int)l->len)
        end = (int)l->len;
      int move = l->len - end;
      for (int i = 0; i < move; i++) {
        l->chars[start + i] = l->chars[end + i];
        l->attrs[start + i] = l->attrs[end + i];
      }
      l->len -= (end - start);
      mark_cursor_row_dirty();
      break;
    }
    case 'm':
      for (int i = 0; i < g_ansi_nparams; i++) {
        int p = g_ansi_params[i];
        if (p == 0) {
          g_cur_fg = 7;
          g_cur_bold = 0;
        } else if (p == 1)
          g_cur_bold = 1;
        else if (p == 22)
          g_cur_bold = 0;
        else if (p >= 30 && p <= 37) {
          g_cur_fg = (uint8_t)(p - 30);
          g_cur_bold = 0;
        } else if (p == 39) {
          g_cur_fg = 7;
          g_cur_bold = 0;
        } else if (p >= 90 && p <= 97) {
          g_cur_fg = (uint8_t)(8 + (p - 90));
          g_cur_bold = 0;
        }
      }
      break;
    case 'h':
    case 'l':
    case '~':
    case 'A':
    case 'B':
      break;
    default:
      break;
    }

    g_ansi_at_home = (c == 'H') ? 1 : 0;
    ansi_reset();
    return;
  }

  g_ansi_at_home = 0;
  mark_cursor_row_dirty();
  g_buf.scroll_offset = 0;

  if (c == '\n') {
    buf_new_line();
    return;
  }
  if (c == '\r') {
    g_buf.cursor_col = 0;
    mark_cursor_row_dirty();
    return;
  }
  if (c == '\b') {
    if (g_buf.cursor_col > 0)
      g_buf.cursor_col--;
    mark_cursor_row_dirty();
    return;
  }
  if (c < 0x20 || c >= 0x7F)
    return;

  int idx = buf_line_idx(g_buf.count - 1);
  line_t *l = &g_buf.lines[idx];
  if (g_buf.cursor_col >= COLS - 1) {
    buf_new_line();
    idx = buf_line_idx(g_buf.count - 1);
    l = &g_buf.lines[idx];
  }
  l->chars[g_buf.cursor_col] = c;
  l->attrs[g_buf.cursor_col] =
      (uint8_t)((g_cur_fg & 0x0F) | (g_cur_bold ? 0x80 : 0));
  if (g_buf.cursor_col + 1 > (int)l->len)
    l->len = (uint16_t)(g_buf.cursor_col + 1);
  g_buf.cursor_col++;
  mark_cursor_row_dirty();
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
static const font_aa_t *g_font = NULL;

static void draw_char(uint32_t *pixels, int cw, int ch, int x, int y,
                      unsigned char c, uint32_t color) {
  if (!g_font)
    g_font = font_system_mono();
  if (!g_font || c >= 128)
    return;

  const glyph_aa_t *g = &g_font->glyphs[c];
  if (!g->bitmap || g->width == 0 || g->height == 0)
    return;

  uint32_t base_a = (color >> 24) & 0xFF;
  uint32_t sr = (color >> 16) & 0xFF;
  uint32_t sg = (color >> 8) & 0xFF;
  uint32_t sb = color & 0xFF;

  int dx = x + g->bearing_x;
  int dy = y + (g_font->height - g->bearing_y);

  for (int gy = 0; gy < g->height; gy++) {
    int py = dy + gy;
    if (py < 0 || py >= ch)
      continue;
    const uint8_t *row = &g->bitmap[gy * g->width];
    uint32_t *drow = &pixels[py * cw];
    for (int gx = 0; gx < g->width; gx++) {
      int px = dx + gx;
      if (px < 0 || px >= cw)
        continue;
      uint8_t a = row[gx];
      if (a == 0)
        continue;
      uint32_t sa = (base_a * a) / 255;
      if (sa == 0)
        continue;
      uint32_t dst = drow[px];
      uint32_t inv = 255 - sa;
      uint32_t dr = (dst >> 16) & 0xFF;
      uint32_t dg = (dst >> 8) & 0xFF;
      uint32_t db = dst & 0xFF;
      uint32_t r = (sr * sa + dr * inv) / 255;
      uint32_t gg = (sg * sa + dg * inv) / 255;
      uint32_t b = (sb * sa + db * inv) / 255;
      drow[px] = 0xFF000000u | (r << 16) | (gg << 8) | b;
    }
  }
}

static void render_region(uint32_t *pixels, int cw, int ch, int rx0, int ry0,
                          int rw, int rh) {
  if (rx0 < 0) {
    rw += rx0;
    rx0 = 0;
  }
  if (ry0 < 0) {
    rh += ry0;
    ry0 = 0;
  }
  if (rx0 + rw > cw)
    rw = cw - rx0;
  if (ry0 + rh > ch)
    rh = ch - ry0;
  if (rw <= 0 || rh <= 0)
    return;

  for (int y = ry0; y < ry0 + rh; y++) {
    uint32_t *row = &pixels[y * cw + rx0];
    for (int x = 0; x < rw; x++)
      row[x] = 0xFF000000;
  }

  if (g_buf.count == 0)
    return;

  int bottom = g_buf.count - 1 - g_buf.scroll_offset;
  if (bottom < 0)
    bottom = 0;
  int top = bottom - ROWS_VISIBLE + 1;
  if (top < 0)
    top = 0;

  for (int i = 0; i < ROWS_VISIBLE; i++) {
    int logical = top + i;
    if (logical > bottom)
      break;
    int idx = buf_line_idx(logical);
    line_t *l = &g_buf.lines[idx];
    int y = i * LINE_HEIGHT + 2;
    if (y + 8 < ry0 || y > ry0 + rh)
      continue;

    for (int k = 0; k < l->len; k++) {
      int x = k * CHAR_WIDTH + 4;
      if (x + 16 < rx0 || x > rx0 + rw)
        continue;
      draw_char(pixels, cw, ch, x, y, (unsigned char)l->chars[k],
                ansi_color(l->attrs[k]));
    }
  }

  if (g_buf.scroll_offset == 0) {
    int cursor_row_in_view = bottom - top;
    if (cursor_row_in_view >= 0 && cursor_row_in_view < ROWS_VISIBLE) {
      int cursor_x = 4 + g_buf.cursor_col * CHAR_WIDTH;
      int cursor_y = cursor_row_in_view * LINE_HEIGHT + 2 + 14 + 1;
      if (cursor_y + 2 > ry0 && cursor_y < ry0 + rh &&
          cursor_x + CHAR_WIDTH > rx0 && cursor_x < rx0 + rw) {
        for (int dy = 0; dy < 2; dy++) {
          for (int dx = 0; dx < CHAR_WIDTH; dx++) {
            int px = cursor_x + dx;
            int py = cursor_y + dy;
            if (px >= 0 && px < cw && py >= 0 && py < ch)
              pixels[py * cw + px] = 0xFFFFFFFF;
          }
        }
      }
    }
  }
}

static void terminal_render(void) {
  if (!g_needs_redraw || !g_pixels)
    return;
  if (dirty_empty()) {
    g_needs_redraw = 0;
    return;
  }

  int rx0 = g_dirty_x0, ry0 = g_dirty_y0;
  int rx1 = g_dirty_x1, ry1 = g_dirty_y1;
  rx0 = (rx0 / CHAR_WIDTH) * CHAR_WIDTH - CHAR_WIDTH;
  ry0 = (ry0 / LINE_HEIGHT) * LINE_HEIGHT - LINE_HEIGHT;
  rx1 = ((rx1 + CHAR_WIDTH - 1) / CHAR_WIDTH) * CHAR_WIDTH + CHAR_WIDTH;
  ry1 = ((ry1 + LINE_HEIGHT - 1) / LINE_HEIGHT) * LINE_HEIGHT + LINE_HEIGHT;
  if (rx0 < 0)
    rx0 = 0;
  if (ry0 < 0)
    ry0 = 0;
  if (rx1 > g_cw)
    rx1 = g_cw;
  if (ry1 > g_ch)
    ry1 = g_ch;

  int rw = rx1 - rx0, rh = ry1 - ry0;
  if (rw <= 0 || rh <= 0) {
    g_needs_redraw = 0;
    dirty_reset();
    return;
  }

  render_region(g_pixels, g_cw, g_ch, rx0, ry0, rw, rh);
  sys_win_blit(g_win, rx0, ry0, rw, rh, rx0, ry0, g_cw, g_pixels);
  g_needs_redraw = 0;
  dirty_reset();
}

// ---------------------------------------------------------------------------
// Pantalla inicial + modo "caja negra"
// ---------------------------------------------------------------------------
static void paint_blank_screen(void) {
  for (int i = 0; i < g_cw * g_ch; i++)
    g_pixels[i] = 0xFF000000;
  dirty_add(0, 0, g_cw, g_ch);
  {
    int rx0 = 0, ry0 = 0, rx1 = g_cw, ry1 = g_ch;
    render_region(g_pixels, g_cw, g_ch, rx0, ry0, rx1 - rx0, ry1 - ry0);
    sys_win_blit(g_win, rx0, ry0, rx1 - rx0, ry1 - ry0, rx0, ry0, g_cw,
                 g_pixels);
  }
  dirty_reset();
}

// La ventana queda en negro. Solo responde a CLOSE. Es lo que el
// usuario ve si el PTY o el spawn del shell fallan.
static void enter_dead_mode(void) {
  for (;;) {
    winsrv_event_t ev;
    if (sys_win_poll_event(g_win, &ev, 1) > 0) {
      if (ev.type == WINSRV_EV_CLOSE) {
        sys_win_destroy(g_win);
        free(g_pixels);
        sys_exit(0);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  int win = sys_win_create(80, 60, WIN_W, WIN_H, "Terminal");
  if (win < 0)
    return 1;

  sys_win_set_icon(win, "/system/icons/terminal-window-dark.bmp");

  g_cw = WIN_W;
  g_ch = WIN_H - TITLEBAR_HEIGHT;
  g_win = win;
  g_pixels = (uint32_t *)malloc(g_cw * g_ch * sizeof(uint32_t));
  if (!g_pixels) {
    sys_win_destroy(win);
    return 1;
  }
  paint_blank_screen();

  // [ENV] defaults
  if (!environ || !environ[0]) {
    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/", 1);
    setenv("TERM", "linux", 1);
    setenv("HOME", "/data", 1);
    setenv("USER", "root", 1);
    setenv("LOGNAME", "root", 1);
    setenv("SHELL", "/bin/sh", 1);
    setenv("PWD", "/", 1);
  }

  buf_init();

  // ---- Abrir el PTY ----
  sys_print("T0");
  int master = open("/dev/ptmx", O_RDWR);
  if (master < 0)
    enter_dead_mode();

  sys_print("T1");
  int pty_n = -1;
  if (ioctl(master, TIOCGPTN, &pty_n) < 0) {
    close(master);
    enter_dead_mode();
  }

  // Linux deja el slave bloqueado por defecto y hay que desbloquearlo
  // explícitamente. Nuestra versión tiene slave_locked = 0 inicial, así
  // que el ioctl es no-op pero lo dejamos por compatibilidad.
  int unlock = 0;
  sys_print("T2");
  ioctl(master, TIOCSPTLCK, &unlock);
  sys_print("T3");

  char slave_path[32];
  snprintf(slave_path, sizeof(slave_path), "/dev/pts/%d", pty_n);
  int slave = open(slave_path, O_RDWR);
  if (slave < 0) {
    close(master);
    enter_dead_mode();
  }

  sys_print("T4");
  // Ajustar winsize del slave (proxy a través del master).
  struct winsize ws = {ROWS_VISIBLE, COLS, 0, 0};
  ioctl(master, TIOCSWINSZ, &ws);

  sys_print("T5");
  // ---- Spawn /bin/sh con el slave como stdio ----
  char *sh_argv[] = {"/bin/sh", NULL};
  spawn_fds_t fds = {slave, slave, slave};
  int child = spawn_args_fds("/bin/sh", sh_argv, 1, &fds);
  close(slave);
  if (child < 0) {
    close(master);
    enter_dead_mode();
  }

  sys_print("T6");
  // [JOB] Dar el terminal al shell (como tcsetpgrp). A partir de aquí,
  // Ctrl+C/Ctrl+Z se envían al pgrp del shell, no al terminal app.
  int child_pgid = child;
  ioctl(master, TIOCSPGRP, &child_pgid);

  sys_print("T7");
  // ---- Loop principal ----
  while (1) {
    int got = 0;
    winsrv_event_t ev;
    while (sys_win_poll_event(win, &ev, 0) > 0) {
      got = 1;
      switch (ev.type) {
      case WINSRV_EV_TTY_INPUT: {
        char c = (char)ev.x;
        write(master, &c, 1);
        break;
      }
      case WINSRV_EV_CLOSE:
        if (child > 0)
          kill(child, SIGKILL);
        close(master);
        sys_win_destroy(win);
        free(g_pixels);
        sys_exit(0);
      default:
        break;
      }
    }

    // Leer output del master (no bloqueante).
    for (;;) {
      struct pollfd pfd = {master, POLLIN, 0};
      int pr = poll(&pfd, 1, 0);
      if (pr <= 0 || !(pfd.revents & POLLIN))
        break;
      char buf[256];
      int64_t n = read(master, buf, sizeof(buf));
      if (n <= 0)
        break;
      for (int i = 0; i < n; i++)
        buf_putchar(buf[i]);
      got = 1;
    }

    if (got) {
      // Marcar todo sucio por seguridad. Podríamos afinar, pero el
      // blit completo es rápido y evita perder pintadas.
      dirty_add(0, 0, g_cw, g_ch);
      g_needs_redraw = 1;
    }

    terminal_render();

    if (child > 0) {
      int st = 0;
      if (waitpid(child, &st, WNOHANG) == child)
        child = -1;
    }

    // Esperar un frame. Con el fix del kernel (poll_wq), esta llamada
    // vuelve inmediatamente cuando el shell escribe al slave. Sin él,
    // sigue funcionando con hasta 16 ms de latencia.
    //
    // No usamos sys_yield() aquí porque eso implica una vuelta completa
    // por idle (con `sti; hlt`) que añade ~10 ms de latencia, y en la
    // ventana entre "el shell escribe" y "el terminal lee" el usuario
    // puede pulsar Enter, causando que dos prompts se rendericen juntos.
    struct pollfd pfd = {master, POLLIN, 0};
    poll(&pfd, 1, 16);
  }
  return 0;
}