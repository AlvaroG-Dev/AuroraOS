// drivers/aur/ahci/ahci.c
//
// [FASE 5.2] ahci.aur — driver AHCI (SATA) en Ring 3.
//
// Usa solo primitivas de usuario:
//   - MMIO : mmap de /sys/bus/pci/devices/<BDF>/resource5 (ABAR)
//   - DMA  : /dev/dma (command list, FIS de recepción, tabla de comandos
//            y un bounce buffer de datos)
//   - Block: /dev/blkproxy (registra los discos como sdX y sirve los bios
//            del block layer con BLKP_FETCH / BLKP_COMPLETE)
//
// Diseño v1 (deliberadamente simple):
//   - Un comando a la vez, slot 0, sin NCQ.
//   - Sin interrupciones: GHC.IE=0 y PxIE=0; se hace polling de PxCI. Así
//     no dependemos de /dev/irq (que solo cubre IRQs legacy 0..15, y los
//     AHCI de q35 usan INTx > 15 o MSI).
//   - Un único controlador AHCI (el primero que aparezca en sysfs).
//
// Solo actúa si existe /usr/lib/aur/ahci.user (ver `make AHCI_USER=1`); sin
// él, el driver de kernel ahci.c sigue siendo el dueño del controlador.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// ============================================================================
// ABI con el kernel (copia de kernel/dma_user.h y kernel/aur.h: el
// toolchain de userland no ve los headers del kernel)
// ============================================================================
#define DMA_ALLOC 0x100
struct dma_alloc_req {
  uint64_t size, phys;
  uint32_t flags, _pad;
};

#define AUR_BLK_OP_READ 0
#define AUR_BLK_OP_WRITE 1
#define AUR_BLK_OP_FLUSH 2
#define AUR_BLK_RO (1u << 0)

#define BLKP_REGISTER 0x400
#define BLKP_READY 0x401
#define BLKP_FETCH 0x402
#define BLKP_COMPLETE 0x403

struct aur_blk_register {
  char name[16];
  uint64_t num_sectors;
  uint32_t sector_size;
  uint32_t max_sectors;
  uint32_t flags;
  uint32_t disk_id;
};
struct aur_blk_req {
  uint64_t tag;
  uint64_t lba;
  uint32_t disk_id;
  uint32_t op;
  uint32_t count;
  uint32_t _pad;
};
struct aur_blk_fetch {
  struct aur_blk_req req;
  uint64_t buf;
  uint64_t buf_size;
  uint32_t flags;
  uint32_t _pad;
};
struct aur_blk_done {
  uint64_t tag;
  int32_t error;
  uint32_t _pad;
  uint64_t buf;
  uint64_t buf_size;
};

_Static_assert(sizeof(struct aur_blk_register) == 40, "ABI aur_blk_register");
_Static_assert(sizeof(struct aur_blk_fetch) == 56, "ABI aur_blk_fetch");
_Static_assert(sizeof(struct aur_blk_done) == 32, "ABI aur_blk_done");

#define MARKER_PATH "/usr/lib/aur/ahci.user"
#define PCI_SYSFS "/sys/bus/pci/devices"

// ============================================================================
// Registros AHCI (spec 1.3.1)
// ============================================================================
#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_IS 0x08
#define HBA_PI 0x0C
#define HBA_CAP2 0x24
#define HBA_BOHC 0x28

#define GHC_IE (1u << 1)
#define GHC_AE (1u << 31)
#define CAP_SCLO (1u << 24)
#define CAP2_BOH (1u << 0)
#define BOHC_BOS (1u << 0)
#define BOHC_OOS (1u << 1)
#define BOHC_BB (1u << 4)

#define PORT_BASE 0x100
#define PORT_STRIDE 0x80
#define PxCLB 0x00
#define PxCLBU 0x04
#define PxFB 0x08
#define PxFBU 0x0C
#define PxIS 0x10
#define PxIE 0x14
#define PxCMD 0x18
#define PxTFD 0x20
#define PxSIG 0x24
#define PxSSTS 0x28
#define PxSERR 0x30
#define PxSACT 0x34
#define PxCI 0x38

#define CMD_ST (1u << 0)
#define CMD_CLO (1u << 3)
#define CMD_FRE (1u << 4)
#define CMD_FR (1u << 14)
#define CMD_CR (1u << 15)

