// platform.h — plataforma PC minima para SeaBIOS (F3a): PCI (puente i440FX +
// virtio-blk heredado), CMOS/RTC, i8042, puerto 0x92, depuracion 0x402.
//
// Se incluye desde main.c (necesita g_ram, g_ram_size, now_ns, g_pit...).
// Ver Plan_MiniVM.md, seccion F3.
#ifndef AVM_PLATFORM_H
#define AVM_PLATFORM_H

#include <sys/stat.h>

#define BIOS_SIZE (128u * 1024)
#define BIOS_ALIAS_GPA 0xFFFE0000ULL

static int g_plat; // 1 = modo BIOS: activa PCI, CMOS completo, i8042
static int g_bios_log;
static int g_reset_req;

// ---------------------------------------------------------------------------
// CMOS / RTC
// ---------------------------------------------------------------------------
static uint8_t g_cmos[128];
static uint8_t g_cmos_idx;

static uint8_t bcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void cmos_init(uint64_t ram_bytes) {
  memset(g_cmos, 0, sizeof(g_cmos));
  uint64_t kb = ram_bytes >> 10;
  uint64_t ext = kb > 1024 ? kb - 1024 : 0;
  if (ext > 0xFFFF)
    ext = 0xFFFF;
  g_cmos[0x15] = 0x80; // memoria base: 640 KiB
  g_cmos[0x16] = 0x02;
  g_cmos[0x17] = g_cmos[0x30] = (uint8_t)ext;
  g_cmos[0x18] = g_cmos[0x31] = (uint8_t)(ext >> 8);
  if (ram_bytes > (16ULL << 20)) {
    uint64_t blocks = (ram_bytes - (16ULL << 20)) >> 16;
    if (blocks > 0xFFFF)
      blocks = 0xFFFF;
    g_cmos[0x34] = (uint8_t)blocks;
    g_cmos[0x35] = (uint8_t)(blocks >> 8);
  }
  g_cmos[0x0B] = 0x02; // 24 h, BCD
  g_cmos[0x0D] = 0x80; // bateria buena
  g_cmos[0x14] = 0x06; // equipo: arranque/FPU
}

static uint8_t cmos_read(uint8_t idx) {
  idx &= 0x7F;
  if (idx <= 9 || idx == 0x32) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    switch (idx) {
    case 0: return bcd((unsigned)tm.tm_sec);
    case 2: return bcd((unsigned)tm.tm_min);
    case 4: return bcd((unsigned)tm.tm_hour);
    case 6: return (uint8_t)(tm.tm_wday + 1);
    case 7: return bcd((unsigned)tm.tm_mday);
    case 8: return bcd((unsigned)tm.tm_mon + 1);
    case 9: return bcd((unsigned)(tm.tm_year % 100));
    case 0x32: return bcd((unsigned)(tm.tm_year + 1900) / 100);
    default: return 0; // alarmas
    }
  }
  if (idx == 0x0A)
    return 0x26; // sin actualizacion en curso
  if (idx == 0x0C)
    return 0;
  return g_cmos[idx];
}

// ---------------------------------------------------------------------------
// i8042 (teclado + raton sin actividad)
// ---------------------------------------------------------------------------
static struct {
  uint8_t q[16], qaux[16];
  int n;
  uint8_t cfg;     // byte de configuracion
  uint8_t outport; // puerto de salida (A20...)
  int expect;      // 0 nada; 1 = escribir cfg; 2 = escribir outport; 3 = byte para el raton; 4 = argumento de comando de teclado; 5 = argumento de comando del raton
} g_kbc;

