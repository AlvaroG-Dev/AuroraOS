// tools/e1000e_aur.c
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#define NETP_SET_MAC 0x201

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
#define E1000_RAL0 0x5400
#define E1000_RAH0 0x5404
#define E1000_RAH_AV (1u << 31)

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
// Descubrimiento del dispositivo (sin BDF ni IRQ fijos)
// ============================================================================
#define PCI_SYSFS "/sys/bus/pci/devices"
#define E1000E_VENDOR 0x8086
#define E1000E_DEVICE 0x10d3

static unsigned long read_sysfs_hex(const char *bdf, const char *attr) {
  char path[512], buf[32];
  snprintf(path, sizeof(path), PCI_SYSFS "/%s/%s", bdf, attr);
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return (unsigned long)-1;
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return (unsigned long)-1;
  buf[n] = '\0';
  return strtoul(buf, NULL, 16);
}

// Busca el primer 8086:10d3 en sysfs y copia su BDF en out.
static int find_device(char *out, size_t len) {
  DIR *d = opendir(PCI_SYSFS);
  if (!d)
    return -1;
  struct dirent *e;
  int rc = -1;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.' || strlen(e->d_name) >= len)
      continue;
    if (read_sysfs_hex(e->d_name, "vendor") == E1000E_VENDOR &&
        read_sysfs_hex(e->d_name, "device") == E1000E_DEVICE) {
      strcpy(out, e->d_name);
      rc = 0;
      break;
    }
  }
  closedir(d);
  return rc;
}

// Lee el config space (256 B). aur_bus ya habilitó MEM + bus mastering
// antes de lanzarnos; esperamos a verlo por si hay carrera.
static int read_config(const char *bdf, uint8_t *cfg) {
  char path[512];
  snprintf(path, sizeof(path), PCI_SYSFS "/%s/config", bdf);
  for (int i = 0; i < 300; i++) {
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
      ssize_t n = read(fd, cfg, 256);
      close(fd);
      if (n >= 0x40 && (cfg[4] & 0x06) == 0x06)
        return 0;
    }
    usleep(10000);
  }
  return -1;
}

// ============================================================================
// RX: vacía el anillo hacia el kernel
// ============================================================================
static uint32_t rx_tail;

static void rx_drain(void) {
  while (rx_ring[rx_tail].status & RX_DD) {
    int len = rx_ring[rx_tail].length;
    if (len > 0) {
      int64_t written;
      do {
        written = write(fd_net0, rx_bufs + (rx_tail * BUF_SIZE), (size_t)len);
      } while (written < 0 && errno == EINTR);
      if (written < 0)
        perror("[e1000e.aur] write /dev/net0");
      else if (written != len)
        fprintf(stderr, "[e1000e.aur] paquete RX incompleto\n");
    }
    // Devolver el descriptor al hardware
    rx_ring[rx_tail].status = 0;
    rx_tail = (rx_tail + 1) % DESC_COUNT;
    mmio_wr(E1000_RDT, (rx_tail + DESC_COUNT - 1) % DESC_COUNT);
  }
}

