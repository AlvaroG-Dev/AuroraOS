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

static void pic_raise(int irq);
static void pic_lower(int irq);

static int g_plat; // 1 = modo BIOS: activa PCI, CMOS completo, i8042
static int g_bios_log;
static int g_reset_req;
static char g_dbgbuf[16384];
static size_t g_dbgbuf_n;

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
static uint8_t g_isa_cfg[256];
static uint8_t g_ide_cfg[256];
static uint32_t g_blk_bar0;
static uint16_t g_blk_cmd;
static uint32_t g_ide_bar4;
static uint16_t g_ide_cmd;
static int g_ide_on; // IDE PIIX (00:01.1) + puente ISA PIIX3 (00:01.0)
static int g_acpi_on; // PM PIIX4 (00:01.3) + puente ISA: SeaBIOS genera las tablas ACPI
static int g_poweroff;
static uint8_t g_pm_cfg[256];

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
  memset(g_isa_cfg, 0, sizeof(g_isa_cfg));
  memset(g_ide_cfg, 0, sizeof(g_ide_cfg));
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
  // PIIX3: puente ISA (00:01.0), multifuncion
  put16(g_isa_cfg + 0x00, 0x8086);
  put16(g_isa_cfg + 0x02, 0x7000);
  put16(g_isa_cfg + 0x04, 0x0007);
  g_isa_cfg[0x0A] = 0x01;
  g_isa_cfg[0x0B] = 0x06;
  g_isa_cfg[0x0E] = 0x80;
  memset(g_isa_cfg + 0x60, 0x80, 4); // PIRQ A-D desactivadas
  put16(g_isa_cfg + 0x2C, 0x1AF4);
  put16(g_isa_cfg + 0x2E, 0x1100);
  // PIIX3 IDE (00:01.1): modo heredado, con maestro de bus
  put16(g_ide_cfg + 0x00, 0x8086);
  put16(g_ide_cfg + 0x02, 0x7010);
  put16(g_ide_cfg + 0x06, 0x0280);
  g_ide_cfg[0x09] = 0x80;
  g_ide_cfg[0x0A] = 0x01;
  g_ide_cfg[0x0B] = 0x01;
  put16(g_ide_cfg + 0x40, 0x8000);  // IDETIM primario/secundario: decodificacion activa
  put16(g_ide_cfg + 0x42, 0x8000);
  put16(g_ide_cfg + 0x2C, 0x1AF4);
  put16(g_ide_cfg + 0x2E, 0x1100);
  g_ide_bar4 = 0;
  g_ide_cmd = 0;
  // PIIX4 PM (00:01.3): reloj, eventos ACPI y apagado
  memset(g_pm_cfg, 0, sizeof(g_pm_cfg));
  put16(g_pm_cfg + 0x00, 0x8086);
  put16(g_pm_cfg + 0x02, 0x7113);
  put16(g_pm_cfg + 0x04, 0x0000);
  g_pm_cfg[0x08] = 0x03;
  g_pm_cfg[0x0A] = 0x80;
  g_pm_cfg[0x0B] = 0x06;
  g_pm_cfg[0x3D] = 0x01;
  g_pm_cfg[0x3C] = 0x09;
  put16(g_pm_cfg + 0x2C, 0x1AF4);
  put16(g_pm_cfg + 0x2E, 0x1100);
  memset(&g_vb, 0, sizeof(g_vb));
}

static uint8_t *pci_cfg_for(unsigned dev, unsigned fn) {
  if (dev == 0 && fn == 0) return g_host_cfg;
  if (dev == 1 && fn == 0 && (g_ide_on || g_acpi_on)) return g_isa_cfg;
  if (dev == 1 && fn == 3 && g_acpi_on) return g_pm_cfg;
  if (dev == 1 && fn == 1 && g_ide_on) return g_ide_cfg;
  if (dev == 3 && fn == 0 && g_disk_fd >= 0) return g_blk_cfg;
  return NULL;
}