static void kbc_reset(void) {
  memset(&g_kbc, 0, sizeof(g_kbc));
  g_kbc.cfg = 0x45;
  g_kbc.outport = 0x02;
}
static void kbc_push(uint8_t b, int aux) {
  if (g_kbc.n < 16) {
    g_kbc.q[g_kbc.n] = b;
    g_kbc.qaux[g_kbc.n] = (uint8_t)aux;
    g_kbc.n++;
  }
}
static uint8_t kbc_pop(void) {
  if (!g_kbc.n)
    return 0;
  uint8_t b = g_kbc.q[0];
  g_kbc.n--;
  memmove(g_kbc.q, g_kbc.q + 1, (size_t)g_kbc.n);
  memmove(g_kbc.qaux, g_kbc.qaux + 1, (size_t)g_kbc.n);
  return b;
}
static uint8_t kbc_status(void) {
  uint8_t s = 0x14; // SYS + inhibicion del teclado desactivada
  if (g_kbc.n) {
    s |= 1;
    if (g_kbc.qaux[0])
      s |= 0x20;
  }
  return s;
}
static void kbc_cmd(uint8_t c) {
  switch (c) {
  case 0x20: kbc_push(g_kbc.cfg, 0); break;
  case 0x60: g_kbc.expect = 1; break;
  case 0xA7: g_kbc.cfg |= 0x20; break;
  case 0xA8: g_kbc.cfg &= (uint8_t)~0x20; break;
  case 0xA9: kbc_push(0x00, 0); break; // prueba de la puerta del raton
  case 0xAA: g_kbc.cfg = 0x45; kbc_push(0x55, 0); break;
  case 0xAB: kbc_push(0x00, 0); break;
  case 0xAD: g_kbc.cfg |= 0x10; break;
  case 0xAE: g_kbc.cfg &= (uint8_t)~0x10; break;
  case 0xD0: kbc_push(g_kbc.outport, 0); break;
  case 0xD1: g_kbc.expect = 2; break;
  case 0xD4: g_kbc.expect = 3; break;
  case 0xFE: g_reset_req = 1; break;
  default: break;
  }
}
static void kbc_dev_byte(uint8_t b, int aux) {
  int *e = &g_kbc.expect;
  (void)e;
  if (aux) {
    if (g_kbc.expect == 5) { // argumento de un comando del raton
      g_kbc.expect = 0;
      kbc_push(0xFA, 1);
      return;
    }
    switch (b) {
    case 0xFF: kbc_push(0xFA, 1); kbc_push(0xAA, 1); kbc_push(0x00, 1); break;
    case 0xF2: kbc_push(0xFA, 1); kbc_push(0x00, 1); break;
    case 0xE8: case 0xF3: case 0xF0: g_kbc.expect = 5; kbc_push(0xFA, 1); break;
    default: kbc_push(0xFA, 1); break;
    }
    return;
  }
  if (g_kbc.expect == 4) {
    g_kbc.expect = 0;
    kbc_push(0xFA, 0);
    return;
  }
  switch (b) {
  case 0xFF: kbc_push(0xFA, 0); kbc_push(0xAA, 0); break;
  case 0xF2: kbc_push(0xFA, 0); kbc_push(0xAB, 0); kbc_push(0x83, 0); break;
  case 0xEE: kbc_push(0xEE, 0); break;
  case 0xED: case 0xF0: case 0xF3: g_kbc.expect = 4; kbc_push(0xFA, 0); break;
  default: kbc_push(0xFA, 0); break;
  }
}
static void kbc_data(uint8_t v) {
  switch (g_kbc.expect) {
  case 1: g_kbc.cfg = v; g_kbc.expect = 0; return;
  case 2: g_kbc.outport = v; g_kbc.expect = 0; return;
  case 3: g_kbc.expect = 0; kbc_dev_byte(v, 1); return;
  default: kbc_dev_byte(v, g_kbc.expect == 5); return;
  }
}

// ---------------------------------------------------------------------------
// PCI (mecanismo 1): 00:00.0 puente i440FX, 00:03.0 virtio-blk heredado
// ---------------------------------------------------------------------------
#define VBLK_QN 128
#define VIO_BAR_SIZE 0x40u

