// tools/e1000e_aur.c
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>


// ============================================================================
// Definiciones de IOCTLs
// ============================================================================
#define DMA_ALLOC 0x100
#define DMA_SYNC 0x101
struct dma_alloc_req {
  uint64_t size, phys;
  uint32_t flags, _pad;
};
struct dma_sync_req {
  uint64_t offset, size;
  uint32_t dir, _pad;
};

#define AUR_IRQ_MASK 0x300
#define AUR_IRQ_UNMASK 0x301
#define DHCP_RESTART 0x200

// ============================================================================
// Registros del e1000e (Intel 82574L)
// ============================================================================
#define E1000_CTRL 0x0000
#define E1000_STATUS 0x0008
#define E1000_ICR 0x00C0
#define E1000_IMS 0x00D0
#define E1000_IMC 0x00D8
#define E1000_RCTL 0x0100
#define E1000_TCTL 0x0400
#define E1000_RDBAL 0x2800
#define E1000_RDBAH 0x2804
#define E1000_RDLEN 0x2808
#define E1000_RDH 0x2810
#define E1000_RDT 0x2818
#define E1000_TDBAL 0x3800
#define E1000_TDBAH 0x3804
#define E1000_TDLEN 0x3808
#define E1000_TDH 0x3810
#define E1000_TDT 0x3818

#define E1000_CTRL_FD (1u << 0)
#define E1000_CTRL_SLU (1u << 6)
#define E1000_CTRL_RST (1u << 26)

#define E1000_RCTL_EN (1u << 1)
#define E1000_RCTL_BAM (1u << 15)
#define E1000_RCTL_SECRC (1u << 26)

#define E1000_TCTL_EN (1u << 1)
#define E1000_TCTL_PSP (1u << 3)

// ============================================================================
// Descriptores DMA
// ============================================================================
#define DESC_COUNT 128
#define BUF_SIZE 2048

typedef struct __attribute__((packed)) {
  uint64_t addr;
  uint16_t length;
  uint16_t csum;
  uint8_t status;
  uint8_t errors;
  uint16_t special;
} rx_desc_t;

typedef struct __attribute__((packed)) {
  uint64_t addr;
  uint16_t length;
  uint8_t cso;
  uint8_t cmd;
  uint8_t status;
  uint8_t css;
  uint16_t special;
} tx_desc_t;

#define RX_DD 0x01u
#define TX_DD 0x01u
#define TX_CMD_EOP 0x01u
#define TX_CMD_IFCS 0x02u
#define TX_CMD_RS 0x08u

// ============================================================================
// Variables Globales
// ============================================================================
static volatile uint32_t *mmio;
static rx_desc_t *rx_ring;
static tx_desc_t *tx_ring;
static uint8_t *rx_bufs, *tx_bufs;

static int fd_net0;
static int fd_irq;

static inline void mmio_wr(uint32_t reg, uint32_t val) { mmio[reg / 4] = val; }
static inline uint32_t mmio_rd(uint32_t reg) { return mmio[reg / 4]; }

