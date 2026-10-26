// drivers/aur/virtio_gpu/virtio_gpu.c
//
// [FASE 6.3] virtio_gpu.aur — driver virtio-gpu 2D (virtio 1.x moderno sobre
// PCI) en Ring 3.
//
// El firmware (OVMF/GOP) deja un framebuffer en RAM que el compositor del
// kernel sigue dibujando; en un virtio-gpu "puro" el host no lo escanea por
// su cuenta. Este driver crea un recurso 2D cuyo *backing* es ese mismo
// framebuffer (GPUP_INFO) y lo fija como scanout 0. Por cada rectángulo sucio
// que acumula el kernel (GPUP_WAIT_PRESENT) emite TRANSFER_TO_HOST_2D +
// RESOURCE_FLUSH. Si el dispositivo ofrece VIRTIO_GPU_F_EDID, el EDID real se
// registra para que DRM lo exponga en la propiedad EDID del conector.
//
// Solo primitivas de usuario:
//   - /sys/bus/pci/devices/<bdf>/{config,resourceN}   MMIO (AUR_CAP_MMIO)
//   - /dev/dma                                        colas y comandos
//   - /dev/gpuproxy                                   framebuffer + presentacion
// Sin interrupciones: las respuestas se sondean (comandos de ~microsegundos).

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// ABI con el kernel (copia de kernel/aur.h)
// ---------------------------------------------------------------------------
#define GPUP_INFO 0x800
#define GPUP_REGISTER 0x801
#define GPUP_WAIT_PRESENT 0x802
#define GPUP_ALLOC_FB 0x803
#define AUR_GPU_NEEDS_PRESENT (1u << 0)
#define AUR_GPU_MAX_MODES 16
struct aur_gpu_info {
  uint64_t fb_phys, fb_size;
  uint32_t width, height, pitch, bpp;
};
struct aur_gpu_mode {
  uint16_t width, height, refresh, flags;
};
struct aur_gpu_reg {
  char name[32];
  uint32_t flags, n_modes;
  struct aur_gpu_mode modes[AUR_GPU_MAX_MODES];
  uint32_t edid_len, _pad;
  uint8_t edid[128];
};
struct aur_gpu_present {
  uint32_t x, y, w, h, flags, seq;
};
_Static_assert(sizeof(struct aur_gpu_info) == 32, "ABI aur_gpu_info");
_Static_assert(sizeof(struct aur_gpu_reg) == 304, "ABI aur_gpu_reg");
_Static_assert(sizeof(struct aur_gpu_present) == 24, "ABI aur_gpu_present");

#define DMA_ALLOC 0x100
struct dma_alloc_req {
  uint64_t size, phys;
  uint32_t flags, _pad;
};

// ---------------------------------------------------------------------------
// virtio-pci moderno
// ---------------------------------------------------------------------------
#define PCI_SYSFS "/sys/bus/pci/devices"
#define VIRTIO_VENDOR 0x1af4
#define VIRTIO_GPU_DEVICE 0x1050

#define VIRTIO_PCI_CAP_COMMON 1
#define VIRTIO_PCI_CAP_NOTIFY 2
#define VIRTIO_PCI_CAP_ISR 3
#define VIRTIO_PCI_CAP_DEVICE 4

#define ST_ACK 1
#define ST_DRIVER 2
#define ST_DRIVER_OK 4
#define ST_FEATURES_OK 8
#define ST_FAILED 0x80

#define VIRTIO_F_VERSION_1 32 // bit de feature (palabra alta, bit 0)
#define VIRTIO_GPU_F_EDID 1

struct vcommon { // virtio_pci_common_cfg
  uint32_t device_feature_select, device_feature;
  uint32_t driver_feature_select, driver_feature;
  uint16_t msix_config, num_queues;
  uint8_t device_status, config_generation;
  uint16_t queue_select, queue_size, queue_msix_vector, queue_enable,
      queue_notify_off;
  uint64_t queue_desc, queue_driver, queue_device;
} __attribute__((packed));

static volatile struct vcommon *g_common;
static volatile uint8_t *g_notify;
static uint32_t g_notify_mult;
static uint16_t g_notify_off;