static uint32_t g_pci_addr;
static uint8_t g_host_cfg[256];
static uint8_t g_blk_cfg[256];
static uint32_t g_blk_bar0;
static uint16_t g_blk_cmd;

static int g_disk_fd = -1;
static uint64_t g_disk_sectors;
static int g_disk_ro;

static struct {
  uint32_t guest_features, pfn;
  uint16_t qsel, notify;
  uint8_t status, isr;
  uint16_t last_avail, used_idx;
} g_vb;

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static void pci_init(void) {
  g_pci_addr = 0;
  memset(g_host_cfg, 0, sizeof(g_host_cfg));
  memset(g_blk_cfg, 0, sizeof(g_blk_cfg));
  put16(g_host_cfg + 0x00, 0x8086);
  put16(g_host_cfg + 0x02, 0x1237);
  put16(g_host_cfg + 0x04, 0x0006);
  g_host_cfg[0x08] = 0x02;
  g_host_cfg[0x0B] = 0x06; // puente de maquina
  put16(g_host_cfg + 0x2C, 0x1AF4); // "QEMU virtual machine": SeaBIOS lo busca asi
  put16(g_host_cfg + 0x2E, 0x1100);
  g_host_cfg[0x59] = 0x10;          // PAM0: RAM presente en 0xF0000
  g_host_cfg[0x72] = 0x02;          // SMRAM
  put16(g_blk_cfg + 0x00, 0x1AF4);
  put16(g_blk_cfg + 0x02, 0x1001);  // virtio-blk transicional
  g_blk_cfg[0x0B] = 0x01;           // almacenamiento
  put16(g_blk_cfg + 0x2C, 0x1AF4);
  put16(g_blk_cfg + 0x2E, 0x0002);  // tipo de dispositivo virtio: bloque
  g_blk_cfg[0x3D] = 1;              // INTA#
  g_blk_bar0 = 0;
  g_blk_cmd = 0;
  memset(&g_vb, 0, sizeof(g_vb));
}

static uint8_t pci_cfg_get(unsigned dev, unsigned off) {
  if (dev == 0)
    return g_host_cfg[off];
  if (dev == 3 && g_disk_fd >= 0) {
    if (off >= 0x10 && off < 0x14)
      return (uint8_t)((((g_blk_bar0 & ~(VIO_BAR_SIZE - 1)) | 1u) >> ((off - 0x10) * 8)) & 0xFF);
    if (off == 0x04 || off == 0x05)
      return (uint8_t)(g_blk_cmd >> ((off - 4) * 8));
    return g_blk_cfg[off];
  }
  return 0xFF;
}

static void pci_cfg_put(unsigned dev, unsigned off, uint8_t v) {
  if (dev == 0) {
    if (off >= 0x40 || off == 0x04 || off == 0x05 || off == 0x0C || off == 0x0D)
      g_host_cfg[off] = v;
    return;
  }
  if (dev == 3 && g_disk_fd >= 0) {
    if (off >= 0x10 && off < 0x14) {
      unsigned sh = (off - 0x10) * 8;
      g_blk_bar0 = (g_blk_bar0 & ~(0xFFu << sh)) | ((uint32_t)v << sh);
    } else if (off == 0x04 || off == 0x05) {
      unsigned sh = (off - 4) * 8;
      g_blk_cmd = (uint16_t)((g_blk_cmd & ~(0xFFu << sh)) | ((unsigned)v << sh));
      g_blk_cmd &= 0x0407;
    } else if (off == 0x3C || off == 0x0C || off == 0x0D) {
      g_blk_cfg[off] = v;
    }
  }
}