// ============================================================================
// Configuración de los Rings DMA
// ============================================================================
static void setup_rings(int fd_dma) {
  struct dma_alloc_req req;
  int fd_buf;

  // 1. Ring RX (1 página)
  req.size = 4096;
  fd_buf = ioctl(fd_dma, DMA_ALLOC, &req);
  rx_ring = mmap(NULL, req.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_buf, 0);
  mmio_wr(E1000_RDBAL, (uint32_t)req.phys);
  mmio_wr(E1000_RDBAH, (uint32_t)(req.phys >> 32));
  mmio_wr(E1000_RDLEN, DESC_COUNT * sizeof(rx_desc_t));
  mmio_wr(E1000_RDH, 0);
  mmio_wr(E1000_RDT, DESC_COUNT - 1);

  // 2. Ring TX (1 página)
  fd_buf = ioctl(fd_dma, DMA_ALLOC, &req);
  tx_ring = mmap(NULL, req.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_buf, 0);
  mmio_wr(E1000_TDBAL, (uint32_t)req.phys);
  mmio_wr(E1000_TDBAH, (uint32_t)(req.phys >> 32));
  mmio_wr(E1000_TDLEN, DESC_COUNT * sizeof(tx_desc_t));
  mmio_wr(E1000_TDH, 0);
  mmio_wr(E1000_TDT, 0);

  // 3. Buffers RX
  req.size = DESC_COUNT * BUF_SIZE;
  fd_buf = ioctl(fd_dma, DMA_ALLOC, &req);
  rx_bufs = mmap(NULL, req.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_buf, 0);
  for (int i = 0; i < DESC_COUNT; i++) {
    rx_ring[i].addr = req.phys + (i * BUF_SIZE);
    rx_ring[i].status = 0;
  }

  // 4. Buffers TX
  fd_buf = ioctl(fd_dma, DMA_ALLOC, &req);
  tx_bufs = mmap(NULL, req.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_buf, 0);
  for (int i = 0; i < DESC_COUNT; i++) {
    tx_ring[i].addr = req.phys + (i * BUF_SIZE);
    tx_ring[i].status = TX_DD;
  }

  // 5. Habilitar motores del e1000e
  mmio_wr(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);
  mmio_wr(E1000_TCTL,
          E1000_TCTL_EN | E1000_TCTL_PSP | (0x10 << 4) | (0x40 << 12));

  // 6. Habilitar todas las interrupciones (MSI/Legacy)
  mmio_wr(E1000_IMS, 0xFFFFFFFF);
}

