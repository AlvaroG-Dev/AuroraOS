// tools/aur_dma_test.c
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define DMA_ALLOC 0x100
#define DMA_SYNC  0x101

struct dma_alloc_req {
  uint64_t size;
  uint64_t phys;
  uint32_t flags;
  uint32_t _pad;
};

struct dma_sync_req {
  uint64_t offset;
  uint64_t size;
  uint32_t dir;
  uint32_t _pad;
};

int main(void) {
    int fd_ctl = open("/dev/dma", O_RDWR);
    if (fd_ctl < 0) {
        perror("open /dev/dma");
        return 1;
    }

    struct dma_alloc_req req = { .size = 4096 * 16 }; // 64 KB
    int fd_buf = ioctl(fd_ctl, DMA_ALLOC, &req);
    if (fd_buf < 0) {
        perror("ioctl DMA_ALLOC");
        close(fd_ctl);
        return 1;
    }

    printf("DMA alloc OK: fd=%d phys=0x%lx size=%lu\n",
           fd_buf, (unsigned long)req.phys, (unsigned long)req.size);

    void *virt = mmap(NULL, req.size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd_buf, 0);
    if (virt == MAP_FAILED) {
        perror("mmap");
        close(fd_buf);
        close(fd_ctl);
        return 1;
    }
    printf("DMA mmap OK en %p\n", virt);

    uint8_t *buf = (uint8_t *)virt;
    for (int i = 0; i < 256; i++) {
        buf[i] = (uint8_t)i;
    }

    struct dma_sync_req sync = { .offset = 0, .size = 256, .dir = 0 };
    if (ioctl(fd_buf, DMA_SYNC, &sync) < 0) {
        perror("ioctl DMA_SYNC");
    }

    printf("buf[0]=%d buf[255]=%d (esperado 0 y 255)\n", buf[0], buf[255]);

    munmap(virt, req.size);
    close(fd_buf);
    close(fd_ctl);
    printf("DMA test completado\n");
    return 0;
}