// user/apps/shell/main.c
//
// Aurora Console: terminal gráfico + shell interactivo.
//
// La app:
//   1. Crea una ventana y se registra como consola.
//   2. Recibe eventos TTY_INPUT (bytes de teclado) y OUTPUT (bytes
//      escritos a stdout por cualquier proceso).
//   3. Hace eco visual, maneja backspace, y ejecuta comandos al
//      pulsar Enter.
//   4. Los comandos que escriben a stdout generan más eventos OUTPUT,
//      que la app también pinta.
//
// [Performance] Render por regiones:
//   - Mantenemos una "región sucia" (rectángulo mínimo que engloba
//     todo lo que ha cambiado desde el último render).
//   - Cuando toca redibujar, limpiamos y repintamos SOLO esa región.
//   - El blit al kernel envía SOLO esa región (con src_stride = cw
//     para que el kernel sepa leer del buffer completo).
//   - Antes enviábamos el buffer completo (820*568*4 ≈ 1.8 MB) en
//     cada tecla. Ahora enviamos ~50 KB. En VirtualBox sin KVM se
//     nota muchísimo.

#include "../../lib/file.h"
#include "../../lib/malloc.h"
#include "../../lib/process.h"
#include "../../lib/string.h"
#include "../../syscall.h"

// ---------------------------------------------------------------------------
// Configuración
// ---------------------------------------------------------------------------
#define WIN_W 820
#define WIN_H 600
#define TITLEBAR_HEIGHT 32

#define COLS 100
#define ROWS_VISIBLE 35
#define ROWS_BUFFER 512
#define LINE_HEIGHT 16
#define CHAR_WIDTH 8

#define LINE_MAX 256
#define MAX_ARGS 8