static uint32_t pci_data_in(unsigned port, int size) {
  if (!(g_pci_addr & 0x80000000u))
    return 0xFFFFFFFFu;
  unsigned bus = (g_pci_addr >> 16) & 0xFF, dev = (g_pci_addr >> 11) & 31,
           fn = (g_pci_addr >> 8) & 7, reg = (g_pci_addr & 0xFC) + (port - 0xCFC);
  uint32_t v = 0;
  for (int i = 0; i < size; i++) {
    uint8_t b = (bus || fn || reg + (unsigned)i > 0xFF) ? 0xFF : pci_cfg_get(dev, reg + (unsigned)i);
    v |= (uint32_t)b << (8 * i);
  }
  return v;
}

static void pci_data_out(unsigned port, uint32_t v, int size) {
  if (!(g_pci_addr & 0x80000000u))
    return;
  unsigned bus = (g_pci_addr >> 16) & 0xFF, dev = (g_pci_addr >> 11) & 31,
           fn = (g_pci_addr >> 8) & 7, reg = (g_pci_addr & 0xFC) + (port - 0xCFC);
  if (bus || fn)
    return;
  for (int i = 0; i < size && reg + (unsigned)i <= 0xFF; i++)
    pci_cfg_put(dev, reg + (unsigned)i, (uint8_t)(v >> (8 * i)));
}

// ---------------------------------------------------------------------------
// virtio-blk heredado (0.9.5): cola unica de VBLK_QN entradas, sincrono
// ---------------------------------------------------------------------------
#define VIRTIO_BLK_F_SEG_MAX (1u << 2)
#define VIRTIO_BLK_F_GEOMETRY (1u << 4)
#define VIRTIO_BLK_F_RO (1u << 5)
#define VIRTIO_BLK_F_BLK_SIZE (1u << 6)
#define VIRTIO_BLK_F_FLUSH (1u << 9)

static uint32_t vb_host_features(void) {
  return VIRTIO_BLK_F_SEG_MAX | VIRTIO_BLK_F_GEOMETRY | VIRTIO_BLK_F_BLK_SIZE |
         VIRTIO_BLK_F_FLUSH | (g_disk_ro ? VIRTIO_BLK_F_RO : 0);
}

static int vb_io_base_ok(unsigned port) {
  if (g_disk_fd < 0 || !(g_blk_cmd & 1))
    return 0;
  unsigned base = g_blk_bar0 & ~(VIO_BAR_SIZE - 1) & ~3u;
  return base && port >= base && port < base + VIO_BAR_SIZE;
}
static unsigned vb_base(void) { return g_blk_bar0 & ~(VIO_BAR_SIZE - 1) & ~3u; }

static void vb_reset(void) {
  g_vb.guest_features = g_vb.pfn = 0;
  g_vb.qsel = g_vb.notify = 0;
  g_vb.status = g_vb.isr = 0;
  g_vb.last_avail = g_vb.used_idx = 0;
}

static uint32_t vb_in(unsigned off, int size) {
  uint8_t b[VIO_BAR_SIZE];
  memset(b, 0, sizeof(b));
  uint32_t cyl = (uint32_t)(g_disk_sectors / (16 * 63));
  if (cyl > 16383)
    cyl = 16383;
  put32(b + 0x00, vb_host_features());
  put32(b + 0x04, g_vb.guest_features);
  put32(b + 0x08, g_vb.pfn);
  put16(b + 0x0C, g_vb.qsel == 0 ? VBLK_QN : 0);
  put16(b + 0x0E, g_vb.qsel);
  put16(b + 0x10, g_vb.notify);
  b[0x12] = g_vb.status;
  b[0x13] = g_vb.isr;
  memcpy(b + 0x14, &g_disk_sectors, 8); // capacidad en sectores de 512
  put32(b + 0x1C, 0);                   // size_max (no ofrecido)
  put32(b + 0x20, VBLK_QN - 2);         // seg_max
  put16(b + 0x24, (uint16_t)cyl);
  b[0x26] = 16;
  b[0x27] = 63;
  put32(b + 0x28, 512);                 // blk_size
  uint32_t v = 0;
  if (off + (unsigned)size <= sizeof(b))
    memcpy(&v, b + off, (size_t)size);
  if (off <= 0x13 && off + (unsigned)size > 0x13)
    g_vb.isr = 0; // la lectura limpia ISR
  return v;
}