#define TFD_ERR (1u << 0)
#define TFD_DRQ (1u << 3)
#define TFD_BSY (1u << 7)

#define IS_FATAL                                                               \
  ((1u << 30) | (1u << 29) | (1u << 28) | (1u << 27) | (1u << 24))
#define IS_TFES (1u << 30)

#define SIG_ATA 0x00000101u

#define ATA_CMD_READ_DMA_EXT 0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_FLUSH_EXT 0xEA
#define ATA_CMD_IDENTIFY 0xEC

// Layout dentro de la página DMA de cada puerto.
#define OFF_CLB 0      // 32 cabeceras * 32 B = 1 KiB, alineada a 1 KiB
#define OFF_FIS 1024   // 256 B, alineada a 256 B
#define OFF_CTBL 2048  // CFIS(0x80) + PRDT, alineada a 128 B
#define OFF_IDENT 3072 // 512 B
#define CTBL_PRDT 0x80

#define BOUNCE_BYTES (256u * 1024u)
#define PORTS_PER_HBA 32
#define MAX_HBAS 4
#define MAX_DISKS 8 // = BLKP_MAX_DISKS del kernel
#define CMD_TIMEOUT_MS 20000

// ============================================================================
// Estado global
// ============================================================================
struct port {
  int dead;               // irrecuperable
  int n;                  // número de puerto dentro de su HBA
  volatile uint8_t *abar; // MMIO del HBA al que pertenece
  uint32_t cap;           // HBA_CAP de ese HBA
  const char *bdf;        // para los mensajes
  uint8_t *mem;           // página DMA (virtual)
  uint64_t phys;
  uint32_t ss; // tamaño de sector lógico
  uint64_t sectors;
};

// Estado del HBA que se está inicializando (solo se usa durante el init;
// en el bucle de servicio cada puerto lleva su propio abar/cap).
static volatile uint8_t *abar;
static size_t abar_len;
static uint32_t hba_cap;

static struct port ports[MAX_HBAS * PORTS_PER_HBA];
static int nports;
static struct port *by_disk[MAX_DISKS]; // disk_id -> puerto
static int fd_dma = -1, fd_blk = -1;
static uint8_t *bounce;
static uint64_t bounce_phys;

static inline uint32_t hba_rd(uint32_t off) {
  return *(volatile uint32_t *)(abar + off);
}
static inline void hba_wr(uint32_t off, uint32_t v) {
  *(volatile uint32_t *)(abar + off) = v;
}
static inline uint32_t prd(const struct port *p, uint32_t off) {
  return *(volatile uint32_t *)(p->abar + PORT_BASE + p->n * PORT_STRIDE + off);
}
static inline void pwr(const struct port *p, uint32_t off, uint32_t v) {
  *(volatile uint32_t *)(p->abar + PORT_BASE + p->n * PORT_STRIDE + off) = v;
}

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// Espera hasta que (rd(off) & mask) == want, con timeout en ms. 0 = OK.
static int port_wait(const struct port *p, uint32_t off, uint32_t mask,
                     uint32_t want, uint32_t timeout_ms) {
  uint64_t t0 = now_ms();
  for (int spin = 0;; spin++) {
    if ((prd(p, off) & mask) == want)
      return 0;
    if (now_ms() - t0 > timeout_ms)
      return -1;
    if (spin > 2000)
      usleep(200);
  }
}

// ============================================================================
// DMA
// ============================================================================
static void *dma_alloc(size_t size, uint64_t *phys) {
  struct dma_alloc_req req;
  memset(&req, 0, sizeof(req));
  req.size = size;
  int fd = ioctl(fd_dma, DMA_ALLOC, &req);
  if (fd < 0)
    return NULL;
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED)
    return NULL;
  memset(p, 0, size);
  *phys = req.phys;
  return p;
}

// Sin CAP.S64A el HBA solo direcciona los 4 GiB bajos.
static int phys_ok(uint32_t cap, uint64_t phys, size_t len) {
  if (cap & (1u << 31))
    return 1;
  return phys + len <= 0x100000000ULL;
}