// Cola de control (split virtqueue).
#define QSZ 16
struct vdesc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags, next;
};
#define VQ_DESC_F_NEXT 1
#define VQ_DESC_F_WRITE 2
static struct vdesc *g_desc;
static volatile uint16_t *g_avail; // flags, idx, ring[]
static volatile uint16_t *g_used;  // flags, idx, ring[] (id u32, len u32)
static uint16_t g_avail_idx, g_last_used;
static uint8_t *g_cmd;  // petición (device-readable)
static uint8_t *g_resp; // respuesta (device-writable)
static uint64_t g_cmd_phys, g_resp_phys;
#define CMD_MAX 4096
#define RESP_MAX 4096

// ---------------------------------------------------------------------------
// virtio-gpu
// ---------------------------------------------------------------------------
enum {
  CMD_GET_DISPLAY_INFO = 0x0100,
  CMD_RESOURCE_CREATE_2D = 0x0101,
  CMD_SET_SCANOUT = 0x0103,
  CMD_RESOURCE_FLUSH = 0x0104,
  CMD_TRANSFER_TO_HOST_2D = 0x0105,
  CMD_RESOURCE_ATTACH_BACKING = 0x0106,
  CMD_GET_EDID = 0x010a,
  RESP_OK_NODATA = 0x1100,
  RESP_OK_DISPLAY_INFO = 0x1101,
  RESP_OK_EDID = 0x1104,
};
#define FORMAT_B8G8R8X8_UNORM 2

struct ghdr {
  uint32_t type, flags;
  uint64_t fence_id;
  uint32_t ctx_id, padding;
};
struct grect {
  uint32_t x, y, w, h;
};

// Envia g_cmd[0..len) y espera la respuesta (resp_len bytes max). Devuelve el
// tipo de respuesta o 0 si hay timeout.
static uint32_t submit(uint32_t len, uint32_t resp_len) {
  memset(g_resp, 0, 64);
  g_desc[0].addr = g_cmd_phys;
  g_desc[0].len = len;
  g_desc[0].flags = VQ_DESC_F_NEXT;
  g_desc[0].next = 1;
  g_desc[1].addr = g_resp_phys;
  g_desc[1].len = resp_len;
  g_desc[1].flags = VQ_DESC_F_WRITE;
  g_desc[1].next = 0;
  __sync_synchronize();
  g_avail[2 + (g_avail_idx % QSZ)] = 0; // cadena que empieza en el descriptor 0
  __sync_synchronize();
  g_avail[1] = ++g_avail_idx;
  __sync_synchronize();
  *(volatile uint16_t *)(g_notify + g_notify_off * g_notify_mult) = 0;
  for (int i = 0; i < 200000; i++) {
    __sync_synchronize();
    if (g_used[1] != g_last_used) {
      g_last_used = g_used[1];
      return ((struct ghdr *)g_resp)->type;
    }
    if (i > 100)
      usleep(50);
  }
  return 0;
}

static uint32_t cmd_simple(uint32_t type, const void *body, uint32_t body_len) {
  struct ghdr *h = (struct ghdr *)g_cmd;
  memset(h, 0, sizeof(*h));
  h->type = type;
  if (body_len)
    memcpy(g_cmd + sizeof(*h), body, body_len);
  return submit((uint32_t)(sizeof(*h) + body_len), sizeof(struct ghdr));
}

// ---------------------------------------------------------------------------
// PCI
// ---------------------------------------------------------------------------
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
  buf[n] = 0;
  return strtoul(buf, NULL, 16);
}

static int find_device(char *out, size_t len) {
  DIR *d = opendir(PCI_SYSFS);
  if (!d)
    return -1;
  struct dirent *e;
  int rc = -1;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.' || strlen(e->d_name) >= len)
      continue;
    if (read_sysfs_hex(e->d_name, "vendor") == VIRTIO_VENDOR &&
        read_sysfs_hex(e->d_name, "device") == VIRTIO_GPU_DEVICE) {
      strcpy(out, e->d_name);
      rc = 0;
      break;
    }
  }
  closedir(d);
  return rc;
}

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

struct vcap {
  int found;
  uint8_t bar;
  uint32_t off, len;
};
static uint8_t *g_bar[6];
static uint64_t g_bar_len[6];

