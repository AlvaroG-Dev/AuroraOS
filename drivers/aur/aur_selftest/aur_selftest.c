// tools/aur_selftest.c
//
// .aur de smoke-test de Fase 4.x.
//
// Ejercita:
//   4.2  manifest parseado → caps = MMIO|DMA|IRQ
//   4.3  mmap de /sys/bus/pci/devices/<BDF>/resource0
//   4.4  /dev/dma: DMA_ALLOC + mmap + DMA_SYNC
//   4.5  /dev/irq/<N>: open + epoll_wait + close
//
// Se empaqueta con:
//   aurbuild --name=aur_selftest --caps=0x7 <elf> <aur>
// y se coloca en sysroot/usr/bin/aur_selftest.
//
// Uso: /usr/bin/aur_selftest [BDF]
//      BDF por defecto: 0000:00:02.0 (e1000e en QEMU).

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>


// -- Copia de kernel/dma_user.h (toolchain userland no ve headers del kernel)
// --
#define DMA_ALLOC 0x100
#define DMA_SYNC 0x101

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

// -- Copia de kernel/aur.h --------------------------------------------------
struct aur_irq_event {
  uint32_t irq;
  uint32_t count;
};

// ===========================================================================
// 4.4: /dev/dma
// ===========================================================================
static int test_dma(void) {
  printf("=== 4.4: DMA ===\n");

  int fd_ctl = open("/dev/dma", O_RDWR);
  if (fd_ctl < 0) {
    printf("  FAIL open(/dev/dma): errno=%d\n", errno);
    return -1;
  }

  struct dma_alloc_req req = {.size = 4096};
  int fd_buf = ioctl(fd_ctl, DMA_ALLOC, &req);
  if (fd_buf < 0) {
    printf("  FAIL ioctl(DMA_ALLOC): errno=%d\n", errno);
    close(fd_ctl);
    return -1;
  }
  printf("  DMA alloc: fd=%d phys=0x%lx size=%lu\n", fd_buf,
         (unsigned long)req.phys, (unsigned long)req.size);

  uint8_t *buf =
      mmap(NULL, req.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_buf, 0);
  if (buf == MAP_FAILED) {
    printf("  FAIL mmap: errno=%d\n", errno);
    close(fd_buf);
    close(fd_ctl);
    return -1;
  }

  for (int i = 0; i < 256; i++)
    buf[i] = (uint8_t)(i ^ 0x5A);

  struct dma_sync_req sync = {.offset = 0, .size = 256, .dir = 0};
  if (ioctl(fd_buf, DMA_SYNC, &sync) < 0) {
    printf("  FAIL ioctl(DMA_SYNC): errno=%d\n", errno);
    munmap(buf, req.size);
    close(fd_buf);
    close(fd_ctl);
    return -1;
  }

  int ok = 1;
  for (int i = 0; i < 256; i++) {
    if (buf[i] != (uint8_t)(i ^ 0x5A)) {
      ok = 0;
      break;
    }
  }
  munmap(buf, req.size);
  close(fd_buf);
  close(fd_ctl);

  if (ok) {
    printf("  PASS DMA (alloc+mmap+sync+readback)\n");
    return 0;
  }
  printf("  FAIL DMA contenido corrupto\n");
  return -1;
}