// ============================================================================
// Control del puerto
// ============================================================================
static int port_stop(struct port *p) {
  uint32_t cmd = prd(p, PxCMD);
  if (cmd & (CMD_ST | CMD_CR | CMD_FRE | CMD_FR)) {
    pwr(p, PxCMD, cmd & ~(CMD_ST | CMD_FRE));
    if (port_wait(p, PxCMD, CMD_CR | CMD_FR, 0, 500) != 0)
      return -1;
  }
  return 0;
}

static int port_start(struct port *p) {
  if (port_wait(p, PxTFD, TFD_BSY | TFD_DRQ, 0, 1000) != 0) {
    // Dispositivo atascado: Command List Override si el HBA lo soporta.
    if (!(p->cap & CAP_SCLO))
      return -1;
    pwr(p, PxCMD, prd(p, PxCMD) | CMD_CLO);
    if (port_wait(p, PxCMD, CMD_CLO, 0, 500) != 0)
      return -1;
  }
  pwr(p, PxCMD, prd(p, PxCMD) | CMD_FRE);
  pwr(p, PxCMD, prd(p, PxCMD) | CMD_ST);
  return 0;
}

// Ejecuta un comando DMA/no-data por el slot 0. 0 = OK, <0 = error.
static int port_cmd(struct port *p, uint8_t cmd, uint64_t lba, uint32_t count,
                    int write, uint64_t dphys, uint32_t bytes) {
  if (p->dead)
    return -EIO;

  uint8_t *ctbl = p->mem + OFF_CTBL;
  uint32_t *hdr = (uint32_t *)(p->mem + OFF_CLB);
  uint64_t ctbl_phys = p->phys + OFF_CTBL;

  memset(ctbl, 0, CTBL_PRDT + 16);
  ctbl[0] = 0x27; // FIS H2D
  ctbl[1] = 0x80; // C=1: registro de comando
  ctbl[2] = cmd;
  ctbl[4] = (uint8_t)lba;
  ctbl[5] = (uint8_t)(lba >> 8);
  ctbl[6] = (uint8_t)(lba >> 16);
  ctbl[7] = (cmd == ATA_CMD_IDENTIFY) ? 0 : 0x40; // LBA mode
  ctbl[8] = (uint8_t)(lba >> 24);
  ctbl[9] = (uint8_t)(lba >> 32);
  ctbl[10] = (uint8_t)(lba >> 40);
  ctbl[12] = (uint8_t)count;
  ctbl[13] = (uint8_t)(count >> 8);

  uint32_t prdtl = 0;
  if (bytes) {
    uint32_t *prdt = (uint32_t *)(ctbl + CTBL_PRDT);
    prdt[0] = (uint32_t)dphys;
    prdt[1] = (uint32_t)(dphys >> 32);
    prdt[2] = 0;
    prdt[3] = bytes - 1; // DBC es "bytes - 1"
    prdtl = 1;
  }

  hdr[0] =
      5u /* CFL: FIS de 5 dwords */ | (write ? (1u << 6) : 0) | (prdtl << 16);
  hdr[1] = 0; // PRDBC
  hdr[2] = (uint32_t)ctbl_phys;
  hdr[3] = (uint32_t)(ctbl_phys >> 32);
  hdr[4] = hdr[5] = hdr[6] = hdr[7] = 0;

  if (port_wait(p, PxTFD, TFD_BSY | TFD_DRQ, 0, 1000) != 0)
    goto fail;

  pwr(p, PxIS, 0xFFFFFFFF);
  __sync_synchronize();
  pwr(p, PxCI, 1);

  uint64_t t0 = now_ms();
  for (int spin = 0;; spin++) {
    if (!(prd(p, PxCI) & 1))
      break;
    if (prd(p, PxIS) & IS_FATAL)
      goto fail;
    if (now_ms() - t0 > CMD_TIMEOUT_MS) {
      fprintf(stderr, "[ahci.aur] puerto %d: timeout (cmd 0x%02x)\n", p->n,
              cmd);
      goto fail;
    }
    if (spin > 2000)
      usleep(100);
  }
  __sync_synchronize();

  if ((prd(p, PxIS) & IS_TFES) || (prd(p, PxTFD) & TFD_ERR))
    goto fail;
  return 0;

fail:
  // Recuperación: parar, limpiar errores, rearrancar. Si no se puede, el
  // puerto queda muerto y los bios siguientes fallan con -EIO.
  fprintf(stderr, "[ahci.aur] puerto %d: error (cmd 0x%02x lba %llu), reset\n",
          p->n, cmd, (unsigned long long)lba);
  if (port_stop(p) != 0) {
    p->dead = 1;
    return -EIO;
  }
  pwr(p, PxSERR, 0xFFFFFFFF);
  pwr(p, PxIS, 0xFFFFFFFF);
  if (port_start(p) != 0)
    p->dead = 1;
  return -EIO;
}

