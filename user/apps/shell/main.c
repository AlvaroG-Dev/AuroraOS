// user/apps/shell/main.c
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

#define LINE_MAX 256
#define MAX_ARGS 16

// ---------------------------------------------------------------------------
// Buffer de líneas (con atributos por carácter)
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
static char g_input[LINE_MAX];
static int g_input_len = 0;

static char g_cwd[256] = "/";

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

static void refresh_cwd(void) {
  char tmp[256];
  if (getcwd(tmp, sizeof(tmp)) > 0) {
    size_t n = strlen(tmp);
    if (n < sizeof(g_cwd))
      memcpy(g_cwd, tmp, n + 1);
  }
}

// ---------------------------------------------------------------------------
// [TTY] Vaciar el buffer de entrada del terminal.
//
// El shell nativo NO lee del tty (consume TTY_INPUT). Pero el TTY
// acumula todo lo tecleado en su ring buffer. Sin este flush, cuando
// el shell spawnea un hijo (busybox, cat, ...), el hijo hace read(0)
// y recibe TODOS los comandos tecleados en el prompt nativo desde el
// arranque — por eso "busybox sh" arrancaba otro "busybox sh" desde
// dentro y aparecían dos prompts.
//
// Se llama ANTES de spawnear (para limpiar) y DESPUÉS de que el hijo
// termine (por si quedó algún byte del eco del prompt).
// ---------------------------------------------------------------------------
static void tty_flush(void) {
  // ioctl(0, TCFLSH, TCIFLUSH). TCFLSH=0x540B, TCIFLUSH=0.
  syscall(SYS_IOCTL, 0, 0x540B, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------
#define MAX_PIPELINE 8

typedef struct {
  char *argv[MAX_ARGS];
  int argc;
  char *in_file;
  char *out_file;
  int append;
} pipeline_stage_t;

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
      if (g_buf.cursor_col >= n)
        g_buf.cursor_col -= n;
      else
        g_buf.cursor_col = 0;
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
      int is_full_clear = (n == 2) || (n == 3) || (n == 0 && g_ansi_at_home);
      if (is_full_clear) {
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
        } else if (p == 1) {
          g_cur_bold = 1;
        } else if (p == 22) {
          g_cur_bold = 0;
        } else if (p >= 30 && p <= 37) {
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
    // [FIX] Solo mover el cursor. La semántica de "\b \b" que emite el
    // TTY como eco se compone así: \b retrocede, ' ' sobreescribe con
    // espacio, \b retrocede otra vez. Si además hiciéramos trim aquí,
    // romperíamos el patrón del eco y perderíamos caracteres.
    if (g_buf.cursor_col > 0) {
      g_buf.cursor_col--;
    }
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

static void buf_puts(const char *s) {
  while (*s)
    buf_putchar(*s++);
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

static void shell_render(void) {
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
// Shell embebido
// ---------------------------------------------------------------------------
static void print_prompt(void) {
  g_cur_fg = 7;
  g_cur_bold = 0;
  buf_puts("aurora:");
  buf_puts(g_cwd);
  buf_puts("> ");
}

static void cmd_help(void) {
  puts("Comandos built-in:");
  puts("  help                    - esta ayuda");
  puts("  cd <dir>                - cambia el directorio");
  puts("  clear                   - limpia la consola");
  puts("  exit                    - salir");
  puts("");
  puts("Operadores:");
  puts("  cmd arg | cmd arg       - pipe");
  puts("  cmd < file              - stdin desde archivo");
  puts("  cmd > file              - stdout a archivo (truncar)");
  puts("  cmd >> file             - stdout a archivo (append)");
  puts("");
  puts("Cmd externos: ls cat mkdir rm cp mv kill echo spawn");
  puts("Ctrl+C envía SIGINT al pipeline en ejecución.");
}

static void cmd_cd(const char *arg) {
  const char *target = (arg && arg[0]) ? arg : getenv("HOME");
  if (!target)
    target = "/";
  int rc = sys_chdir(target);
  if (rc != 0) {
    printf("cd: no se pudo cambiar a '%s' (%d)\n", target, rc);
    return;
  }
  char tmp[256];
  if (getcwd(tmp, sizeof(tmp)) > 0)
    setenv("PWD", tmp, 1);
}

static void cmd_clear(void) { buf_init(); }

static int parse_pipeline(char *line, pipeline_stage_t *stages,
                          int max_stages) {
  int n = 0;
  char *p = line;
  while (*p && n < max_stages) {
    while (*p == ' ' || *p == '\t')
      p++;
    if (!*p)
      break;
    pipeline_stage_t *s = &stages[n];
    s->argc = 0;
    s->in_file = NULL;
    s->out_file = NULL;
    s->append = 0;
    while (*p && *p != '|') {
      while (*p == ' ' || *p == '\t')
        p++;
      if (!*p || *p == '|')
        break;
      if (*p == '<') {
        p++;
        while (*p == ' ' || *p == '\t')
          p++;
        char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '|')
          p++;
        if (*p) {
          *p = '\0';
          p++;
        }
        s->in_file = start;
        continue;
      }
      if (*p == '>') {
        p++;
        s->append = 0;
        if (*p == '>') {
          s->append = 1;
          p++;
        }
        while (*p == ' ' || *p == '\t')
          p++;
        char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '|')
          p++;
        if (*p) {
          *p = '\0';
          p++;
        }
        s->out_file = start;
        continue;
      }
      char *start = p;
      while (*p && *p != ' ' && *p != '\t' && *p != '|')
        p++;
      if (*p) {
        *p = '\0';
        p++;
      }
      if (s->argc < MAX_ARGS)
        s->argv[s->argc++] = start;
    }
    if (*p == '|')
      p++;
    if (s->argc > 0 || s->in_file || s->out_file)
      n++;
  }
  return n;
}

static int open_for_write(const char *path, int append) {
  if (!append) {
    unlink(path);
    create(path);
  }
  int fd = open(path, O_WRONLY);
  if (fd < 0) {
    if (create(path) != 0)
      return -1;
    fd = open(path, O_WRONLY);
    if (fd < 0)
      return -1;
  }
  if (append)
    lseek(fd, 0, SEEK_END);
  return fd;
}

static int spawn_stage(pipeline_stage_t *s, const spawn_fds_t *fds) {
  int pid = spawn_args_fds(s->argv[0], s->argv, s->argc, fds);
  if (pid >= 0)
    return pid;

  char buf[160];
  if (strlen(s->argv[0]) + 14 < sizeof(buf)) {
    strcpy(buf, "/initrd/apps/");
    strcpy(buf + 13, s->argv[0]);
    pid = spawn_args_fds(buf, s->argv, s->argc, fds);
    if (pid >= 0)
      return pid;
  }
  if (strlen(s->argv[0]) + 6 < sizeof(buf)) {
    size_t n = 0;
    buf[n++] = '/';
    for (int i = 0; s->argv[0][i] && n < sizeof(buf) - 6; i++) {
      char c = s->argv[0][i];
      if (c >= 'a' && c <= 'z')
        c -= 32;
      buf[n++] = c;
    }
    buf[n++] = '.';
    buf[n++] = 'E';
    buf[n++] = 'L';
    buf[n++] = 'F';
    buf[n] = '\0';
    pid = spawn_args_fds(buf, s->argv, s->argc, fds);
  }
  return pid;
}

static void run_pipeline(pipeline_stage_t *stages, int n, int win_id) {
  // [TTY] Limpiar el buffer de entrada antes de spawnear. Si no, el
  // primer read(0) del hijo recibiría todo lo tecleado en el shell
  // nativo desde el arranque.
  tty_flush();

  int pipefds[MAX_PIPELINE - 1][2];
  int n_pipes = (n > 0) ? (n - 1) : 0;
  for (int i = 0; i < n_pipes; i++) {
    if (pipe(pipefds[i]) != 0) {
      puts("pipe: fallo");
      shell_render();
      return;
    }
  }
  int pids[MAX_PIPELINE];
  for (int i = 0; i < n; i++)
    pids[i] = -1;
  int spawned = 0;

  for (int i = 0; i < n; i++) {
    int in_fd = -1, out_fd = -1;
    if (i == 0) {
      if (stages[i].in_file) {
        in_fd = open(stages[i].in_file, O_RDONLY);
        if (in_fd < 0) {
          printf("no se puede abrir %s\n", stages[i].in_file);
          goto fail;
        }
      }
    } else {
      in_fd = pipefds[i - 1][0];
    }
    if (i == n - 1) {
      if (stages[i].out_file) {
        out_fd = open_for_write(stages[i].out_file, stages[i].append);
        if (out_fd < 0) {
          printf("no se puede escribir %s\n", stages[i].out_file);
          goto fail;
        }
      }
    } else {
      out_fd = pipefds[i][1];
    }
    spawn_fds_t sf = {in_fd, out_fd, out_fd};
    int pid = spawn_stage(&stages[i], &sf);
    if (pid < 0) {
      printf("console: no se pudo ejecutar %s\n", stages[i].argv[0]);
      goto fail;
    }
    pids[i] = pid;
    spawned++;
    if (i == 0 && in_fd != -1)
      close(in_fd);
    if (i == n - 1 && out_fd != -1)
      close(out_fd);
  }
  for (int i = 0; i < n_pipes; i++) {
    close(pipefds[i][0]);
    close(pipefds[i][1]);
  }

  int remaining = spawned;
  while (remaining > 0) {
    for (int i = 0; i < n; i++) {
      if (pids[i] < 0)
        continue;
      int status = 0;
      int r = waitpid(pids[i], &status, WNOHANG);
      if (r == pids[i]) {
        pids[i] = -1;
        remaining--;
      }
    }
    winsrv_event_t ev;
    while (sys_win_poll_event(win_id, &ev, 0) > 0) {
      if (ev.type == WINSRV_EV_OUTPUT) {
        buf_putchar((char)ev.x);
        g_needs_redraw = 1;
      } else if (ev.type == WINSRV_EV_TTY_INPUT) {
        if ((char)ev.x == 0x03)
          for (int i = 0; i < n; i++)
            if (pids[i] > 0)
              kill(pids[i], SIGINT);
        // Otros TTY_INPUT: los ignoramos. El proceso hijo los lee
        // del tty directamente.
      } else if (ev.type == WINSRV_EV_CLOSE) {
        for (int i = 0; i < n; i++)
          if (pids[i] > 0)
            kill(pids[i], SIGKILL);
        sys_win_destroy(win_id);
        sys_exit(0);
      }
    }
    shell_render();
    if (remaining > 0)
      sys_yield();
  }
  return;

fail:
  tty_flush();
  for (int i = 0; i < n; i++)
    if (pids[i] > 0)
      kill(pids[i], SIGKILL);
  for (int i = 0; i < n_pipes; i++) {
    close(pipefds[i][0]);
    close(pipefds[i][1]);
  }
}

static void run_command(int win_id) {
  g_cur_fg = 7;
  g_cur_bold = 0;
  g_input[g_input_len] = '\0';

  // [TTY] Nada que ver con buf_putchar('\n'): el eco del '\n' que
  // disparó este comando ya llegó por WINSRV_EV_OUTPUT (el TTY lo
  // emite antes que el TTY_INPUT). Repetirlo aquí daba una línea en
  // blanco extra.

  if (g_input_len > 0) {
    pipeline_stage_t stages[MAX_PIPELINE];
    int n = parse_pipeline(g_input, stages, MAX_PIPELINE);
    if (n == 0) {
      /* nada */
    } else if (n == 1 && !stages[0].in_file && !stages[0].out_file) {
      const char *cmd = stages[0].argv[0];
      if (strcmp(cmd, "help") == 0)
        cmd_help();
      else if (strcmp(cmd, "cd") == 0)
        cmd_cd(stages[0].argc > 1 ? stages[0].argv[1] : NULL);
      else if (strcmp(cmd, "clear") == 0)
        cmd_clear();
      else if (strcmp(cmd, "exit") == 0) {
        puts("Adiós.");
        sys_exit(0);
      } else if (strcmp(cmd, "spawn") == 0) {
        if (stages[0].argc < 2)
          puts("uso: spawn <path> [args]");
        else
          run_pipeline(&stages[0], 1, win_id);
      } else
        run_pipeline(stages, 1, win_id);
    } else {
      run_pipeline(stages, n, win_id);
    }
  }
  g_input_len = 0;

  // Drenar eventos residuales del hijo. Solo OUTPUT (los TTY_INPUT ya
  // se consumieron en run_pipeline; los que queden son de la ventana
  // del usuario durante la transición y los descartamos).
  winsrv_event_t ev;
  while (sys_win_poll_event(win_id, &ev, 0) > 0) {
    if (ev.type == WINSRV_EV_OUTPUT) {
      buf_putchar((char)ev.x);
      g_needs_redraw = 1;
    } else if (ev.type == WINSRV_EV_CLOSE) {
      sys_win_destroy(win_id);
      sys_exit(0);
    }
  }

  // [TTY] Limpiar también después: si el usuario tecleó algo justo
  // cuando el hijo murió, esa entrada puede quedar a medias en el
  // canon_buf del TTY. Limpiamos para que el próximo spawn empiece
  // con buffer limpio.
  tty_flush();

  refresh_cwd();
  print_prompt();
  g_needs_redraw = 1;
  shell_render();
}

// ---------------------------------------------------------------------------
// [FIX] handle_key ya NO hace eco visual. El TTY es el único punto que
// dibuja la tecla al console (vía WINSRV_EV_OUTPUT). Aquí solo se
// mantiene el buffer de línea del shell (g_input) y se detecta Enter /
// Backspace.
// ---------------------------------------------------------------------------
static void handle_key(char c, int win_id) {
  (void)win_id;
  if (c == '\n') {
    run_command(win_id);
    return;
  }
  if (c == '\b') {
    if (g_input_len > 0)
      g_input_len--;
    return;
  }
  if (c >= 0x20 && c < 0x7F) {
    if (g_input_len < LINE_MAX - 1)
      g_input[g_input_len++] = c;
  }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  sys_print("SHELL: main begin");
  int cw = WIN_W;
  int ch = WIN_H - TITLEBAR_HEIGHT;

  int win = sys_win_create(80, 60, WIN_W, WIN_H, "Aurora Console");
  sys_print("SHELL: after win_create");
  if (win < 0) {
    sys_print("SHELL: win < 0, aborting");
    puts("console: no se pudo crear la ventana");
    return 1;
  }
  sys_print("SHELL: win >= 0");

  int icon_rc =
      sys_win_set_icon(win, "/initrd/system/icons/terminal-window-dark.bmp");
  if (icon_rc < 0)
    sys_print("SHELL: sys_win_set_icon FALLO");

  g_cw = cw;
  g_ch = ch;
  g_win = win;
  g_pixels = (uint32_t *)malloc(cw * ch * sizeof(uint32_t));
  if (!g_pixels) {
    puts("console: sin memoria");
    sys_win_destroy(win);
    return 1;
  }
  uint32_t *pixels = g_pixels;

  // [ENV] Defaults si el kernel no nos pasó entorno.
  if (!environ || !environ[0]) {
    setenv("PATH", "/initrd/apps:/bin:/", 1);
    setenv("TERM", "linux", 1);
    setenv("HOME", "/", 1);
    setenv("USER", "root", 1);
    setenv("LOGNAME", "root", 1);
    setenv("SHELL", "/initrd/apps/shell", 1);
    setenv("PWD", "/", 1);
  }

  buf_init();
  buf_puts("============================================\n");
  buf_puts("  Aurora OS Console v0.1\n");
  buf_puts("  Escribe 'help' para ver los comandos.\n");
  buf_puts("============================================\n");
  refresh_cwd();
  print_prompt();

  for (int i = 0; i < cw * ch; i++)
    pixels[i] = 0xFF000000;
  dirty_add(0, 0, cw, ch);
  {
    int rx0 = 0, ry0 = 0, rx1 = cw, ry1 = ch;
    render_region(pixels, cw, ch, rx0, ry0, rx1 - rx0, ry1 - ry0);
    sys_win_blit(win, rx0, ry0, rx1 - rx0, ry1 - ry0, rx0, ry0, cw, pixels);
  }
  dirty_reset();

  if (sys_win_register_console(win) < 0) {
    sys_print("SHELL: register_console failed");
    puts("console: no se pudo registrar como consola");
  }

  while (1) {
    winsrv_event_t ev;
    int r = sys_win_poll_event(win, &ev, 1);
    if (r <= 0)
      continue;

    switch (ev.type) {
    case WINSRV_EV_TTY_INPUT:
      handle_key((char)ev.x, win);
      g_needs_redraw = 1;
      break;
    case WINSRV_EV_OUTPUT:
      buf_putchar((char)ev.x);
      g_needs_redraw = 1;
      break;
    case WINSRV_EV_KEY:
      if (ev.x == 0x49) {
        g_buf.scroll_offset += 5;
        dirty_add(0, 0, cw, ch);
        g_needs_redraw = 1;
      } else if (ev.x == 0x51) {
        g_buf.scroll_offset -= 5;
        if (g_buf.scroll_offset < 0)
          g_buf.scroll_offset = 0;
        dirty_add(0, 0, cw, ch);
        g_needs_redraw = 1;
      } else if (ev.x == 0x4F) {
        g_buf.scroll_offset = 0;
        dirty_add(0, 0, cw, ch);
        g_needs_redraw = 1;
      } else if (ev.x == 0x47) {
        g_buf.scroll_offset = 9999999;
        dirty_add(0, 0, cw, ch);
        g_needs_redraw = 1;
      }
      break;
    case WINSRV_EV_CLOSE:
      sys_win_destroy(win);
      free(g_pixels);
      return 0;
    default:
      break;
    }

    while (sys_win_poll_event(win, &ev, 0) > 0) {
      switch (ev.type) {
      case WINSRV_EV_TTY_INPUT:
        handle_key((char)ev.x, win);
        g_needs_redraw = 1;
        break;
      case WINSRV_EV_OUTPUT:
        buf_putchar((char)ev.x);
        g_needs_redraw = 1;
        break;
      case WINSRV_EV_CLOSE:
        sys_win_destroy(win);
        free(g_pixels);
        return 0;
      default:
        break;
      }
    }

    int max_off = g_buf.count - 1;
    if (max_off < 0)
      max_off = 0;
    if (g_buf.scroll_offset > max_off)
      g_buf.scroll_offset = max_off;
    if (g_buf.scroll_offset < 0)
      g_buf.scroll_offset = 0;

    shell_render();
  }
  return 0;
}