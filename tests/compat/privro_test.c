// privro_test: MAP_PRIVATE de solo lectura ve lo que otro proceso escribe en el
// fichero (Linux: page cache compartida hasta la primera escritura). Wine mapea
// asi la memoria de sesion de wineserver.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int ok, bad;
#define CHECK(c, m) do { if (c) { ok++; printf("  ok  %s\n", m); } else { bad++; printf("  FALLO %s\n", m); } } while (0)

int main(void) {
    char path[] = "/tmp/privroXXXXXX";
    int fd = mkstemp(path);
    unlink(path);
    if (fd < 0 || ftruncate(fd, 3 * 4096)) { perror("tmp"); return 1; }
    volatile unsigned *w = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    volatile unsigned *r = mmap(NULL, 3 * 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(w != MAP_FAILED && r != MAP_FAILED, "mmap");
    w[0] = 1;
    CHECK(r[0] == 1, "lectura inicial");
    w[0] = 2;
    CHECK(r[0] == 2, "ve escritura posterior (misma pagina ya leida)");
    pid_t p = fork();
    if (p == 0) { w[1024] = 77; _exit(0); }
    waitpid(p, NULL, 0);
    CHECK(r[1024] == 77, "ve escritura de otro proceso");
    volatile unsigned *r2 = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 4096);
    CHECK(r2 != MAP_FAILED && r2[0] == 77, "con offset");
    p = fork();
    if (p == 0) { _exit(r[0] == 2 ? 0 : 1); }
    int st; waitpid(p, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "hijo hereda la vista");
    CHECK(mprotect((void *)r, 4096, PROT_READ | PROT_WRITE) == 0, "mprotect escritura");
    ((unsigned *)r)[0] = 5;
    CHECK(w[0] == 2, "escribir la privada no toca el fichero");
    w[0] = 9;
    CHECK(r[0] == 5, "la privada ya es copia");
    CHECK(r[1024] == 77, "resto sigue igual");
    char buf[8];
    if (pread(fd, buf, 4, 0) != 4) bad++;
    CHECK(*(unsigned *)buf == 9, "pread coherente");
    // MAP_PRIVATE escribible (Wine mapea asi sus vistas): COW por pagina.
    volatile unsigned *pw = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(pw != MAP_FAILED && pw[0] == 9, "privada RW lee el fichero");
    w[0] = 10;
    CHECK(pw[0] == 10, "privada RW ve escritura posterior");
    pw[0] = 11;
    CHECK(w[0] == 10, "escribir privada RW no toca el fichero");
    w[0] = 12; w[1024] = 13;
    CHECK(pw[0] == 11 && pw[1024] == 13, "pagina copiada fija, la otra sigue compartida");
    p = fork();
    if (p == 0) { pw[2048] = 1; _exit(pw[0] == 11 && w[2048] == 0 ? 0 : 1); }
    waitpid(p, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "fork + COW en el hijo");
    CHECK(pw[2048] == 0, "el padre no ve la copia del hijo");
    CHECK(munmap((void *)pw, 3 * 4096) == 0, "munmap");
    printf("PRIVRO-TEST: %d ok, %d fallos%s\n", ok, bad, bad ? "" : " ALL PASS");
    return bad != 0;
}