// ============================================================================
// Inicialización de un puerto
// ============================================================================
static void ata_string(const uint16_t *w, int first, int last, char *out) {
  int n = 0;
  for (int i = first; i <= last; i++) {
    out[n++] = (char)(w[i] >> 8);
    out[n++] = (char)(w[i] & 0xFF);
  }
  while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\0'))
    n--;
  out[n] = '\0';
}

static int port_init(struct port *p) {
  uint32_t ssts = prd(p, PxSSTS);
  if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1)
    return -ENODEV; // sin dispositivo / enlace inactivo
  if (prd(p, PxSIG) != SIG_ATA)
    return -ENODEV; // ATAPI, port multiplier...

  p->mem = dma_alloc(4096, &p->phys);
  if (!p->mem || !phys_ok(p->cap, p->phys, 4096)) {
    fprintf(stderr, "[ahci.aur] puerto %d: sin memoria DMA usable\n", p->n);
    return -ENOMEM;
  }

  if (port_stop(p) != 0) {
    fprintf(stderr, "[ahci.aur] puerto %d: no se pudo parar\n", p->n);
    return -EIO;
  }
  pwr(p, PxCLB, (uint32_t)(p->phys + OFF_CLB));
  pwr(p, PxCLBU, (uint32_t)((p->phys + OFF_CLB) >> 32));
  pwr(p, PxFB, (uint32_t)(p->phys + OFF_FIS));
  pwr(p, PxFBU, (uint32_t)((p->phys + OFF_FIS) >> 32));
  pwr(p, PxSERR, 0xFFFFFFFF);
  pwr(p, PxIS, 0xFFFFFFFF);
  pwr(p, PxIE, 0); // polling: ninguna fuente de interrupción
  if (port_start(p) != 0) {
    fprintf(stderr, "[ahci.aur] puerto %d: no arranca\n", p->n);
    return -EIO;
  }

  if (port_cmd(p, ATA_CMD_IDENTIFY, 0, 0, 0, p->phys + OFF_IDENT, 512) != 0) {
    fprintf(stderr, "[ahci.aur] puerto %d: IDENTIFY falló\n", p->n);
    return -EIO;
  }

  const uint16_t *w = (const uint16_t *)(p->mem + OFF_IDENT);
  if (!(w[83] & (1u << 10))) {
    fprintf(stderr, "[ahci.aur] puerto %d: sin LBA48, no soportado\n", p->n);
    return -ENOTSUP;
  }
  p->sectors = (uint64_t)w[100] | ((uint64_t)w[101] << 16) |
               ((uint64_t)w[102] << 32) | ((uint64_t)w[103] << 48);
  p->ss = 512;
  if ((w[106] & 0xC000) == 0x4000 && (w[106] & (1u << 12))) {
    uint32_t words = (uint32_t)w[117] | ((uint32_t)w[118] << 16);
    if (words >= 256 && words <= 2048)
      p->ss = words * 2;
  }
  if (p->sectors == 0)
    return -ENODEV;

  char model[41];
  ata_string(w, 27, 46, model);
  printf("[ahci.aur] puerto %d: %s, %llu sectores de %u B (%llu MB)\n", p->n,
         model, (unsigned long long)p->sectors, p->ss,
         (unsigned long long)(p->sectors * p->ss / (1024 * 1024)));
  return 0;
}

// ============================================================================
// Controladores
// ============================================================================
// Rellena bdfs[] con los controladores AHCI (class 0x010601) de sysfs.
static int find_controllers(char bdfs[][64], int max) {
  DIR *d = opendir(PCI_SYSFS);
  if (!d)
    return 0;
  struct dirent *e;
  int n = 0;
  while (n < max && (e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.' || strlen(e->d_name) >= 64)
      continue;
    char path[512];
    snprintf(path, sizeof(path), PCI_SYSFS "/%s/class", e->d_name);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
      continue;
    char buf[32];
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0)
      continue;
    buf[len] = '\0';
    // 0x010601 = Mass storage / SATA / AHCI 1.0
    if (strtoul(buf, NULL, 16) == 0x010601UL)
      strcpy(bdfs[n++], e->d_name);
  }
  closedir(d);
  return n;
}