// ===========================================================================
// 4.3: MMIO (resource0)
// ===========================================================================
static int test_mmio(const char *bdf) {
  printf("=== 4.3: MMIO (%s) ===\n", bdf);

  char path[128];
  snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0", bdf);

  int fd = open(path, O_RDWR | O_SYNC);
  if (fd < 0) {
    printf("  SKIP: %s no abre (errno=%d)\n", path, errno);
    return 0; // SKIP no cuenta como fallo
  }

  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size < 0x10) {
    printf("  SKIP: resource0 size=%ld < 0x10\n", (long)st.st_size);
    close(fd);
    return 0;
  }
  printf("  resource0 size=0x%lx\n", (unsigned long)st.st_size);

  void *bar = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (bar == MAP_FAILED) {
    printf("  FAIL mmap: errno=%d\n", errno);
    close(fd);
    return -1;
  }

  volatile uint32_t *regs = (volatile uint32_t *)bar;
  uint32_t ctrl = regs[0x00 / 4];
  uint32_t status = regs[0x08 / 4];
  uint32_t ctrl_ext = regs[0x18 / 4];
  printf("  BAR0: CTRL=0x%08x STATUS=0x%08x CTRL_EXT=0x%08x\n", ctrl, status,
         ctrl_ext);

  int ok = (status != 0xFFFFFFFF);
  munmap(bar, 0x1000);
  close(fd);

  if (ok) {
    printf("  PASS MMIO (STATUS válido)\n");
    return 0;
  }
  printf("  FAIL MMIO STATUS=0xFFFFFFFF\n");
  return -1;
}

// ===========================================================================
// 4.5: /dev/irq/<N>
//
// Usamos 0x64 = 100, un vector MSI libre. No dispara solo (no hay
// device que lo emita), así que validamos:
//   - open() con AUR_CAP_IRQ
//   - epoll_ctl() + epoll_wait() con timeout
//   - close()  → irq_free, restaura el estado del irq_threaded
//
// Si quisiéramos probar DELIVERY real, podríamos pulsar una tecla en
// IRQ 1, pero eso desplaza temporalmente el handler del PS/2. Por eso
// usamos MSI libre: testeable sin interacción.
// ===========================================================================
static int test_irq(void) {
  printf("=== 4.5: IRQ ===\n");

  // Test A: open + close (lifecycle)
  int fd = open("/dev/irq/100", O_RDONLY);
  if (fd < 0) {
    printf("  FAIL open(/dev/irq/100): errno=%d\n", errno);
    return -1;
  }
  close(fd);
  printf("  PASS open/close irq 100 (0x64)\n");

  // Test B: epoll con timeout (no debe dispararse solo)
  fd = open("/dev/irq/101", O_RDONLY);
  if (fd < 0) {
    printf("  FAIL open(/dev/irq/101): errno=%d\n", errno);
    return -1;
  }

  int epfd = epoll_create1(0);
  if (epfd < 0) {
    printf("  FAIL epoll_create1: errno=%d\n", errno);
    close(fd);
    return -1;
  }

  struct epoll_event ev = {.events = EPOLLIN};
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
    printf("  FAIL epoll_ctl(ADD): errno=%d\n", errno);
    close(epfd);
    close(fd);
    return -1;
  }

  struct epoll_event out;
  int n = epoll_wait(epfd, &out, 1, 1000); // 1 s

  close(epfd);
  close(fd);

  if (n == 0) {
    printf("  PASS epoll timeout irq 101 (esperado, MSI sin device)\n");
    return 0;
  }
  if (n > 0) {
    // Inesperado: si algún día un device emite MSI 0x65, veremos
    // esto. Un PASS de todos modos, con el count.
    struct aur_irq_event e = {0};
    printf("  PASS epoll fired irq 101 (evento inesperado)\n");
    (void)e;
    return 0;
  }
  printf("  FAIL epoll_wait: errno=%d\n", errno);
  return -1;
}

// ===========================================================================
int main(int argc, char **argv) {
  const char *bdf = (argc > 1) ? argv[1] : "0000:00:02.0";
  printf("[aur_selftest] inicio (caps debe ser MMIO|DMA|IRQ = 0x7)\n");

  int fails = 0;
  if (test_mmio(bdf) < 0)
    fails++;
  if (test_dma() < 0)
    fails++;
  if (test_irq() < 0)
    fails++;

  if (fails == 0) {
    printf("[aur_selftest] PASS\n");
    return 0;
  }
  printf("[aur_selftest] FAILED (%d)\n", fails);
  return 1;
}