static void *map_cap(const char *bdf, const struct vcap *c) {
  if (c->bar > 5)
    return NULL;
  if (!g_bar[c->bar]) {
    char path[512];
    snprintf(path, sizeof(path), PCI_SYSFS "/%s/resource%d", bdf, c->bar);
    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0)
      return NULL;
    // Se mapea hasta el final de la region mas alta que usamos de ese BAR.
    uint64_t need = ((uint64_t)c->off + c->len + 0xFFF) & ~0xFFFULL;
    if (need < 0x1000)
      need = 0x1000;
    void *p = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED)
      return NULL;
    g_bar[c->bar] = p;
    g_bar_len[c->bar] = need;
  }
  if ((uint64_t)c->off + c->len > g_bar_len[c->bar]) {
    // La region cae mas alla de lo mapeado: remapear mas grande.
    char path[512];
    snprintf(path, sizeof(path), PCI_SYSFS "/%s/resource%d", bdf, c->bar);
    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0)
      return NULL;
    uint64_t need = ((uint64_t)c->off + c->len + 0xFFF) & ~0xFFFULL;
    void *p = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED)
      return NULL;
    g_bar[c->bar] = p;
    g_bar_len[c->bar] = need;
  }
  return g_bar[c->bar] + c->off;
}

static int dma_alloc(int fd_dma, uint64_t size, uint8_t **virt, uint64_t *phys) {
  struct dma_alloc_req req;
  memset(&req, 0, sizeof(req));
  req.size = size;
  int fd = ioctl(fd_dma, DMA_ALLOC, &req);
  if (fd < 0)
    return -1;
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED)
    return -1;
  memset(p, 0, size);
  *virt = p;
  *phys = req.phys;
  return 0;
}