static int guest_ok(uint64_t gpa, uint64_t n) { return gpa + n >= gpa && gpa + n <= g_ram_size; }

// Procesa una peticion; devuelve los bytes escritos en el huesped (incluido el estado).
static uint32_t vb_request(uint16_t head) {
  struct seg { uint64_t addr; uint32_t len; uint16_t flags; } segs[VBLK_QN];
  unsigned ns = 0;
  uint64_t desc = (uint64_t)g_vb.pfn << 12;
  uint16_t i = head;
  for (unsigned guard = 0; guard < VBLK_QN; guard++) {
    if (i >= VBLK_QN || !guest_ok(desc + 16ULL * i, 16))
      return 0;
    const uint8_t *d = g_ram + desc + 16ULL * i;
    uint64_t a; uint32_t l; uint16_t f, nx;
    memcpy(&a, d, 8); memcpy(&l, d + 8, 4); memcpy(&f, d + 12, 2); memcpy(&nx, d + 14, 2);
    if (f & 4) // INDIRECT: no se ofrece
      return 0;
    segs[ns++] = (struct seg){a, l, f};
    if (!(f & 1))
      break;
    i = nx;
  }
  if (ns < 2 || segs[0].len < 16 || (segs[0].flags & 2) || !(segs[ns - 1].flags & 2) ||
      segs[ns - 1].len < 1 || !guest_ok(segs[0].addr, 16) || !guest_ok(segs[ns - 1].addr, 1))
    return 0;
  uint32_t type; uint64_t sector;
  memcpy(&type, g_ram + segs[0].addr, 4);
  memcpy(&sector, g_ram + segs[0].addr + 8, 8);
  uint8_t status = 0; // OK
  uint32_t written = 0;
  uint64_t off = sector * 512;
  for (unsigned k = 1; k + 1 < ns && status == 0; k++) {
    struct seg *s = &segs[k];
    if (!guest_ok(s->addr, s->len)) { status = 1; break; }
    if (type == 0 || type == 8) { // IN / GET_ID: el dispositivo escribe
      if (!(s->flags & 2)) { status = 1; break; }
      if (type == 0) {
        if (off + s->len > g_disk_sectors * 512) { status = 1; break; }
        ssize_t r = pread(g_disk_fd, g_ram + s->addr, s->len, (off_t)off);
        if (r != (ssize_t)s->len) { status = 1; break; }
        off += s->len;
      } else {
        char id[20] = "aurora-vm-disk";
        memset(g_ram + s->addr, 0, s->len);
        memcpy(g_ram + s->addr, id, s->len < sizeof(id) ? s->len : sizeof(id));
      }
      written += s->len;
    } else if (type == 1) { // OUT: el dispositivo lee
      if ((s->flags & 2) || g_disk_ro || off + s->len > g_disk_sectors * 512) { status = 1; break; }
      if (pwrite(g_disk_fd, g_ram + s->addr, s->len, (off_t)off) != (ssize_t)s->len) { status = 1; break; }
      off += s->len;
    } else if (type != 4) {
      status = 2;
    }
  }
  if (type == 4) {
    if (g_disk_ro) status = 1; else if (fsync(g_disk_fd) < 0) status = 1;
  } else if (type != 0 && type != 1 && type != 8) {
    status = 2;
  }
  g_ram[segs[ns - 1].addr] = status;
  return written + 1;
}