// ============================================================================
// Main
// ============================================================================
int main(void) {
  printf("[e1000e.aur] Iniciando driver en Ring 3...\n");

  // 1. Localizar el NIC y leer su IRQ del config space (offset 0x3C).
  char bdf[64];
  if (find_device(bdf, sizeof(bdf)) != 0) {
    fprintf(stderr, "[e1000e.aur] no hay 8086:10d3 en sysfs\n");
    return 1;
  }
  uint8_t cfg[256];
  if (read_config(bdf, cfg) != 0)
    fprintf(stderr, "[e1000e.aur] %s: bus mastering no habilitado\n", bdf);
  int irq = cfg[0x3C];
  printf("[e1000e.aur] NIC en %s, IRQ %d\n", bdf, irq);

  // 2. Abrir /dev/net0 (TUN/TAP virtual)
  fd_net0 = open("/dev/net0", O_RDWR);
  if (fd_net0 < 0) {
    perror("open /dev/net0");
    return 1;
  }

  // 3. Abrir y mapear MMIO (BAR0)
  char path[512];
  snprintf(path, sizeof(path), PCI_SYSFS "/%s/resource0", bdf);
  int fd_mmio = open(path, O_RDWR | O_SYNC);
  if (fd_mmio < 0) {
    perror("open resource0");
    return 1;
  }
  mmio = mmap(NULL, 0x20000, PROT_READ | PROT_WRITE, MAP_SHARED, fd_mmio, 0);
  if (mmio == MAP_FAILED) {
    perror("mmap mmio");
    return 1;
  }

  // 4. Abrir /dev/dma
  int fd_dma = open("/dev/dma", O_RDWR);
  if (fd_dma < 0) {
    perror("open /dev/dma");
    return 1;
  }

  // 5. Reset del dispositivo e1000e
  mmio_wr(E1000_CTRL, E1000_CTRL_RST);
  usleep(10000); // Esperar a que el reset se complete
  mmio_wr(E1000_CTRL, mmio_rd(E1000_CTRL) | E1000_CTRL_SLU | E1000_CTRL_FD);

  // 6. MAC real: tras el reset RAL0/RAH0 se recargan de la NVM. Aseguramos
  // el bit Address Valid y se la comunicamos al kernel.
  uint32_t ral = mmio_rd(E1000_RAL0), rah = mmio_rd(E1000_RAH0);
  uint8_t mac[6] = {ral, ral >> 8, ral >> 16, ral >> 24, rah, rah >> 8};
  mmio_wr(E1000_RAH0, rah | E1000_RAH_AV);
  if (ioctl(fd_net0, NETP_SET_MAC, mac) != 0)
    perror("[e1000e.aur] NETP_SET_MAC");
  else
    printf("[e1000e.aur] MAC %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);

  // 7. Configurar los Rings DMA
  setup_rings(fd_dma);
  printf("[e1000e.aur] Rings DMA configurados\n");

  // 8. IRQ: /dev/irq solo cubre las legacy 1..15. Si no hay IRQ usable,
  // caemos a polling (epoll con timeout) en vez de fallar.
  fd_irq = -1;
  if (irq >= 1 && irq <= 15) {
    snprintf(path, sizeof(path), "/dev/irq/%d", irq);
    fd_irq = open(path, O_RDONLY);
    if (fd_irq < 0)
      perror("[e1000e.aur] open irq");
  }
  if (fd_irq < 0)
    printf("[e1000e.aur] sin IRQ usable: modo polling\n");

  // 9. Configurar epoll para multiplexar TX (net0) y RX (IRQ)
  int epfd = epoll_create1(0);
  struct epoll_event ev, events[2];

  ev.events = EPOLLIN;
  ev.data.fd = fd_net0;
  epoll_ctl(epfd, EPOLL_CTL_ADD, fd_net0, &ev);

  if (fd_irq >= 0) {
    ev.events = EPOLLIN;
    ev.data.fd = fd_irq;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd_irq, &ev);
  }

  printf("[e1000e.aur] Bucle principal iniciado. Notificando al kernel...\n");

  // 10. Despertar al DHCP del kernel para que empiece a enviar paquetes
  // (El hilo de DHCP del kernel se lanzó antes que nosotros y podría estar
  // dormido/abortado)
  ioctl(fd_net0, DHCP_RESTART, 0);

  uint32_t tx_head = 0;

  while (1) {
    // Desenmascarar la IRQ en cada iteración: otros drivers (AHCI) la
    // enmascaran durante su init.
    if (fd_irq >= 0)
      ioctl(fd_irq, AUR_IRQ_UNMASK, 0);

    // Con IRQ dormimos indefinidamente; en polling, 2 ms.
    int n = epoll_wait(epfd, events, 2, fd_irq >= 0 ? -1 : 2);
    for (int i = 0; i < n; i++) {
      // TX: El kernel quiere enviar un paquete (DHCP, TCP, etc.)
      if (events[i].data.fd == fd_net0) {
        uint8_t tmp[2048];
        int len = read(fd_net0, tmp, sizeof(tmp));
        if (len > 0) {
          memcpy(tx_bufs + (tx_head * BUF_SIZE), tmp, len);
          tx_ring[tx_head].length = len;
          tx_ring[tx_head].cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
          tx_ring[tx_head].status = 0;
          tx_head = (tx_head + 1) % DESC_COUNT;
          mmio_wr(E1000_TDT, tx_head);
        }
      }
      // IRQ: ack en el kernel
      else if (events[i].data.fd == fd_irq) {
        uint32_t irq_event[2];
        int64_t irq_bytes;
        do {
          irq_bytes = read(fd_irq, irq_event, sizeof(irq_event));
        } while (irq_bytes < 0 && errno == EINTR);
        if (irq_bytes != (int64_t)sizeof(irq_event)) {
          if (irq_bytes < 0)
            perror("[e1000e.aur] read IRQ");
          continue;
        }
      }
    }

    // RX: tras cualquier despertar (IRQ, TX o timeout de polling) limpiamos
    // la causa y vaciamos el anillo. Leer ICR la limpia.
    (void)mmio_rd(E1000_ICR);
    rx_drain();
  }
  return 0;
}
