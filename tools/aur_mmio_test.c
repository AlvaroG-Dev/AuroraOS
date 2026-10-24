// tools/aur_mmio_test.c
// Test de FASE 4.3: mmap de /sys/bus/pci/devices/<BDF>/resource0.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    const char *bdf = (argc > 1) ? argv[1] : "0000:00:02.0";
    char path[128];
    snprintf(path, sizeof(path),
             "/sys/bus/pci/devices/%s/resource0", bdf);

    printf("abriendo %s...\n", path);
    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return 1;
    }
    printf("resource0 size=%ld\n", (long)st.st_size);
    if (st.st_size < 0x10) {
        printf("BAR demasiado pequeño para probar\n");
        close(fd);
        return 1;
    }

    void *bar = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (bar == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    printf("mmap OK en %p\n", bar);

    volatile uint32_t *regs = (volatile uint32_t *)bar;
    // e1000e: CTRL @ 0x0000, STATUS @ 0x0008, CTRL_EXT @ 0x0018
    uint32_t ctrl      = regs[0x00 / 4];
    uint32_t status    = regs[0x08 / 4];  // <-- Corregido
    uint32_t ctrl_ext  = regs[0x18 / 4];

    printf("CTRL=0x%08x STATUS=0x%08x CTRL_EXT=0x%08x\n",
           ctrl, status, ctrl_ext);

    if (status == 0xFFFFFFFF) {
        printf("FAIL: STATUS=0xFFFFFFFF (BAR sin mapear)\n");
    } else if (status == 0) {
        printf("WARN: STATUS=0 (device dormido?)\n");
    } else {
        printf("MMIO OK: STATUS valido (0x%08x)\n", status);
    }

    if (munmap(bar, 0x1000) != 0) {
        perror("munmap");
    }
    close(fd);
    printf("test completado\n");  // <-- añadido para depurar
    return 0;
}