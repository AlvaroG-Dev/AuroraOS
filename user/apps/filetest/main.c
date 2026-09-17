// apps/filetest/main.c
// Tests: open, read, seek, fstat, write(stdout), close
#include "../../syscall.h"
#include "../../lib/string.h"
#include "../../lib/malloc.h"

static void print_int(const char *prefix, int64_t v) {
    char buf[32];
    char out[128];
    // sign
    int neg = 0;
    uint64_t uv;
    if (v < 0) { neg = 1; uv = (uint64_t)(-v); } else { uv = (uint64_t)v; }
    int i = 0;
    if (uv == 0) { buf[i++] = '0'; }
    while (uv) { buf[i++] = '0' + (int)(uv % 10); uv /= 10; }
    if (neg) buf[i++] = '-';
    // reverse
    for (int a = 0, b = i - 1; a < b; a++, b--) {
        char t = buf[a]; buf[a] = buf[b]; buf[b] = t;
    }
    buf[i] = 0;
    int oi = 0, pi = 0, bi = 0;
    while (prefix[pi]) out[oi++] = prefix[pi++];
    while (buf[bi]) out[oi++] = buf[bi++];
    out[oi++] = '\n'; out[oi] = 0;
    sys_print(out);
}

int main(void) {
    sys_print("[filetest] === Aurora OS File Test App ===\n");

    // Test 1: open + read
    sys_print("[filetest] Test 1: open/read system/config.txt\n");
    int fd = sys_open("system/config.txt", O_RDONLY);
    if (fd < 0) {
        sys_print("[filetest] ERROR: no se pudo abrir config.txt\n");
        sys_exit(10);
    }

    char buf[64];
    int64_t n = sys_read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        sys_print("[filetest] ERROR: read fallido\n");
        sys_exit(11);
    }
    buf[n] = 0;
    sys_print("[filetest]   Contenido: '");
    sys_print(buf);
    sys_print("'\n");

    // Test 2: fstat - check file size
    sys_print("[filetest] Test 2: fstat\n");
    stat_t st;
    int sr = sys_fstat(fd, &st);
    if (sr < 0) {
        sys_print("[filetest] ERROR: fstat fallido\n");
        sys_exit(12);
    }
    print_int("[filetest]   File size: ", (int64_t)st.size);
    if (st.size == 0) {
        sys_print("[filetest] ERROR: tamaño de archivo es 0\n");
        sys_exit(13);
    }
    sys_print("[filetest]   fstat: OK\n");

    // Test 3: seek to beginning and re-read
    sys_print("[filetest] Test 3: seek(SEEK_SET) + re-read\n");
    int64_t pos = sys_seek(fd, 0, SEEK_SET);
    if (pos < 0) {
        sys_print("[filetest] ERROR: seek fallido\n");
        sys_exit(14);
    }
    char buf2[64];
    int64_t n2 = sys_read(fd, buf2, sizeof(buf2) - 1);
    if (n2 <= 0) {
        sys_print("[filetest] ERROR: re-read fallido\n");
        sys_exit(15);
    }
    buf2[n2] = 0;
    // Verify same content
    int match = (n == n2);
    for (int i = 0; match && i < (int)n; i++) if (buf[i] != buf2[i]) match = 0;
    sys_print(match ? "[filetest]   seek+re-read: OK\n" : "[filetest]   seek+re-read: FALLO\n");
    if (!match) sys_exit(16);

    // Test 4: write to stdout
    sys_print("[filetest] Test 4: write(1, ...) a stdout\n");
    const char *msg = "[filetest]   write-stdout: OK\n";
    int64_t written = sys_write(1, msg, strlen(msg));
    if (written <= 0) {
        sys_print("[filetest] ERROR: write a stdout fallido\n");
        sys_exit(17);
    }

    // Test 5: close
    sys_print("[filetest] Test 5: close\n");
    int cr = sys_close(fd);
    if (cr < 0) {
        sys_print("[filetest] ERROR: close fallido\n");
        sys_exit(18);
    }
    sys_print("[filetest]   close: OK\n");

    sys_print("[filetest] Todas las pruebas VFS PASARON. Saliendo con 0.\n");
    sys_exit(0);
    return 0;
}