static uint8_t pci_cfg_get(unsigned dev, unsigned fn, unsigned off) {
  uint8_t *c = pci_cfg_for(dev, fn);
  if (!c)
    return 0xFF;
  if (c == g_blk_cfg) {
    if (off >= 0x10 && off < 0x14)
      return (uint8_t)((((g_blk_bar0 & ~(VIO_BAR_SIZE - 1)) | 1u) >> ((off - 0x10) * 8)) & 0xFF);
    if (off == 0x04 || off == 0x05)
      return (uint8_t)(g_blk_cmd >> ((off - 4) * 8));
  } else if (c == g_ide_cfg) {
    if (off >= 0x20 && off < 0x24)
      return (uint8_t)((((g_ide_bar4 & ~15u) | 1u) >> ((off - 0x20) * 8)) & 0xFF);
    if (off == 0x04 || off == 0x05)
      return (uint8_t)(g_ide_cmd >> ((off - 4) * 8));
  }
  return c[off];
}

static void pci_cfg_put(unsigned dev, unsigned fn, unsigned off, uint8_t v) {
  uint8_t *c = pci_cfg_for(dev, fn);
  if (!c)
    return;
  if (c == g_host_cfg) {
    if (off >= 0x40 || off == 0x04 || off == 0x05 || off == 0x0C || off == 0x0D)
      g_host_cfg[off] = v;
  } else if (c == g_isa_cfg) {
    if (off >= 0x40 || off == 0x04 || off == 0x05)
      g_isa_cfg[off] = v;
  } else if (c == g_pm_cfg) {
    if (off >= 0x40 || off == 0x04 || off == 0x05 || off == 0x3C)
      g_pm_cfg[off] = v;
  } else if (c == g_ide_cfg) {
    if (off >= 0x20 && off < 0x24) {
      unsigned sh = (off - 0x20) * 8;
      g_ide_bar4 = (g_ide_bar4 & ~(0xFFu << sh)) | ((uint32_t)v << sh);
    } else if (off == 0x04 || off == 0x05) {
      unsigned sh = (off - 4) * 8;
      g_ide_cmd = (uint16_t)((g_ide_cmd & ~(0xFFu << sh)) | ((unsigned)v << sh));
      g_ide_cmd &= 0x0405;
    } else if (off >= 0x40 || off == 0x3C || off == 0x0C || off == 0x0D) {
      g_ide_cfg[off] = v;
    }
  } else { // virtio-blk
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
    uint8_t b = (bus || reg + (unsigned)i > 0xFF) ? 0xFF : pci_cfg_get(dev, fn, reg + (unsigned)i);
    v |= (uint32_t)b << (8 * i);
  }
  return v;
}