int main(int argc, char **argv) {
  printf("[virtio_gpu.aur] Iniciando driver en Ring 3...\n");
  char bdf[64];
  if (argc > 1 && strlen(argv[1]) < sizeof(bdf))
    strcpy(bdf, argv[1]);
  else if (find_device(bdf, sizeof(bdf)) != 0) {
    fprintf(stderr, "[virtio_gpu.aur] no hay 1af4:1050 en sysfs\n");
    return 1;
  }
  uint8_t cfg[256];
  if (read_config(bdf, cfg) != 0)
    fprintf(stderr, "[virtio_gpu.aur] %s: bus mastering no habilitado\n", bdf);

  // 1. Capacidades virtio (lista de capabilities del config space).
  struct vcap caps[5];
  memset(caps, 0, sizeof(caps));
  for (uint32_t p = cfg[0x34]; p >= 0x40 && p < 0xF0;) {
    uint8_t id = cfg[p], next = cfg[p + 1], len = cfg[p + 2];
    if (id == 0x09 && len >= 16) {
      uint8_t type = cfg[p + 3];
      if (type >= 1 && type <= 4 && !caps[type].found) {
        caps[type].found = 1;
        caps[type].bar = cfg[p + 4];
        caps[type].off = *(uint32_t *)&cfg[p + 8];
        caps[type].len = *(uint32_t *)&cfg[p + 12];
        if (type == VIRTIO_PCI_CAP_NOTIFY && len >= 20)
          g_notify_mult = *(uint32_t *)&cfg[p + 16];
      }
    }
    if (next == 0 || next == p)
      break;
    p = next;
  }
  if (!caps[VIRTIO_PCI_CAP_COMMON].found || !caps[VIRTIO_PCI_CAP_NOTIFY].found) {
    fprintf(stderr, "[virtio_gpu.aur] sin capabilities virtio modernas\n");
    return 1;
  }
  g_common = map_cap(bdf, &caps[VIRTIO_PCI_CAP_COMMON]);
  g_notify = map_cap(bdf, &caps[VIRTIO_PCI_CAP_NOTIFY]);
  if (!g_common || !g_notify) {
    fprintf(stderr, "[virtio_gpu.aur] no se pudo mapear el BAR\n");
    return 1;
  }

  int fd_dma = open("/dev/dma", O_RDWR);
  int fd_gpu = open("/dev/gpuproxy", O_RDWR);
  if (fd_dma < 0 || fd_gpu < 0) {
    perror("[virtio_gpu.aur] open /dev/dma o /dev/gpuproxy");
    return 1;
  }
  // 2. Inicializacion del dispositivo (virtio 1.0, 3.1.1).
  g_common->device_status = 0;
  for (int i = 0; i < 1000 && g_common->device_status; i++)
    usleep(1000);
  g_common->device_status = ST_ACK;
  g_common->device_status = ST_ACK | ST_DRIVER;
  g_common->device_feature_select = 0;
  uint32_t f_lo = g_common->device_feature;
  g_common->device_feature_select = 1;
  uint32_t f_hi = g_common->device_feature;
  if (!(f_hi & (1u << (VIRTIO_F_VERSION_1 - 32)))) {
    fprintf(stderr, "[virtio_gpu.aur] el dispositivo no ofrece VERSION_1\n");
    return 1;
  }
  int have_edid = (f_lo >> VIRTIO_GPU_F_EDID) & 1;
  g_common->driver_feature_select = 0;
  g_common->driver_feature = have_edid ? (1u << VIRTIO_GPU_F_EDID) : 0;
  g_common->driver_feature_select = 1;
  g_common->driver_feature = 1u << (VIRTIO_F_VERSION_1 - 32);
  g_common->device_status = ST_ACK | ST_DRIVER | ST_FEATURES_OK;
  if (!(g_common->device_status & ST_FEATURES_OK)) {
    fprintf(stderr, "[virtio_gpu.aur] FEATURES_OK rechazado\n");
    return 1;
  }

  // 3. Cola 0 (controlq): descriptores, anillo avail y used en una pagina.
  uint8_t *qmem;
  uint64_t qphys;
  if (dma_alloc(fd_dma, 4096, &qmem, &qphys) != 0 ||
      dma_alloc(fd_dma, CMD_MAX, &g_cmd, &g_cmd_phys) != 0 ||
      dma_alloc(fd_dma, RESP_MAX, &g_resp, &g_resp_phys) != 0) {
    perror("[virtio_gpu.aur] dma_alloc");
    return 1;
  }
  g_common->queue_select = 0;
  uint16_t qsz = g_common->queue_size;
  if (qsz == 0) {
    fprintf(stderr, "[virtio_gpu.aur] cola 0 inexistente\n");
    return 1;
  }
  if (qsz > QSZ)
    g_common->queue_size = qsz = QSZ;
  g_desc = (struct vdesc *)qmem;
  g_avail = (volatile uint16_t *)(qmem + 1024);
  g_used = (volatile uint16_t *)(qmem + 2048);
  g_common->queue_desc = qphys;
  g_common->queue_driver = qphys + 1024;
  g_common->queue_device = qphys + 2048;
  g_notify_off = g_common->queue_notify_off;
  g_common->queue_enable = 1;
  g_common->device_status = ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK;

  // 3b. Framebuffer: el del firmware (GOP lineal) o, si no hay (GOP "BltOnly"
  // de virtio-gpu), uno en RAM que reserva el kernel a peticion nuestra con la
  // resolucion preferida del scanout 0.
  struct aur_gpu_info info;
  memset(&info, 0, sizeof(info));
  int have_info = 0;
  for (int i = 0; i < 20 && !have_info; i++) {
    if (ioctl(fd_gpu, GPUP_INFO, &info) == 0)
      have_info = 1;
    else
      usleep(100000);
  }
  if (!have_info) {
    uint32_t dw = 1280, dh = 800;
    struct ghdr *h0 = (struct ghdr *)g_cmd;
    memset(h0, 0, sizeof(*h0));
    h0->type = CMD_GET_DISPLAY_INFO;
    if (submit((uint32_t)sizeof(*h0), 24 + 16 * 24) == RESP_OK_DISPLAY_INFO) {
      uint32_t rw = *(uint32_t *)(g_resp + 24 + 8);
      uint32_t rh = *(uint32_t *)(g_resp + 24 + 12);
      uint32_t en = *(uint32_t *)(g_resp + 24 + 16);
      printf("[virtio_gpu.aur] scanout 0: %ux%u enabled=%u\n", rw, rh, en);
      if (rw >= 640 && rh >= 480 && rw <= 4096 && rh <= 4096) {
        dw = rw;
        dh = rh;
      }
    }
    memset(&info, 0, sizeof(info));
    info.width = dw;
    info.height = dh;
    if (ioctl(fd_gpu, GPUP_ALLOC_FB, &info) != 0) {
      perror("[virtio_gpu.aur] GPUP_ALLOC_FB");
      return 1;
    }
  }
  printf("[virtio_gpu.aur] framebuffer %ux%u pitch=%u fb=0x%llx\n", info.width,
         info.height, info.pitch, (unsigned long long)info.fb_phys);

  // 4. Recurso 2D con el framebuffer como backing + scanout 0.
  uint32_t res_w = info.pitch / 4, res_h = info.height;
  struct {
    uint32_t id, format, w, h;
  } create = {1, FORMAT_B8G8R8X8_UNORM, res_w, res_h};
  if (cmd_simple(CMD_RESOURCE_CREATE_2D, &create, sizeof(create)) !=
      RESP_OK_NODATA) {
    fprintf(stderr, "[virtio_gpu.aur] RESOURCE_CREATE_2D fallo\n");
    return 1;
  }
  struct {
    uint32_t id, nr;
    uint64_t addr;
    uint32_t len, pad;
  } attach = {1, 1, info.fb_phys, (uint32_t)info.fb_size, 0};
  if (cmd_simple(CMD_RESOURCE_ATTACH_BACKING, &attach, sizeof(attach)) !=
      RESP_OK_NODATA) {
    fprintf(stderr, "[virtio_gpu.aur] ATTACH_BACKING fallo\n");
    return 1;
  }
  struct {
    struct grect r;
    uint32_t scanout, res;
  } so = {{0, 0, info.width, info.height}, 0, 1};
  if (cmd_simple(CMD_SET_SCANOUT, &so, sizeof(so)) != RESP_OK_NODATA) {
    fprintf(stderr, "[virtio_gpu.aur] SET_SCANOUT fallo\n");
    return 1;
  }

  // 5. Registro del adaptador (con el EDID real si lo hay).
  struct aur_gpu_reg reg;
  memset(&reg, 0, sizeof(reg));
  snprintf(reg.name, sizeof(reg.name), "virtio_gpu");
  reg.flags = AUR_GPU_NEEDS_PRESENT;
  if (have_edid) {
    // La respuesta de GET_EDID es mayor que la cabecera: reenviar con espacio.
    struct ghdr *h = (struct ghdr *)g_cmd;
    memset(h, 0, sizeof(*h));
    h->type = CMD_GET_EDID;
    memset(g_cmd + sizeof(*h), 0, 8);
    if (submit((uint32_t)(sizeof(*h) + 8), 24 + 8 + 1024) == RESP_OK_EDID) {
      uint32_t size = *(uint32_t *)(g_resp + 24);
      if (size >= 128 && g_resp[32] == 0 && g_resp[33] == 0xFF) {
        memcpy(reg.edid, g_resp + 32, 128);
        reg.edid_len = 128;
      }
    }
  }
  reg.n_modes = 1;
  reg.modes[0].width = (uint16_t)info.width;
  reg.modes[0].height = (uint16_t)info.height;
  reg.modes[0].refresh = 60;
  reg.modes[0].flags = 1;
  if (ioctl(fd_gpu, GPUP_REGISTER, &reg) != 0) {
    perror("[virtio_gpu.aur] GPUP_REGISTER");
    return 1;
  }
  printf("[virtio_gpu.aur] scanout 0 activo (edid=%u). Presentando...\n",
         reg.edid_len);
  fflush(stdout);

  // 6. Bucle de presentacion.
  for (;;) {
    struct aur_gpu_present p;
    memset(&p, 0, sizeof(p));
    if (ioctl(fd_gpu, GPUP_WAIT_PRESENT, &p) != 0) {
      if (errno == EINTR)
        continue;
      perror("[virtio_gpu.aur] GPUP_WAIT_PRESENT");
      return 1;
    }
    struct {
      struct grect r;
      uint64_t offset;
      uint32_t res, pad;
    } tr = {{p.x, p.y, p.w, p.h},
            (uint64_t)p.y * info.pitch + (uint64_t)p.x * 4, 1, 0};
    if (cmd_simple(CMD_TRANSFER_TO_HOST_2D, &tr, sizeof(tr)) != RESP_OK_NODATA)
      fprintf(stderr, "[virtio_gpu.aur] TRANSFER fallo\n");
    struct {
      struct grect r;
      uint32_t res, pad;
    } fl = {{p.x, p.y, p.w, p.h}, 1, 0};
    if (cmd_simple(CMD_RESOURCE_FLUSH, &fl, sizeof(fl)) != RESP_OK_NODATA)
      fprintf(stderr, "[virtio_gpu.aur] FLUSH fallo\n");
  }
}