// El .aur no puede escribir el config space: aur_bus habilita MEM y bus
// mastering al lanzarnos, pero como gestionamos TODOS los controladores
// puede que el probe del segundo aún no haya corrido. Esperamos (máx. 3 s).
static void wait_bus_master(const char *bdf) {
  char path[512];
  snprintf(path, sizeof(path), PCI_SYSFS "/%s/config", bdf);
  for (int i = 0; i < 300; i++) {
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
      uint8_t cfg[8];
      ssize_t n = read(fd, cfg, sizeof(cfg));
      close(fd);
      if (n == 8 && (cfg[4] & 0x06) == 0x06) // COMMAND: MEM | BUS_MASTER
        return;
    }
    usleep(10000);
  }
  fprintf(stderr, "[ahci.aur] %s: bus mastering no habilitado tras 3 s\n", bdf);
}

static int map_abar(const char *bdf) {
  char path[512];
  snprintf(path, sizeof(path), PCI_SYSFS "/%s/resource5", bdf);
  int fd = open(path, O_RDWR | O_SYNC);
  if (fd < 0) {
    perror("[ahci.aur] open resource5");
    return -1;
  }
  // Los puertos 0..29 caben en 0x1000; hasta el 31 hacen falta 0x2000.
  abar_len = 0x2000;
  void *m = mmap(NULL, abar_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (m == MAP_FAILED) {
    abar_len = 0x1000;
    m = mmap(NULL, abar_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  }
  if (m == MAP_FAILED) {
    perror("[ahci.aur] mmap ABAR");
    return -1;
  }
  abar = (volatile uint8_t *)m;
  return 0;
}

static void hba_init(void) {
  hba_wr(HBA_GHC, hba_rd(HBA_GHC) | GHC_AE);

  // BIOS/OS handoff (spec 10.6.3), solo si el HBA lo anuncia.
  if (hba_rd(HBA_CAP2) & CAP2_BOH) {
    hba_wr(HBA_BOHC, hba_rd(HBA_BOHC) | BOHC_OOS);
    uint64_t t0 = now_ms();
    while ((hba_rd(HBA_BOHC) & (BOHC_BOS | BOHC_BB)) && now_ms() - t0 < 2000)
      usleep(1000);
  }

  // Sin interrupciones del HBA (polling).
  hba_wr(HBA_GHC, (hba_rd(HBA_GHC) & ~GHC_IE) | GHC_AE);
  hba_wr(HBA_IS, 0xFFFFFFFF);
  hba_cap = hba_rd(HBA_CAP);
}

static int register_disk(struct port *p) {
  struct aur_blk_register r;
  memset(&r, 0, sizeof(r));
  r.num_sectors = p->sectors;
  r.sector_size = p->ss;
  r.max_sectors = BOUNCE_BYTES / p->ss;
  if (ioctl(fd_blk, BLKP_REGISTER, &r) != 0) {
    perror("[ahci.aur] BLKP_REGISTER");
    return -1;
  }
  if (r.disk_id >= MAX_DISKS)
    return -1;
  by_disk[r.disk_id] = p;
  printf("[ahci.aur] %s puerto %d -> /dev/%s (disk_id=%u)\n", p->bdf, p->n,
         r.name, r.disk_id);
  return 0;
}

// Inicializa un HBA y registra sus discos. Devuelve cuántos.
static int init_hba(const char *bdf) {
  wait_bus_master(bdf);
  if (map_abar(bdf) != 0)
    return 0;
  hba_init();

  uint32_t pi = hba_rd(HBA_PI);
  printf("[ahci.aur] %s CAP=0x%08x PI=0x%08x\n", bdf, hba_cap, pi);
  if (!phys_ok(hba_cap, bounce_phys, BOUNCE_BYTES)) {
    fprintf(stderr, "[ahci.aur] %s: el bounce buffer queda fuera de 4 GiB\n",
            bdf);
    return 0;
  }

  int found = 0;
  for (int i = 0; i < PORTS_PER_HBA; i++) {
    if (!(pi & (1u << i)))
      continue;
    if (PORT_BASE + (size_t)(i + 1) * PORT_STRIDE > abar_len)
      break; // ABAR mapeado demasiado corto para este puerto
    if (nports >= (int)(sizeof(ports) / sizeof(ports[0])))
      break;
    struct port *p = &ports[nports];
    memset(p, 0, sizeof(*p));
    p->n = i;
    p->abar = abar;
    p->cap = hba_cap;
    p->bdf = bdf;
    if (port_init(p) == 0 && register_disk(p) == 0) {
      nports++;
      found++;
    }
  }
  return found;
}

// ============================================================================
// Bucle de servicio
// ============================================================================
static int serve(const struct aur_blk_req *rq) {
  struct port *p = (rq->disk_id < MAX_DISKS) ? by_disk[rq->disk_id] : NULL;
  if (!p)
    return -ENODEV;
  uint32_t bytes = rq->count * p->ss;

  switch (rq->op) {
  case AUR_BLK_OP_READ:
    return port_cmd(p, ATA_CMD_READ_DMA_EXT, rq->lba, rq->count, 0, bounce_phys,
                    bytes);
  case AUR_BLK_OP_WRITE:
    return port_cmd(p, ATA_CMD_WRITE_DMA_EXT, rq->lba, rq->count, 1,
                    bounce_phys, bytes);
  case AUR_BLK_OP_FLUSH:
    return port_cmd(p, ATA_CMD_FLUSH_EXT, 0, 0, 0, 0, 0);
  default:
    return -EINVAL;
  }
}

int main(void) {
  if (access(MARKER_PATH, F_OK) != 0)
    return 0; // el driver de kernel es el dueño del AHCI

  // aur_bus lanza una instancia por controlador; solo una puede abrir
  // /dev/blkproxy y esa se encarga de todos los controladores.
  fd_blk = open("/dev/blkproxy", O_RDWR);
  if (fd_blk < 0) {
    if (errno != EBUSY)
      perror("[ahci.aur] open /dev/blkproxy");
    return errno == EBUSY ? 0 : 1;
  }
  printf("[ahci.aur] Iniciando driver AHCI en Ring 3...\n");

  static char bdfs[MAX_HBAS][64];
  int nhba = find_controllers(bdfs, MAX_HBAS);
  if (nhba == 0)
    fprintf(stderr, "[ahci.aur] no hay controlador AHCI en sysfs\n");

  fd_dma = open("/dev/dma", O_RDWR);
  if (fd_dma < 0)
    perror("[ahci.aur] open /dev/dma");
  else if (nhba > 0) {
    bounce = dma_alloc(BOUNCE_BYTES, &bounce_phys);
    if (!bounce)
      fprintf(stderr, "[ahci.aur] sin bounce buffer DMA\n");
  }

  int ndisks = 0;
  if (bounce) {
    for (int h = 0; h < nhba; h++)
      ndisks += init_hba(bdfs[h]);
  }

  // Avisar al kernel aunque no haya discos, para que el arranque no espere.
  ioctl(fd_blk, BLKP_READY, 0);
  if (ndisks == 0) {
    printf("[ahci.aur] sin discos SATA; saliendo\n");
    return 0;
  }
  printf("[ahci.aur] %d disco(s) registrados; sirviendo bios\n", ndisks);

  for (;;) {
    struct aur_blk_fetch f;
    memset(&f, 0, sizeof(f));
    f.buf = (uint64_t)(uintptr_t)bounce;
    f.buf_size = BOUNCE_BYTES;

    if (ioctl(fd_blk, BLKP_FETCH, &f) != 0) {
      if (errno == EINTR)
        continue;
      perror("[ahci.aur] BLKP_FETCH");
      return 1;
    }

    struct aur_blk_done dn;
    memset(&dn, 0, sizeof(dn));
    dn.tag = f.req.tag;
    dn.error = serve(&f.req);
    dn.buf = (uint64_t)(uintptr_t)bounce;
    dn.buf_size = BOUNCE_BYTES;
    if (ioctl(fd_blk, BLKP_COMPLETE, &dn) != 0 && errno != EINTR)
      perror("[ahci.aur] BLKP_COMPLETE");
  }
  return 0;
}