static void pci_data_out(unsigned port, uint32_t v, int size) {
  if (!(g_pci_addr & 0x80000000u))
    return;
  unsigned bus = (g_pci_addr >> 16) & 0xFF, dev = (g_pci_addr >> 11) & 31,
           fn = (g_pci_addr >> 8) & 7, reg = (g_pci_addr & 0xFC) + (port - 0xCFC);
  if (bus)
    return;
  for (int i = 0; i < size && reg + (unsigned)i <= 0xFF; i++)
    pci_cfg_put(dev, fn, reg + (unsigned)i, (uint8_t)(v >> (8 * i)));
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
  if (cyl < 1)
    cyl = 1; // discos diminutos (pruebas): SeaBIOS rechaza 0 cilindros
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
// IDE ATA (PIIX3, modo heredado): canal 0 con un maestro; canal 1 vacio.
// PIO (incluye MULTIPLE y LBA48) y DMA de bus maestro con tabla PRD.
// ---------------------------------------------------------------------------
#define ATA_BSY 0x80
#define ATA_DRDY 0x40
#define ATA_DSC 0x10
#define ATA_DRQ 0x08
#define ATA_ERR 0x01
#define IDE_BUF_SECT 16

static int g_ide_fd = -1;
static uint64_t g_ide_sectors;
static int g_ide_ro;

struct ide_chan {
  int present;
  uint8_t err, feat, sc, sn, cl, ch, dh, status, ctl;
  uint8_t hsc, hsn, hcl, hch, hfeat; // valores anteriores (LBA48)
  uint8_t heads, spt;
  uint8_t multi;
  uint8_t buf[IDE_BUF_SECT * 512];
  unsigned pos, len;        // bytes del bloque PIO actual
  uint64_t lba;             // siguiente sector
  uint32_t left;            // sectores que faltan tras el bloque actual
  unsigned blk;             // sectores por bloque DRQ
  int write;                // transferencia PIO/DMA de escritura
  int dma_pending;
  uint32_t dma_count;
  uint8_t bm_cmd, bm_status;
  uint32_t bm_prd;
  int irq;                  // 14 / 15
};
static struct ide_chan g_ide[2];

static void ide_reset(struct ide_chan *c) {
  c->err = 1;
  c->sc = c->sn = 1;
  c->cl = c->ch = 0;
  c->dh = 0;
  c->status = c->present ? (ATA_DRDY | ATA_DSC) : 0;
  c->pos = c->len = 0;
  c->left = 0;
  c->dma_pending = 0;
}

static void ide_init(void) {
  memset(g_ide, 0, sizeof(g_ide));
  for (int i = 0; i < 2; i++) {
    g_ide[i].irq = 14 + i;
    g_ide[i].heads = 16;
    g_ide[i].spt = 63;
    g_ide[i].multi = 16;
  }
  g_ide[0].present = g_ide_fd >= 0;
  g_ide[0].bm_status = g_ide[0].present ? 0x20 : 0;
  for (int i = 0; i < 2; i++)
    ide_reset(&g_ide[i]);
}

static void ide_irq(struct ide_chan *c) {
  if (!(c->ctl & 2))
    pic_raise(c->irq);
}

static void ide_put_str(uint16_t *w, int first, int words, const char *s) {
  size_t n = strlen(s);
  for (int i = 0; i < words; i++) {
    char a = (size_t)(2 * i) < n ? s[2 * i] : ' ';
    char b = (size_t)(2 * i + 1) < n ? s[2 * i + 1] : ' ';
    w[first + i] = (uint16_t)(((uint8_t)a << 8) | (uint8_t)b);
  }
}

static void ide_identify(struct ide_chan *c) {
  uint16_t w[256];
  memset(w, 0, sizeof(w));
  uint64_t sec = g_ide_sectors;
  uint32_t cyl = (uint32_t)(sec / (16 * 63));
  if (cyl > 16383)
    cyl = 16383;
  if (cyl < 1)
    cyl = 1; // discos diminutos (pruebas): SeaBIOS rechaza 0 cilindros
  w[0] = 0x0040;
  w[1] = (uint16_t)cyl;
  w[3] = 16;
  w[6] = 63;
  ide_put_str(w, 10, 10, "AURORA0000001");
  ide_put_str(w, 23, 4, "1.0");
  ide_put_str(w, 27, 20, "AURORA VM DISK");
  w[47] = 0x8000 | IDE_BUF_SECT;
  w[49] = 0x0300;            // LBA + DMA
  w[53] = 0x0007;
  w[54] = (uint16_t)cyl;
  w[55] = 16;
  w[56] = 63;
  uint32_t cur = (uint32_t)(cyl * 16 * 63);
  w[57] = (uint16_t)cur;
  w[58] = (uint16_t)(cur >> 16);
  w[59] = 0x0100 | c->multi;
  uint32_t l28 = sec > 0x0FFFFFFF ? 0x0FFFFFFF : (uint32_t)sec;
  w[60] = (uint16_t)l28;
  w[61] = (uint16_t)(l28 >> 16);
  w[63] = 0x0007;            // DMA multiword 0-2
  w[64] = 0x0003;            // PIO 3-4
  w[65] = w[66] = w[67] = w[68] = 120;
  w[80] = 0x00F0;            // ATA-4..7
  w[83] = 0x7400;            // FLUSH CACHE (+EXT), LBA48
  w[84] = 0x4000;
  w[85] = 0x0000;
  w[86] = 0x7400;
  w[87] = 0x4000;
  w[88] = 0x003F;            // UDMA 0-5
  w[100] = (uint16_t)sec;
  w[101] = (uint16_t)(sec >> 16);
  w[102] = (uint16_t)(sec >> 32);
  w[103] = (uint16_t)(sec >> 48);
  memcpy(c->buf, w, 512);
}

static uint64_t ide_lba(struct ide_chan *c, int ext) {
  if (ext)
    return ((uint64_t)c->hch << 40) | ((uint64_t)c->hcl << 32) | ((uint64_t)c->hsn << 24) |
           ((uint64_t)c->ch << 16) | ((uint64_t)c->cl << 8) | c->sn;
  if (c->dh & 0x40)
    return ((uint64_t)(c->dh & 0x0F) << 24) | ((uint64_t)c->ch << 16) | ((uint64_t)c->cl << 8) | c->sn;
  uint64_t cyl = ((uint64_t)c->ch << 8) | c->cl;
  return (cyl * c->heads + (c->dh & 0x0F)) * c->spt + (c->sn ? c->sn - 1u : 0);
}

static void ide_abort(struct ide_chan *c) {
  c->status = ATA_DRDY | ATA_DSC | ATA_ERR;
  c->err = 0x04;
  c->left = 0;
  ide_irq(c);
}

// Carga el siguiente bloque de lectura PIO en el buffer. Devuelve 0 si va bien.
static int ide_load_block(struct ide_chan *c) {
  unsigned n = c->left < c->blk ? c->left : c->blk;
  if (c->lba + n > g_ide_sectors ||
      pread(g_ide_fd, c->buf, (size_t)n * 512, (off_t)(c->lba * 512)) != (ssize_t)n * 512) {
    ide_abort(c);
    return -1;
  }
  c->lba += n;
  c->left -= n;
  c->pos = 0;
  c->len = n * 512;
  c->status = ATA_DRDY | ATA_DSC | ATA_DRQ;
  ide_irq(c);
  return 0;
}

static void ide_dma_run(struct ide_chan *c) {
  uint32_t total = c->dma_count * 512;
  uint64_t off = c->lba * 512;
  int fail = 0;
  if (c->lba + c->dma_count > g_ide_sectors || (c->write && g_ide_ro))
    fail = 1;
  uint32_t prd = c->bm_prd & ~3u;
  while (!fail && total) {
    if (!guest_ok(prd, 8)) { fail = 1; break; }
    uint32_t base; uint16_t cnt, fl;
    memcpy(&base, g_ram + prd, 4); memcpy(&cnt, g_ram + prd + 4, 2); memcpy(&fl, g_ram + prd + 6, 2);
    uint32_t n = cnt ? cnt : 65536;
    if (n > total) n = total;
    if (!guest_ok(base, n)) { fail = 1; break; }
    ssize_t r = c->write ? pwrite(g_ide_fd, g_ram + base, n, (off_t)off)
                         : pread(g_ide_fd, g_ram + base, n, (off_t)off);
    if (r != (ssize_t)n) { fail = 1; break; }
    off += n;
    total -= n;
    prd += 8;
    if (fl & 0x8000) break;
  }
  if (total)
    fail = 1;
  c->dma_pending = 0;
  c->bm_status &= (uint8_t)~1;
  if (fail) {
    c->bm_status |= 2;
    c->status = ATA_DRDY | ATA_DSC | ATA_ERR;
    c->err = 0x04;
  } else {
    c->status = ATA_DRDY | ATA_DSC;
  }
  c->bm_status |= 4;
  ide_irq(c);
}

static void ide_command(struct ide_chan *c, uint8_t cmd) {
  if (!c->present || (c->dh & 0x10))
    return;
  c->err = 0;
  c->status = ATA_DRDY | ATA_DSC;
  uint32_t cnt8 = c->sc ? c->sc : 256;
  uint32_t cnt48 = ((uint32_t)c->hsc << 8) | c->sc;
  if (!cnt48) cnt48 = 65536;
  switch (cmd) {
  case 0xEC: // IDENTIFY DEVICE
    ide_identify(c);
    c->pos = 0; c->len = 512; c->left = 0;
    c->status = ATA_DRDY | ATA_DSC | ATA_DRQ;
    ide_irq(c);
    break;
  case 0x20: case 0x21: case 0x24: case 0xC4: case 0x29: {
    int ext = cmd == 0x24 || cmd == 0x29;
    c->write = 0;
    c->lba = ide_lba(c, ext);
    c->left = ext ? cnt48 : cnt8;
    c->blk = (cmd == 0xC4 || cmd == 0x29) ? c->multi : 1;
    ide_load_block(c);
    break;
  }
  case 0x30: case 0x31: case 0x34: case 0xC5: case 0x39: {
    int ext = cmd == 0x34 || cmd == 0x39;
    if (g_ide_ro) { ide_abort(c); break; }
    c->write = 1;
    c->lba = ide_lba(c, ext);
    c->left = ext ? cnt48 : cnt8;
    c->blk = (cmd == 0xC5 || cmd == 0x39) ? c->multi : 1;
    unsigned n = c->left < c->blk ? c->left : c->blk;
    if (c->lba + c->left > g_ide_sectors) { ide_abort(c); break; }
    c->pos = 0;
    c->len = n * 512;
    c->status = ATA_DRDY | ATA_DSC | ATA_DRQ;
    break;
  }
  case 0xC8: case 0x25: case 0xCA: case 0x35: {
    int ext = cmd == 0x25 || cmd == 0x35;
    c->write = cmd == 0xCA || cmd == 0x35;
    c->lba = ide_lba(c, ext);
    c->dma_count = ext ? cnt48 : cnt8;
    c->dma_pending = 1;
    c->status = ATA_DRDY | ATA_DSC | ATA_BSY; // hasta que el maestro de bus arranque
    c->bm_status |= 1;
    if (c->bm_cmd & 1)
      ide_dma_run(c);
    break;
  }
  case 0xC6: // SET MULTIPLE MODE
    if (c->sc && c->sc <= IDE_BUF_SECT && !(c->sc & (c->sc - 1)))
      c->multi = c->sc;
    else { ide_abort(c); break; }
    ide_irq(c);
    break;
  case 0x91: // INITIALIZE DEVICE PARAMETERS
    c->heads = (uint8_t)((c->dh & 0x0F) + 1);
    c->spt = c->sc ? c->sc : 63;
    ide_irq(c);
    break;
  case 0xE7: case 0xEA: // FLUSH CACHE (+EXT)
    if (!g_ide_ro)
      fsync(g_ide_fd);
    ide_irq(c);
    break;
  case 0xEF: case 0x40: case 0x41: case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE6:
  case 0xE8: case 0x10: case 0x70:
    ide_irq(c);
    break;
  case 0xE5: // CHECK POWER MODE: activo
    c->sc = 0xFF;
    ide_irq(c);
    break;
  case 0xF8: { // READ NATIVE MAX ADDRESS
    uint64_t m = g_ide_sectors - 1;
    c->sn = (uint8_t)m; c->cl = (uint8_t)(m >> 8); c->ch = (uint8_t)(m >> 16);
    c->dh = (uint8_t)((c->dh & 0xF0) | ((m >> 24) & 0x0F));
    ide_irq(c);
    break;
  }
  case 0x27: { // READ NATIVE MAX ADDRESS EXT
    uint64_t m = g_ide_sectors - 1;
    c->sn = (uint8_t)m; c->cl = (uint8_t)(m >> 8); c->ch = (uint8_t)(m >> 16);
    c->hsn = (uint8_t)(m >> 24); c->hcl = (uint8_t)(m >> 32); c->hch = (uint8_t)(m >> 40);
    ide_irq(c);
    break;
  }
  default: // IDENTIFY PACKET, DEVICE RESET, SMART...: no soportado
    ide_abort(c);
    break;
  }
}

static uint32_t ide_data_in(struct ide_chan *c, int size) {
  if (!c->present || !(c->status & ATA_DRQ) || c->write)
    return 0xFFFFFFFFu;
  uint32_t v = 0;
  for (int i = 0; i < size && c->pos < c->len; i++)
    v |= (uint32_t)c->buf[c->pos++] << (8 * i);
  if (c->pos >= c->len) {
    c->status = ATA_DRDY | ATA_DSC;
    if (c->left)
      ide_load_block(c);
  }
  return v;
}

static void ide_data_out(struct ide_chan *c, uint32_t v, int size) {
  if (!c->present || !(c->status & ATA_DRQ) || !c->write)
    return;
  for (int i = 0; i < size && c->pos < c->len; i++)
    c->buf[c->pos++] = (uint8_t)(v >> (8 * i));
  if (c->pos >= c->len) {
    unsigned n = c->len / 512;
    if (pwrite(g_ide_fd, c->buf, c->len, (off_t)(c->lba * 512)) != (ssize_t)c->len) {
      ide_abort(c);
      return;
    }
    c->lba += n;
    c->left -= n;
    c->status = ATA_DRDY | ATA_DSC;
    if (c->left) {
      unsigned m = c->left < c->blk ? c->left : c->blk;
      c->pos = 0;
      c->len = m * 512;
      c->status |= ATA_DRQ;
    }
    ide_irq(c);
  }
}

// Registros 0..7 (a partir de la base) y control (nr == 8)
static uint32_t ide_reg_in(struct ide_chan *c, unsigned nr, int size) {
  if (nr == 0)
    return ide_data_in(c, size);
  int absent = !c->present || ((c->dh & 0x10) && nr != 6);
  if (nr == 8 || nr == 7) {
    uint8_t st = (!c->present || (c->dh & 0x10)) ? 0 : c->status;
    if (nr == 7)
      pic_lower(c->irq);
    return st;
  }
  if (absent)
    return 0;
  int hob = c->ctl & 0x80;
  switch (nr) {
  case 1: return c->err;
  case 2: return hob ? c->hsc : c->sc;
  case 3: return hob ? c->hsn : c->sn;
  case 4: return hob ? c->hcl : c->cl;
  case 5: return hob ? c->hch : c->ch;
  case 6: return c->dh;
  default: return 0;
  }
}

static void ide_reg_out(struct ide_chan *c, unsigned nr, uint32_t v, int size) {
  uint8_t b = (uint8_t)v;
  if (nr == 0) {
    ide_data_out(c, v, size);
    return;
  }
  if (nr == 8) { // control: SRST (bit 2), nIEN (bit 1), HOB (bit 7)
    int was = c->ctl & 4;
    c->ctl = b;
    if (was && !(b & 4))
      ide_reset(c);
    return;
  }
  switch (nr) {
  case 1: c->hfeat = c->feat; c->feat = b; break;
  case 2: c->hsc = c->sc; c->sc = b; break;
  case 3: c->hsn = c->sn; c->sn = b; break;
  case 4: c->hcl = c->cl; c->cl = b; break;
  case 5: c->hch = c->ch; c->ch = b; break;
  case 6: c->dh = b; break;
  case 7:
    pic_lower(c->irq);
    ide_command(c, b);
    break;
  default: break;
  }
}

// Maestro de bus (BAR4, 16 bytes): canal 0 en +0..+7, canal 1 en +8..+15
static int ide_bm_ok(unsigned port) {
  if (!g_ide_on || !(g_ide_cmd & 1))
    return 0;
  unsigned base = g_ide_bar4 & ~15u & ~3u;
  return base && port >= base && port < base + 16;
}
static unsigned ide_bm_base(void) { return g_ide_bar4 & ~15u & ~3u; }

static uint32_t ide_bm_in(unsigned off, int size) {
  struct ide_chan *c = &g_ide[(off >> 3) & 1];
  unsigned r = off & 7;
  uint32_t v = 0;
  for (int i = 0; i < size; i++) {
    unsigned k = r + (unsigned)i;
    uint8_t b = 0;
    if (k == 0) b = c->bm_cmd;
    else if (k == 2) b = c->bm_status;
    else if (k >= 4 && k < 8) b = (uint8_t)(c->bm_prd >> (8 * (k - 4)));
    v |= (uint32_t)b << (8 * i);
  }
  return v;
}

static void ide_bm_out(unsigned off, uint32_t v, int size) {
  struct ide_chan *c = &g_ide[(off >> 3) & 1];
  unsigned r = off & 7;
  for (int i = 0; i < size; i++) {
    unsigned k = r + (unsigned)i;
    uint8_t b = (uint8_t)(v >> (8 * i));
    if (k == 0) {
      uint8_t was = c->bm_cmd;
      c->bm_cmd = b & 9;
      if (!(was & 1) && (b & 1) && c->dma_pending)
        ide_dma_run(c);
      if ((was & 1) && !(b & 1))
        c->bm_status &= (uint8_t)~1;
    } else if (k == 2) {
      c->bm_status = (uint8_t)((c->bm_status & ~(b & 6)) & ~0x60);
      c->bm_status |= (uint8_t)(b & 0x60);
      if (c->present)
        c->bm_status |= 0x20;
    } else if (k >= 4 && k < 8) {
      unsigned sh = 8 * (k - 4);
      c->bm_prd = (c->bm_prd & ~(0xFFu << sh)) | ((uint32_t)b << sh);
      c->bm_prd &= ~3u;
    }
  }
}

// ---------------------------------------------------------------------------
// PIIX4 PM (ACPI): PM1 (estado/habilitacion/control), temporizador de 3,58 MHz,
// GPE0, SMI_CMD (0xB2: 0xF1 activa ACPI, 0xF0 lo desactiva) y apagado S5.
// ---------------------------------------------------------------------------
#define PM_REGS 0x40u
static struct { uint16_t sts, en, cnt, gpe_sts, gpe_en; int sci; } g_pm;

static unsigned pm_base(void) {
  if (!g_acpi_on || !(g_pm_cfg[0x80] & 1))
    return 0;
  uint32_t b;
  memcpy(&b, g_pm_cfg + 0x40, 4);
  return b & 0xFFC0u;
}

static void pm_sci_update(void) {
  int want = (g_pm.cnt & 1) && ((g_pm.sts & g_pm.en) || (g_pm.gpe_sts & g_pm.gpe_en));
  if (want != g_pm.sci) {
    g_pm.sci = want;
    if (want)
      pic_raise(9);
    else
      pic_lower(9);
  }
}

static uint32_t pm_tmr(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  __uint128_t ns = (__uint128_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
  return (uint32_t)((ns * 3579545u / 1000000000ULL) & 0xFFFFFF);
}

static uint32_t pm_in(unsigned off, int size) {
  uint8_t b[PM_REGS];
  memset(b, 0, sizeof(b));
  put16(b + 0, g_pm.sts);
  put16(b + 2, g_pm.en);
  put16(b + 4, g_pm.cnt);
  put32(b + 8, pm_tmr());
  uint32_t v = 0;
  if (off + (unsigned)size <= sizeof(b))
    memcpy(&v, b + off, (size_t)size);
  return v;
}

static void pm_out(unsigned off, uint32_t v, int size) {
  if (off == 0 && size >= 2) {
    g_pm.sts &= (uint16_t)~v; // escribir 1 limpia
  } else if (off == 2 && size >= 2) {
    g_pm.en = (uint16_t)v;
  } else if (off == 4 && size >= 2) {
    g_pm.cnt = (uint16_t)(v & ~0x2000u);
    if (v & 0x2000) { // SLP_EN
      unsigned typ = (v >> 10) & 7;
      if (typ == 0 || typ == 5) // _S5 (QEMU usa SLP_TYP=0)
        g_poweroff = 1;
    }
  }
  pm_sci_update();
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
  if (g_acpi_on) {
    unsigned pb = pm_base();
    if (pb && port >= pb && port < pb + PM_REGS) { *v = pm_in(port - pb, size); return 1; }
    if (port >= 0xAFE0 && port <= 0xAFE3) {
      uint32_t w = (uint32_t)g_pm.gpe_sts | ((uint32_t)g_pm.gpe_en << 16);
      *v = (w >> (8 * (port - 0xAFE0))) & (size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu);
      return 1;
    }
    if ((port >= 0xAE00 && port <= 0xAE17) || port == 0xB2 || port == 0xB3) { *v = 0; return 1; }
  }
  if (g_ide_on || g_acpi_on) {
    if (port == 0x4D0 || port == 0x4D1) { *v = 0; return 1; }
  }
  if (g_ide_on) {
    if (port >= 0x1F0 && port <= 0x1F7) { *v = ide_reg_in(&g_ide[0], port - 0x1F0, size); return 1; }
    if (port == 0x3F6) { *v = ide_reg_in(&g_ide[0], 8, size); return 1; }
    if (port >= 0x170 && port <= 0x177) { *v = ide_reg_in(&g_ide[1], port - 0x170, size); return 1; }
    if (port == 0x376) { *v = ide_reg_in(&g_ide[1], 8, size); return 1; }
    if (ide_bm_ok(port)) { *v = ide_bm_in(port - ide_bm_base(), size); return 1; }
  }
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
  if (g_acpi_on) {
    unsigned pb = pm_base();
    if (pb && port >= pb && port < pb + PM_REGS) { pm_out(port - pb, v, size); return 1; }
    if (port == 0xAFE0 || port == 0xAFE1 || port == 0xAFE2) {
      if (size >= 2 && port == 0xAFE0) g_pm.gpe_sts &= (uint16_t)~v;
      if (port == 0xAFE2) g_pm.gpe_en = (uint16_t)v;
      pm_sci_update();
      return 1;
    }
    if (port == 0xB2) {
      if ((v & 0xFF) == 0xF1) g_pm.cnt |= 1;
      else if ((v & 0xFF) == 0xF0) g_pm.cnt &= (uint16_t)~1;
      pm_sci_update();
      return 1;
    }
  }
  if (g_ide_on || g_acpi_on) {
    if (port == 0x4D0 || port == 0x4D1) return 1;
  }
  if (g_ide_on) {
    if (port >= 0x1F0 && port <= 0x1F7) { ide_reg_out(&g_ide[0], port - 0x1F0, v, size); return 1; }
    if (port == 0x3F6) { ide_reg_out(&g_ide[0], 8, v, size); return 1; }
    if (port >= 0x170 && port <= 0x177) { ide_reg_out(&g_ide[1], port - 0x170, v, size); return 1; }
    if (port == 0x376) { ide_reg_out(&g_ide[1], 8, v, size); return 1; }
    if (ide_bm_ok(port)) { ide_bm_out(port - ide_bm_base(), v, size); return 1; }
  }
  if (port == 0x402 || port == 0xE9) {
    if (g_bios_log)
      fputc((int)(v & 0xFF), stderr);
    else if (g_dbgbuf_n < sizeof(g_dbgbuf))
      g_dbgbuf[g_dbgbuf_n++] = (char)v; // se enseña si el arranque falla
    return 1;
  }
  if (port == 0x80 || port <= 0x0F || (port >= 0x81 && port <= 0x8F) ||
      (port >= 0xC0 && port <= 0xDF) || port == 0xB2 || port == 0xB3 || port == 0x3F9 ||
      (port >= 0x3FA && port <= 0x3FF))
    return 1; // DMA, paginas DMA, SMI, POST, serie extra: se ignoran
  return 0;
}

#endif
