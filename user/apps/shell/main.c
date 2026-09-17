// user/apps/console/main.c
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
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // ' '
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00}, // '!'
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // '"'
    {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00}, // '#'
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00}, // '$'
    {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00}, // '%'
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00}, // '&'
    {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00}, // '\''
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00}, // '('
    {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00}, // ')'
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, // '*'
    {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00}, // '+'
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06}, // ','
    {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00}, // '-'
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00}, // '.'
    {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00}, // '/'
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00}, // '0'
    {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00}, // '1'
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00}, // '2'
    {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00}, // '3'
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00}, // '4'
    {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00}, // '5'
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00}, // '6'
    {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00}, // '7'
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00}, // '8'
    {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00}, // '9'
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00}, // ':'
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06}, // ';'
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00}, // '<'
    {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00}, // '='
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00}, // '>'
    {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00}, // '?'
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00}, // '@'
    {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00}, // 'A'
    {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00}, // 'B'
    {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00}, // 'C'
    {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00}, // 'D'
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00}, // 'E'
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00}, // 'F'
    {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00}, // 'G'
    {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00}, // 'H'
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, // 'I'
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00}, // 'J'
    {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00}, // 'K'
    {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00}, // 'L'
    {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00}, // 'M'
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00}, // 'N'
    {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00}, // 'O'
    {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00}, // 'P'
    {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00}, // 'Q'
    {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00}, // 'R'
    {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00}, // 'S'
    {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, // 'T'
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00}, // 'U'
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, // 'V'
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00}, // 'W'
    {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00}, // 'X'
    {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00}, // 'Y'
    {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00}, // 'Z'
    {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00}, // '['
    {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00}, // '\\'
    {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00}, // ']'
    {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00}, // '^'
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF}, // '_'
    {0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}, // '`'
    {0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00}, // 'a'
    {0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00}, // 'b'
    {0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00}, // 'c'
    {0x38, 0x30, 0x30, 0x3e, 0x33, 0x33, 0x6E, 0x00}, // 'd'
    {0x00, 0x00, 0x1E, 0x33, 0x3f, 0x03, 0x1E, 0x00}, // 'e'
    {0x1C, 0x36, 0x06, 0x0f, 0x06, 0x06, 0x0F, 0x00}, // 'f'
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F}, // 'g'
    {0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00}, // 'h'
    {0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, // 'i'
    {0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E}, // 'j'
    {0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00}, // 'k'
    {0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, // 'l'
    {0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00}, // 'm'
    {0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00}, // 'n'
    {0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00}, // 'o'
    {0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F}, // 'p'
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78}, // 'q'
    {0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00}, // 'r'
    {0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00}, // 's'
    {0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00}, // 't'
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00}, // 'u'
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, // 'v'
    {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00}, // 'w'
    {0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00}, // 'x'
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F}, // 'y'
    {0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00}, // 'z'
    {0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00}, // '{'
    {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00}, // '|'
    {0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00}, // '}'
    {0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // '~'
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // DEL
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

// Línea actual de input del usuario (independiente del buffer de pantalla).
static char g_input[LINE_MAX];
static int  g_input_len = 0;

static void buf_new_line(void) {
    int next;
    if (g_buf.count < ROWS_BUFFER) {
        next = g_buf.count;
        g_buf.count++;
    } else {
        next = g_buf.head;
        g_buf.head = (g_buf.head + 1) % ROWS_BUFFER;
    }
    g_buf.lines[next].len = 0;
    g_buf.cursor_col = 0;
}

static int buf_line_idx(int logical) {
    if (g_buf.count < ROWS_BUFFER) return logical;
    return (g_buf.head + logical) % ROWS_BUFFER;
}

static void buf_init(void) {
    memset(&g_buf, 0, sizeof(g_buf));
    g_buf.cursor_col = 0;
    g_buf.scroll_offset = 0;
    buf_new_line();
}

static void buf_putchar(char c) {
    g_buf.scroll_offset = 0;

    if (c == '\n') { buf_new_line(); return; }
    if (c == '\r') { g_buf.cursor_col = 0; return; }
    if (c == '\b') {
        if (g_buf.cursor_col > 0) {
            g_buf.cursor_col--;
            int idx = buf_line_idx(g_buf.count - 1);
            line_t *l = &g_buf.lines[idx];
            if (g_buf.cursor_col < l->len) {
                if (g_buf.cursor_col == l->len - 1) l->len--;
            }
        }
        return;
    }
    if (c < 0x20 || c >= 0x7F) return;

    int idx = buf_line_idx(g_buf.count - 1);
    line_t *l = &g_buf.lines[idx];

    if (g_buf.cursor_col >= COLS - 1) {
        buf_new_line();
        idx = buf_line_idx(g_buf.count - 1);
        l = &g_buf.lines[idx];
    }
    l->chars[g_buf.cursor_col] = c;
    if (g_buf.cursor_col + 1 > l->len) l->len = g_buf.cursor_col + 1;
    g_buf.cursor_col++;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
static void draw_char(uint32_t *pixels, int cw, int ch,
                      int x, int y, char c, uint32_t color) {
    if (c < 32 || c > 127) return;
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

static void render(uint32_t *pixels, int cw, int ch) {
    for (int i = 0; i < cw * ch; i++) pixels[i] = 0xFF000000;

    if (g_buf.count == 0) return;

    int bottom = g_buf.count - 1 - g_buf.scroll_offset;
    if (bottom < 0) bottom = 0;
    int top = bottom - ROWS_VISIBLE + 1;
    if (top < 0) top = 0;

    for (int i = 0; i < ROWS_VISIBLE; i++) {
        int logical = top + i;
        if (logical > bottom) break;
        int idx = buf_line_idx(logical);
        line_t *l = &g_buf.lines[idx];
        int y = i * LINE_HEIGHT + 2;

        for (int k = 0; k < l->len; k++) {
            int x = k * CHAR_WIDTH + 4;
            draw_char(pixels, cw, ch, x, y, l->chars[k], 0xFFFFFFFF);
        }
    }

    if (g_buf.scroll_offset == 0) {
        int cursor_x = 4 + g_buf.cursor_col * CHAR_WIDTH;
        int cursor_y = (ROWS_VISIBLE - 1) * LINE_HEIGHT + 2 + 9;
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

// ---------------------------------------------------------------------------
// Shell embebido
// ---------------------------------------------------------------------------
static int parse_line(char *line, char *argv[MAX_ARGS]) {
    int argc = 0;
    char *p = line;
    while (*p && argc < MAX_ARGS) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }
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
        if (i > 1) write(1, " ", 1);
        write(1, argv[i], strlen(argv[i]));
    }
    write(1, "\n", 1);
}

static void cmd_cat(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("cat: no existe %s\n", path); return; }
    char buf[512];
    int64_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) write(1, buf, n);
    close(fd);
}

static void cmd_spawn(const char *path) {
    int pid = spawn(path);
    if (pid < 0) { printf("console: no se pudo ejecutar %s\n", path); return; }
    int status = 0;
    int r = waitpid(pid, &status, 0);
    printf("console: %s terminó (pid=%d, exit=%d)\n", path, r, status);
}

static void cmd_clear(void) {
    // Borrar todas las líneas: reiniciar el buffer.
    buf_init();
}

static void run_command(void) {
    g_input[g_input_len] = '\0';

    // Eco de la línea + salto para que aparezca en el scroll.
    // El eco ya lo hemos pintado a medida que se tecleaba, así que
    // solo añadimos el \n al buffer.
    buf_putchar('\n');

    if (g_input_len == 0) {
        g_input_len = 0;
        return;
    }

    char *argv[MAX_ARGS];
    int argc = parse_line(g_input, argv);
    if (argc == 0) { g_input_len = 0; return; }

    const char *cmd = argv[0];
    if (strcmp(cmd, "help") == 0) cmd_help();
    else if (strcmp(cmd, "echo") == 0) cmd_echo(argc, argv);
    else if (strcmp(cmd, "cat") == 0) {
        if (argc < 2) puts("uso: cat <path>");
        else cmd_cat(argv[1]);
    } else if (strcmp(cmd, "spawn") == 0) {
        if (argc < 2) puts("uso: spawn <path>");
        else cmd_spawn(argv[1]);
    } else if (strcmp(cmd, "clear") == 0) {
        cmd_clear();
    } else if (strcmp(cmd, "exit") == 0) {
        puts("Adiós.");
        sys_exit(0);
    } else {
        // Por defecto, intentar spawnear el comando como path.
        cmd_spawn(cmd);
    }

    g_input_len = 0;
}

// ---------------------------------------------------------------------------
// Manejo de teclado
// ---------------------------------------------------------------------------
static void handle_key(char c) {
    if (c == '\n') {
        run_command();
        return;
    }
    if (c == '\b') {
        if (g_input_len > 0) {
            g_input_len--;
            // Actualizar el buffer visual: borrar el último char.
            int idx = buf_line_idx(g_buf.count - 1);
            line_t *l = &g_buf.lines[idx];
            if (l->len > 0) {
                l->len--;
                if (g_buf.cursor_col > 0) g_buf.cursor_col--;
            }
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
    int cw = WIN_W;
    int ch = WIN_H - TITLEBAR_HEIGHT;

    int win = sys_win_create(80, 60, WIN_W, WIN_H, "Aurora Console");
    if (win < 0) {
        puts("console: no se pudo crear la ventana");
        return 1;
    }

    uint32_t *pixels = (uint32_t *)malloc(cw * ch * sizeof(uint32_t));
    if (!pixels) {
        puts("console: sin memoria");
        sys_win_destroy(win);
        return 1;
    }

    buf_init();

    // Banner.
    buf_putchar('='); buf_putchar('='); buf_putchar('='); buf_putchar('=');
    buf_putchar('='); buf_putchar('='); buf_putchar('='); buf_putchar('=');
    buf_putchar('='); buf_putchar('='); buf_putchar('='); buf_putchar('=');
    buf_putchar('\n');
    // "  Aurora OS Console v0.1" -> escribimos char a char
    const char *banner1 = "  Aurora OS Console v0.1";
    for (const char *p = banner1; *p; p++) buf_putchar(*p);
    buf_putchar('\n');
    const char *banner2 = "  Escribe 'help' para ver los comandos.";
    for (const char *p = banner2; *p; p++) buf_putchar(*p);
    buf_putchar('\n');
    for (int i = 0; i < 12; i++) buf_putchar('=');
    buf_putchar('\n');

    // Prompt inicial.
    const char *prompt = "aurora> ";
    for (const char *p = prompt; *p; p++) buf_putchar(*p);

    render(pixels, cw, ch);
    sys_win_blit(win, 0, 0, cw, ch, pixels);

    if (sys_win_register_console(win) < 0) {
        puts("console: no se pudo registrar como consola");
    }

    int needs_redraw = 0;

    while (1) {
        winsrv_event_t ev;
        int r = sys_win_poll_event(win, &ev, 1);
        if (r <= 0) continue;

        // Procesar este evento.
        switch (ev.type) {
        case WINSRV_EV_TTY_INPUT:
            handle_key((char)ev.x);
            needs_redraw = 1;
            break;
        case WINSRV_EV_OUTPUT:
            buf_putchar((char)ev.x);
            needs_redraw = 1;
            break;
        case WINSRV_EV_KEY:
            if (ev.x == 0x49) { g_buf.scroll_offset += 5; needs_redraw = 1; }
            else if (ev.x == 0x51) { g_buf.scroll_offset -= 5; if (g_buf.scroll_offset < 0) g_buf.scroll_offset = 0; needs_redraw = 1; }
            else if (ev.x == 0x4F) { g_buf.scroll_offset = 0; needs_redraw = 1; }
            else if (ev.x == 0x47) { g_buf.scroll_offset = 9999999; needs_redraw = 1; }
            break;
        case WINSRV_EV_CLOSE:
            sys_win_destroy(win);
            free(pixels);
            return 0;
        default:
            break;
        }

        // Drenar la cola sin bloquear para agrupar eventos.
        while (sys_win_poll_event(win, &ev, 0) > 0) {
            switch (ev.type) {
            case WINSRV_EV_TTY_INPUT:
                handle_key((char)ev.x);
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

        // Clamp de scroll.
        int max_off = g_buf.count - 1;
        if (max_off < 0) max_off = 0;
        if (g_buf.scroll_offset > max_off) g_buf.scroll_offset = max_off;
        if (g_buf.scroll_offset < 0) g_buf.scroll_offset = 0;

        if (needs_redraw) {
            render(pixels, cw, ch);
            sys_win_blit(win, 0, 0, cw, ch, pixels);
            needs_redraw = 0;
        }
    }

    return 0;
}