static void vb_notify(void) {
  if (!g_vb.pfn)
    return;
  uint64_t base = (uint64_t)g_vb.pfn << 12;
  uint64_t avail = base + 16ULL * VBLK_QN;
  uint64_t used = (avail + 4 + 2ULL * VBLK_QN + 2 + 4095) & ~4095ULL;
  if (!guest_ok(avail, 4 + 2ULL * VBLK_QN) || !guest_ok(used, 4 + 8ULL * VBLK_QN))
    return;
  for (;;) {
    uint16_t aidx;
    memcpy(&aidx, g_ram + avail + 2, 2);
    if (aidx == g_vb.last_avail)
      break;
    uint16_t head;
    memcpy(&head, g_ram + avail + 4 + 2ULL * (g_vb.last_avail % VBLK_QN), 2);
    uint32_t len = vb_request(head);
    uint8_t *u = g_ram + used + 4 + 8ULL * (g_vb.used_idx % VBLK_QN);
    uint32_t id = head;
    memcpy(u, &id, 4);
    memcpy(u + 4, &len, 4);
    g_vb.used_idx++;
    g_vb.last_avail++;
    memcpy(g_ram + used + 2, &g_vb.used_idx, 2);
    g_vb.isr |= 1;
  }
}

static void vb_out(unsigned off, uint32_t v, int size) {
  (void)size;
  switch (off) {
  case 0x04: g_vb.guest_features = v; break;
  case 0x08: g_vb.pfn = v; g_vb.last_avail = g_vb.used_idx = 0; break;
  case 0x0E: g_vb.qsel = (uint16_t)v; break;
  case 0x10: g_vb.notify = (uint16_t)v; if (v == 0) vb_notify(); break;
  case 0x12:
    if ((v & 0xFF) == 0)
      vb_reset();
    else
      g_vb.status = (uint8_t)v;
    break;
  default: break;
  }
}

// ---------------------------------------------------------------------------
// Puertos de la plataforma. Devuelve 1 si el puerto es de la plataforma.
// ---------------------------------------------------------------------------
static int plat_in(uint16_t port, int size, uint32_t *v) {
  if (!g_plat)
    return 0;
  if (port == 0x71) { *v = cmos_read(g_cmos_idx); return 1; }
  if (port == 0x60) { *v = kbc_pop(); return 1; }
  if (port == 0x64) { *v = kbc_status(); return 1; }
  if (port == 0x92) { *v = 0x02; return 1; }
  if (port == 0xCF8) { *v = g_pci_addr; return 1; }
  if (port >= 0xCFC && port <= 0xCFF) { *v = pci_data_in(port, size); return 1; }
  if (vb_io_base_ok(port)) { *v = vb_in(port - vb_base(), size); return 1; }
  if (port == 0x402 || port == 0xE9) { *v = 0xE9; return 1; }
  return 0;
}

static int plat_out(uint16_t port, uint32_t v, int size) {
  if (!g_plat)
    return 0;
  if (port == 0x70) { g_cmos_idx = (uint8_t)(v & 0x7F); return 1; }
  if (port == 0x71) { g_cmos[g_cmos_idx & 0x7F] = (uint8_t)v; return 1; }
  if (port == 0x60) { kbc_data((uint8_t)v); return 1; }
  if (port == 0x64) { kbc_cmd((uint8_t)v); return 1; }
  if (port == 0x92) return 1;
  if (port == 0xCF8) { g_pci_addr = v; return 1; }
  if (port == 0xCF9) { if (v & 4) g_reset_req = 1; return 1; }
  if (port >= 0xCFC && port <= 0xCFF) { pci_data_out(port, v, size); return 1; }
  if (vb_io_base_ok(port)) { vb_out(port - vb_base(), v, size); return 1; }
  if (port == 0x402 || port == 0xE9) {
    if (g_bios_log) { fputc((int)(v & 0xFF), stderr); }
    return 1;
  }
  if (port == 0x80 || port <= 0x0F || (port >= 0x81 && port <= 0x8F) ||
      (port >= 0xC0 && port <= 0xDF) || port == 0xB2 || port == 0xB3 || port == 0x3F9 ||
      (port >= 0x3FA && port <= 0x3FF))
    return 1; // DMA, paginas DMA, SMI, POST, serie extra: se ignoran
  return 0;
}

#endif