// ============================================================================
// Main
// ============================================================================
int main(void) {
  printf("[e1000e.aur] Iniciando driver en Ring 3...\n");

  // 1. Abrir /dev/net0 (TUN/TAP virtual)
  fd_net0 = open("/dev/net0", O_RDWR);
  if (fd_net0 < 0) {
    perror("open /dev/net0");
    return 1;
  }

  // 2. Abrir y mapear MMIO (BAR0)
  int fd_mmio =
      open("/sys/bus/pci/devices/0000:00:02.0/resource0", O_RDWR | O_SYNC);
  if (fd_mmio < 0) {
    perror("open resource0");
    return 1;
  }
  mmio = mmap(NULL, 0x20000, PROT_READ | PROT_WRITE, MAP_SHARED, fd_mmio, 0);
  if (mmio == MAP_FAILED) {
    perror("mmap mmio");
    return 1;
  }

  // 3. Abrir /dev/dma
  int fd_dma = open("/dev/dma", O_RDWR);
  if (fd_dma < 0) {
    perror("open /dev/dma");
    return 1;
  }

  // 4. Reset del dispositivo e1000e
  mmio_wr(E1000_CTRL, E1000_CTRL_RST);
  usleep(10000); // Esperar a que el reset se complete
  mmio_wr(E1000_CTRL, mmio_rd(E1000_CTRL) | E1000_CTRL_SLU | E1000_CTRL_FD);

  // 5. Configurar los Rings DMA
  setup_rings(fd_dma);
  printf("[e1000e.aur] Rings DMA configurados\n");

  // 6. Abrir IRQ (QEMU e1000e usa IRQ 11 en modo legacy)
  fd_irq = open("/dev/irq/11", O_RDONLY);
  if (fd_irq < 0) {
    perror("open /dev/irq/11");
    return 1;
  }

  // 7. Configurar epoll para multiplexar TX (net0) y RX (IRQ)
  int epfd = epoll_create1(0);
  struct epoll_event ev, events[2];

  ev.events = EPOLLIN;
  ev.data.fd = fd_net0;
  epoll_ctl(epfd, EPOLL_CTL_ADD, fd_net0, &ev);

  ev.events = EPOLLIN;
  ev.data.fd = fd_irq;
  epoll_ctl(epfd, EPOLL_CTL_ADD, fd_irq, &ev);

  printf("[e1000e.aur] Bucle principal iniciado. Notificando al kernel...\n");

  // 8. Despertar al DHCP del kernel para que empiece a enviar paquetes
  // (El hilo de DHCP del kernel se lanzó antes que nosotros y podría estar
  // dormido/abortado)
  ioctl(fd_net0, DHCP_RESTART, 0);

  uint32_t rx_tail = 0, tx_head = 0;

  while (1) {
    // [FIX] Desenmascarar la IRQ en cada iteración.
    // AHCI la enmascara durante su init (que ocurre después de nuestro open),
    // así que nos aseguramos de que siempre esté activa antes de dormirnos.
    ioctl(fd_irq, AUR_IRQ_UNMASK, 0);

    int n = epoll_wait(epfd, events, 2, -1);
    for (int i = 0; i < n; i++) {
      // ---------------------------------------------------------
      // TX: El kernel quiere enviar un paquete (DHCP, TCP, etc.)
      // ---------------------------------------------------------
      if (events[i].data.fd == fd_net0) {
        uint8_t tmp[2048];
        int len = read(fd_net0, tmp, sizeof(tmp));
        if (len > 0) {
          // printf("[e1000e.aur] TX len=%d\n", len); // Comentado para no
          // spamear
          memcpy(tx_bufs + (tx_head * BUF_SIZE), tmp, len);
          tx_ring[tx_head].length = len;
          tx_ring[tx_head].cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
          tx_ring[tx_head].status = 0;
          tx_head = (tx_head + 1) % DESC_COUNT;
          mmio_wr(E1000_TDT, tx_head);
        }
      }
      // ---------------------------------------------------------
      // RX: El hardware tiene paquetes (Respuestas DHCP, TCP, etc.)
      // ---------------------------------------------------------
      else if (events[i].data.fd == fd_irq) {
        // Ack de la IRQ en el kernel
        uint32_t irq_event[2];
        int64_t irq_bytes;
        do {
          irq_bytes = read(fd_irq, irq_event, sizeof(irq_event));
        } while (irq_bytes < 0 && errno == EINTR);
        if (irq_bytes < 0) {
          perror("[e1000e.aur] read IRQ");
          continue;
        }
        if ((size_t)irq_bytes != sizeof(irq_event)) {
          fprintf(stderr, "[e1000e.aur] lectura IRQ incompleta: %lld bytes\n",
                  (long long)irq_bytes);
          continue;
        }

        // Leer Interrupt Cause Register (limpia los bits de interrupción)
        uint32_t icr = mmio_rd(E1000_ICR);

        // RXT0 (0x80) = RX Timer, RXDMT0 (0x10) = RX Descriptor Min Threshold
        if (icr & (0x80 | 0x10 | 0x40)) {
          while (rx_ring[rx_tail].status & RX_DD) {
            int len = rx_ring[rx_tail].length;
            if (len > 0) {
              // printf("[e1000e.aur] RX len=%d\n", len); // Comentado para no
              // spamear Escribir el paquete recibido al kernel
              int64_t written;
              do {
                written =
                    write(fd_net0, rx_bufs + (rx_tail * BUF_SIZE), (size_t)len);
              } while (written < 0 && errno == EINTR);
              if (written < 0) {
                perror("[e1000e.aur] write /dev/net0");
              } else if (written != len) {
                fprintf(stderr,
                        "[e1000e.aur] paquete RX incompleto: %lld de %d bytes\n",
                        (long long)written, len);
              }
            }
            // Devolver el descriptor al hardware
            rx_ring[rx_tail].status = 0;
            rx_tail = (rx_tail + 1) % DESC_COUNT;
            mmio_wr(E1000_RDT, (rx_tail - 1) % DESC_COUNT);
          }
        }
      }
    }
  }
  return 0;
}