// ---------------------------------------------------------------------------
// Fuente 8x8 (ASCII 32..127)
// ---------------------------------------------------------------------------
static const uint8_t font8x8[96][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00},
    {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00},
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00},
    {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00},
    {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00},
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00},
    {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06},
    {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00},
    {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00},
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00},
    {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00},
    {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00},
    {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00},
    {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00},
    {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00},
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06},
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00},
    {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00},
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00},
    {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00},
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00},
    {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00},
    {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},
    {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00},
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00},
    {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},
    {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00},
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00},
    {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},
    {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00},
    {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00},
    {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00},
    {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00},
    {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},
    {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},
    {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00},
    {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},
    {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00},
    {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00},
    {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00},
    {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00},
    {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},
    {0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00},
    {0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00},
    {0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00},
    {0x38, 0x30, 0x30, 0x3e, 0x33, 0x33, 0x6E, 0x00},
    {0x00, 0x00, 0x1E, 0x33, 0x3f, 0x03, 0x1E, 0x00},
    {0x1C, 0x36, 0x06, 0x0f, 0x06, 0x06, 0x0F, 0x00},
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F},
    {0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00},
    {0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E},
    {0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00},
    {0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00},
    {0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00},
    {0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00},
    {0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F},
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78},
    {0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00},
    {0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00},
    {0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00},
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00},
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},
    {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00},
    {0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00},
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F},
    {0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00},
    {0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00},
    {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00},
    {0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00},
    {0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

// ---------------------------------------------------------------------------
// Buffer de líneas
// ---------------------------------------------------------------------------
typedef struct {
  char chars[COLS];
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

// ---------------------------------------------------------------------------
// Región sucia. Rectángulo mínimo que engloba todo lo que ha cambiado
// desde el último render. Se resetea tras cada render.
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

// Fila visible (0..ROWS_VISIBLE-1) de la línea actual del cursor, o -1
// si no está visible en el viewport actual.
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

// Marca la fila donde está el cursor como sucia.
static void mark_cursor_row_dirty(void) {
  int r = cursor_row_visible();
  if (r < 0)
    return;
  // La fila de texto empieza en y = r * LINE_HEIGHT + 2.
  // Ancho = COLS caracteres * CHAR_WIDTH + 4 px de margen a cada lado.
  dirty_add(0, r * LINE_HEIGHT, COLS * CHAR_WIDTH + 8, LINE_HEIGHT);
}

// ---------------------------------------------------------------------------
// Operaciones sobre el buffer de líneas
// ---------------------------------------------------------------------------
static void buf_new_line(void) {
  // Antes de mover el cursor de sitio, marcar la fila donde estaba.
  mark_cursor_row_dirty();

  int next;
  if (g_buf.count < ROWS_BUFFER) {
    next = g_buf.count;
    g_buf.count++;
  } else {
    next = g_buf.head;
    g_buf.head = (g_buf.head + 1) % ROWS_BUFFER;
    // Al hacer scroll del buffer (se descarta la primera línea),
    // toda la pantalla cambia: marcamos todo.
    dirty_add(0, 0, COLS * CHAR_WIDTH + 8, ROWS_VISIBLE * LINE_HEIGHT);
  }
  g_buf.lines[next].len = 0;
  g_buf.cursor_col = 0;

  // Después, marcar la nueva fila donde está el cursor.
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
  buf_new_line();
  // buf_new_line ya ha marcado toda la pantalla.
}

static void buf_putchar(char c) {
  // Marcar dónde estaba el cursor antes.
  mark_cursor_row_dirty();

  g_buf.scroll_offset = 0;

  if (c == '\n') {
    buf_new_line();
    return; // buf_new_line ya marca la nueva fila.
  }
  if (c == '\r') {
    g_buf.cursor_col = 0;
    mark_cursor_row_dirty();
    return;
  }
  if (c == '\b') {
    if (g_buf.cursor_col > 0) {
      g_buf.cursor_col--;
      int idx = buf_line_idx(g_buf.count - 1);
      line_t *l = &g_buf.lines[idx];
      if (g_buf.cursor_col < l->len) {
        if (g_buf.cursor_col == l->len - 1)
          l->len--;
      }
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
  if (g_buf.cursor_col + 1 > l->len)
    l->len = g_buf.cursor_col + 1;
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
static void draw_char(uint32_t *pixels, int cw, int ch, int x, int y,
                      unsigned char c, uint32_t color) {
  if (c < 32 || c > 127)
    return;
  const uint8_t *glyph = font8x8[(int)c - 32];
  for (int row = 0; row < 8; row++) {
    uint8_t bits = glyph[row];
    for (int col = 0; col < 8; col++) {
      if (bits & (1 << col)) {
        int px = x + col;
        int py = y + row;
        if (px >= 0 && px < cw && py >= 0 && py < ch) {
          pixels[py * cw + px] = color;
        }
      }
    }
  }
}

// Redibuja SOLO el rectángulo [rx0, ry0, rw, rh] del buffer `pixels`.
// Limpia la región con el color de fondo y redibuja los caracteres que
// intersectan. NO toca los píxeles fuera de la región.
static void render_region(uint32_t *pixels, int cw, int ch, int rx0, int ry0,
                          int rw, int rh) {
  // Clip a la ventana.
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

  // 1. Limpiar la región con el color de fondo.
  for (int y = ry0; y < ry0 + rh; y++) {
    uint32_t *row = &pixels[y * cw + rx0];
    for (int x = 0; x < rw; x++)
      row[x] = 0xFF000000;
  }

  if (g_buf.count == 0)
    return;

  // 2. Redibujar los caracteres que intersectan con la región.
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

    // Saltar filas totalmente fuera de la región.
    if (y + 8 < ry0 || y > ry0 + rh)
      continue;

    for (int k = 0; k < l->len; k++) {
      int x = k * CHAR_WIDTH + 4;
      // Saltar columnas totalmente fuera de la región.
      if (x + 8 < rx0 || x > rx0 + rw)
        continue;
      draw_char(pixels, cw, ch, x, y, (unsigned char)l->chars[k], 0xFFFFFFFF);
    }
  }

  // 3. Redibujar el cursor si está dentro de la región.
  if (g_buf.scroll_offset == 0) {
    int cursor_row_in_view = bottom - top;
    if (cursor_row_in_view >= 0 && cursor_row_in_view < ROWS_VISIBLE) {
      int cursor_x = 4 + g_buf.cursor_col * CHAR_WIDTH;
      int cursor_y = cursor_row_in_view * LINE_HEIGHT + 2 + 9;
      if (cursor_y + 2 > ry0 && cursor_y < ry0 + rh &&
          cursor_x + CHAR_WIDTH > rx0 && cursor_x < rx0 + rw) {
        for (int dy = 0; dy < 2; dy++) {
          for (int dx = 0; dx < CHAR_WIDTH; dx++) {
            int px = cursor_x + dx;
            int py = cursor_y + dy;
            if (px >= 0 && px < cw && py >= 0 && py < ch) {
              pixels[py * cw + px] = 0xFFFFFFFF;
            }
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Shell embebido
// ---------------------------------------------------------------------------
static const char *PROMPT = "aurora> ";

static void print_prompt(void) { buf_puts(PROMPT); }

static int parse_line(char *line, char *argv[MAX_ARGS]) {
  int argc = 0;
  char *p = line;
  while (*p && argc < MAX_ARGS) {
    while (*p == ' ' || *p == '\t')
      p++;
    if (!*p)
      break;
    argv[argc++] = p;
    while (*p && *p != ' ' && *p != '\t')
      p++;
    if (*p) {
      *p = '\0';
      p++;
    }
  }
  return argc;
}

static void cmd_help(void) {
  puts("Comandos disponibles:");
  puts("  help        - esta ayuda");
  puts("  echo <str>  - imprime el texto");
  puts("  cat <file>  - muestra un archivo");
  puts("  spawn <p>   - ejecuta el binario en <p>");
  puts("  clear       - limpia la consola");
  puts("  exit        - salir");
}

static void cmd_echo(int argc, char *argv[]) {
  for (int i = 1; i < argc; i++) {
    if (i > 1)
      write(1, " ", 1);
    write(1, argv[i], strlen(argv[i]));
  }
  write(1, "\n", 1);
}

static void cmd_cat(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    printf("cat: no existe %s\n", path);
    return;
  }
  char buf[512];
  int64_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0)
    write(1, buf, n);
  close(fd);
}

static void cmd_spawn(const char *path) {
  int pid = spawn(path);
  if (pid < 0) {
    printf("console: no se pudo ejecutar %s\n", path);
    return;
  }
  int status = 0;
  int r = waitpid(pid, &status, 0);
  printf("console: %s terminó (pid=%d, exit=%d)\n", path, r, status);
}

static void cmd_clear(void) {
  buf_init();
  // buf_init -> buf_new_line -> mark_cursor_row_dirty + dirty_add(0,0,...)
  // Con eso la región sucia cubre toda la pantalla.
}

static void run_command(int win_id) {
  g_input[g_input_len] = '\0';
  buf_putchar('\n');

  if (g_input_len > 0) {
    char *argv[MAX_ARGS];
    int argc = parse_line(g_input, argv);
    if (argc > 0) {
      const char *cmd = argv[0];
      if (strcmp(cmd, "help") == 0)
        cmd_help();
      else if (strcmp(cmd, "echo") == 0)
        cmd_echo(argc, argv);
      else if (strcmp(cmd, "cat") == 0) {
        if (argc < 2)
          puts("uso: cat <path>");
        else
          cmd_cat(argv[1]);
      } else if (strcmp(cmd, "spawn") == 0) {
        if (argc < 2)
          puts("uso: spawn <path>");
        else
          cmd_spawn(argv[1]);
      } else if (strcmp(cmd, "clear") == 0) {
        cmd_clear();
      } else if (strcmp(cmd, "exit") == 0) {
        puts("Adiós.");
        sys_exit(0);
      } else {
        cmd_spawn(cmd);
      }
    }
  }

  g_input_len = 0;

  // Drenar los OUTPUT que el comando haya generado. Esto garantiza
  // que el output aparezca ANTES del prompt.
  winsrv_event_t ev;
  while (sys_win_poll_event(win_id, &ev, 0) > 0) {
    if (ev.type == WINSRV_EV_OUTPUT) {
      buf_putchar((char)ev.x);
    } else if (ev.type == WINSRV_EV_CLOSE) {
      sys_win_destroy(win_id);
      sys_exit(0);
    }
  }

  print_prompt();
}

// ---------------------------------------------------------------------------
// Manejo de teclado
// ---------------------------------------------------------------------------
static void handle_key(char c, int win_id) {
  if (c == '\n') {
    run_command(win_id);
    return;
  }
  if (c == '\b') {
    if (g_input_len > 0) {
      // Marcar la fila antes y después.
      mark_cursor_row_dirty();
      g_input_len--;
      int idx = buf_line_idx(g_buf.count - 1);
      line_t *l = &g_buf.lines[idx];
      if (l->len > 0) {
        l->len--;
        if (g_buf.cursor_col > 0)
          g_buf.cursor_col--;
      }
      mark_cursor_row_dirty();
    }
    return;
  }
  if (c >= 0x20 && c < 0x7F) {
    if (g_input_len < LINE_MAX - 1) {
      g_input[g_input_len++] = c;
      buf_putchar(c);
    }
  }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(void) {
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

  // [FIX] Pedir al kernel que ponga el icono del terminal a la ventana.
  // El kernel carga el BMP desde tarfs y lo cachea tanto para la barra
  // de título como para el taskbar.
  //
  // Antes se ignoraba el valor de retorno: si el BMP no se encontraba en
  // el tarfs (o no estaba empaquetado en el initrd), la ventana se
  // quedaba con el icono por defecto SIN ningún aviso visible. Ahora se
  // comprueba y se avisa por sys_print para poder depurarlo.
  int icon_rc = sys_win_set_icon(win, "/system/icons/terminal-icon.bmp");
  if (icon_rc < 0) {
    sys_print("SHELL: sys_win_set_icon FALLO (system/icons/terminal-icon.bmp "
              "no encontrado en tarfs?)");
  }

  uint32_t *pixels = (uint32_t *)malloc(cw * ch * sizeof(uint32_t));
  if (!pixels) {
    puts("console: sin memoria");
    sys_win_destroy(win);
    return 1;
  }

  buf_init();

  buf_puts("============================================\n");
  buf_puts("  Aurora OS Console v0.1\n");
  buf_puts("  Escribe 'help' para ver los comandos.\n");
  buf_puts("============================================\n");
  print_prompt();

  // Primer render: toda la pantalla.
  for (int i = 0; i < cw * ch; i++)
    pixels[i] = 0xFF000000;
  dirty_add(0, 0, cw, ch);
  {
    int rx0 = 0, ry0 = 0;
    int rx1 = cw, ry1 = ch;
    render_region(pixels, cw, ch, rx0, ry0, rx1 - rx0, ry1 - ry0);
    sys_win_blit(win, rx0, ry0, rx1 - rx0, ry1 - ry0, rx0, ry0, cw, pixels);
  }
  dirty_reset();

  if (sys_win_register_console(win) < 0) {
    sys_print("SHELL: register_console failed");
    puts("console: no se pudo registrar como consola");
  }

  int needs_redraw = 0;

  while (1) {
    winsrv_event_t ev;
    int r = sys_win_poll_event(win, &ev, 1);
    if (r <= 0)
      continue;

    switch (ev.type) {
    case WINSRV_EV_TTY_INPUT:
      handle_key((char)ev.x, win);
      needs_redraw = 1;
      break;
    case WINSRV_EV_OUTPUT:
      buf_putchar((char)ev.x);
      needs_redraw = 1;
      break;
    case WINSRV_EV_KEY:
      if (ev.x == 0x49) {
        g_buf.scroll_offset += 5;
        dirty_add(0, 0, cw, ch);
        needs_redraw = 1;
      } else if (ev.x == 0x51) {
        g_buf.scroll_offset -= 5;
        if (g_buf.scroll_offset < 0)
          g_buf.scroll_offset = 0;
        dirty_add(0, 0, cw, ch);
        needs_redraw = 1;
      } else if (ev.x == 0x4F) {
        g_buf.scroll_offset = 0;
        dirty_add(0, 0, cw, ch);
        needs_redraw = 1;
      } else if (ev.x == 0x47) {
        g_buf.scroll_offset = 9999999;
        dirty_add(0, 0, cw, ch);
        needs_redraw = 1;
      }
      break;
    case WINSRV_EV_CLOSE:
      sys_win_destroy(win);
      free(pixels);
      return 0;
    default:
      break;
    }

    // Drenar cola sin bloquear.
    while (sys_win_poll_event(win, &ev, 0) > 0) {
      switch (ev.type) {
      case WINSRV_EV_TTY_INPUT:
        handle_key((char)ev.x, win);
        needs_redraw = 1;
        break;
      case WINSRV_EV_OUTPUT:
        buf_putchar((char)ev.x);
        needs_redraw = 1;
        break;
      case WINSRV_EV_CLOSE:
        sys_win_destroy(win);
        free(pixels);
        return 0;
      default:
        break;
      }
    }

    // Clamp de scroll_offset.
    int max_off = g_buf.count - 1;
    if (max_off < 0)
      max_off = 0;
    if (g_buf.scroll_offset > max_off)
      g_buf.scroll_offset = max_off;
    if (g_buf.scroll_offset < 0)
      g_buf.scroll_offset = 0;

    // Redibujar SOLO la región sucia.
    if (needs_redraw && !dirty_empty()) {
      // Extender la región a la rejilla de caracteres + margen, para
      // que draw_char nunca escriba fuera de la región.
      int rx0 = g_dirty_x0;
      int ry0 = g_dirty_y0;
      int rx1 = g_dirty_x1;
      int ry1 = g_dirty_y1;

      rx0 = (rx0 / CHAR_WIDTH) * CHAR_WIDTH - CHAR_WIDTH;
      ry0 = (ry0 / LINE_HEIGHT) * LINE_HEIGHT - LINE_HEIGHT;
      rx1 = ((rx1 + CHAR_WIDTH - 1) / CHAR_WIDTH) * CHAR_WIDTH + CHAR_WIDTH;
      ry1 = ((ry1 + LINE_HEIGHT - 1) / LINE_HEIGHT) * LINE_HEIGHT + LINE_HEIGHT;

      if (rx0 < 0)
        rx0 = 0;
      if (ry0 < 0)
        ry0 = 0;
      if (rx1 > cw)
        rx1 = cw;
      if (ry1 > ch)
        ry1 = ch;

      int rw = rx1 - rx0;
      int rh = ry1 - ry0;

      render_region(pixels, cw, ch, rx0, ry0, rw, rh);
      // Blit solo de la región. src_stride = cw, porque pixels es el
      // buffer completo. El rectángulo dentro del buffer es
      // (src_x=rx0, src_y=ry0), y el destino en la ventana es (x=rx0,
      // y=ry0). Como la ventana es del mismo tamaño que el buffer,
      // coinciden.
      sys_win_blit(win, rx0, ry0, rw, rh, rx0, ry0, cw, pixels);

      needs_redraw = 0;
      dirty_reset();
    }
  }

  return 0;
}