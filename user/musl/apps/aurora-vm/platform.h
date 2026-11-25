// platform.h — plataforma PC de la mini-VM (i440FX + PIIX3/PIIX4 como el "pc" de
// QEMU): PCI (mecanismo 1), virtio-blk heredado, ACPI PM (PIIX4), puerto 0x92,
// RCR 0xCF9, depuracion 0x402 y el reparto de puertos de E/S entre dispositivos.
// Los dispositivos viven en sus propios ficheros: pic.h, pit.h, rtc.h, ps2.h,
// ide.h, apic.h y vga.h.
//
// Se incluye desde main.c (necesita g_ram, g_ram_size, dma_ptr, irq_set, ...).
// Ver Plan_MiniVM.md, seccion F3.
#ifndef AVM_PLATFORM_H
#define AVM_PLATFORM_H

#include <sys/stat.h>

#define BIOS_SIZE (128u * 1024)
#define BIOS_ALIAS_GPA 0xFFFE0000ULL

static int g_vga_on;
static uint8_t vga_cfg_get(unsigned off);
static void vga_cfg_put(unsigned off, uint8_t v);
static int vga_in(uint16_t port, int size, uint32_t *v);
static int vga_out(uint16_t port, uint32_t v, int size);
static uint8_t g_vga_cfg[256];

static int g_plat; // 1 = modo BIOS: activa PCI y la plataforma completa
static int g_bios_log;
static char g_dbgbuf[16384];
static size_t g_dbgbuf_n;
static uint8_t g_port92 = 0x02;

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

// ---------------------------------------------------------------------------
// PCI (mecanismo 1): 00:00.0 i440FX, 00:01.0 PIIX3 ISA, 00:01.1 IDE, 00:01.3 PM,
// 00:02.0 VGA, 00:03.0 virtio-blk heredado
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
static int g_acpi_on; // PM PIIX4 (00:01.3): SeaBIOS genera las tablas ACPI
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
static int g_vb_irq = -1; // linea ISA que tiene levantada (PIRQ)

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
  put16(g_ide_cfg + 0x2C, 0x1AF4);
  put16(g_ide_cfg + 0x2E, 0x1100);
  g_ide_bar4 = 0;
  g_ide_cmd = 0;
  // PIIX4 PM (00:01.3): reloj, eventos ACPI y apagado
  memset(g_pm_cfg, 0, sizeof(g_pm_cfg));
  put16(g_pm_cfg + 0x00, 0x8086);
  put16(g_pm_cfg + 0x02, 0x7113);
  g_pm_cfg[0x08] = 0x03;
  g_pm_cfg[0x0A] = 0x80;
  g_pm_cfg[0x0B] = 0x06;
  g_pm_cfg[0x3D] = 0x01;
  g_pm_cfg[0x3C] = 0x09;
  put16(g_pm_cfg + 0x2C, 0x1AF4);
  put16(g_pm_cfg + 0x2E, 0x1100);
  memset(&g_vb, 0, sizeof(g_vb));
  g_vb_irq = -1;
}

static uint8_t *pci_cfg_for(unsigned dev, unsigned fn) {
  if (dev == 0 && fn == 0) return g_host_cfg;
  if (dev == 1 && fn == 0) return g_isa_cfg; // PIIX3: siempre (PIRQ, ELCR, IDE, PM)
  if (dev == 1 && fn == 3 && g_acpi_on) return g_pm_cfg;
  if (dev == 1 && fn == 1 && g_ide_on) return g_ide_cfg;
  if (dev == 2 && fn == 0 && g_vga_on) return g_vga_cfg;
  if (dev == 3 && fn == 0 && g_disk_fd >= 0) return g_blk_cfg;
  return NULL;
}

static void vb_irq_update(void);

static uint8_t pci_cfg_get(unsigned dev, unsigned fn, unsigned off) {
  uint8_t *c = pci_cfg_for(dev, fn);
  if (!c)
    return 0xFF;
  if (c == g_vga_cfg)
    return vga_cfg_get(off);
  if (c == g_blk_cfg) {
    if (off >= 0x10 && off < 0x14)
      return (uint8_t)((((g_blk_bar0 & ~(VIO_BAR_SIZE - 1)) | 1u) >> ((off - 0x10) * 8)) & 0xFF);
    if (off == 0x04 || off == 0x05)
      return (uint8_t)(g_blk_cmd >> ((off - 4) * 8));
    if (off == 0x06) // estado: bit 3 = INTx activa
      return (uint8_t)((g_vb.isr & 1) ? 0x08 : 0);
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
  if (c == g_vga_cfg) {
    vga_cfg_put(off, v);
    return;
  }
  if (c == g_host_cfg) {
    if (off >= 0x40 || off == 0x04 || off == 0x05 || off == 0x0C || off == 0x0D)
      g_host_cfg[off] = v;
  } else if (c == g_isa_cfg) {
    if (off >= 0x40 || off == 0x04 || off == 0x05)
      g_isa_cfg[off] = v;
    if (off >= 0x60 && off <= 0x63)
      vb_irq_update(); // cambio de encaminamiento de las PIRQ
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
      vb_irq_update(); // bit 10: INTx desactivada
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

// INTA# del dispositivo 3 -> PIRQD ((pin - 1 + slot) & 3) -> IRQ de PIIX3 0x63.
static void vb_irq_update(void) {
  uint8_t r = g_isa_cfg[0x60 + ((0 + 3) & 3)];
  int irq = (r & 0x80) ? -1 : (r & 0x0F);
  int level = (g_vb.isr & 1) && !(g_blk_cmd & 0x400) && g_disk_fd >= 0;
  if (g_vb_irq >= 0 && (irq != g_vb_irq || !level)) {
    irq_set(g_vb_irq, 0);
    g_vb_irq = -1;
  }
  if (level && irq > 0 && g_vb_irq < 0) {
    g_vb_irq = irq;
    irq_set(irq, 1);
  }
}

static uint32_t vb_host_features(void) {
  return VIRTIO_BLK_F_SEG_MAX | VIRTIO_BLK_F_GEOMETRY | VIRTIO_BLK_F_BLK_SIZE |
         VIRTIO_BLK_F_FLUSH | (g_disk_ro ? VIRTIO_BLK_F_RO : 0);
}

static unsigned vb_base(void) { return g_blk_bar0 & ~(VIO_BAR_SIZE - 1) & ~3u; }
static int vb_io_base_ok(unsigned port) {
  if (g_disk_fd < 0 || !(g_blk_cmd & 1))
    return 0;
  unsigned base = vb_base();
  return base && port >= base && port < base + VIO_BAR_SIZE;
}

static void vb_reset(void) {
  g_vb.guest_features = g_vb.pfn = 0;
  g_vb.qsel = g_vb.notify = 0;
  g_vb.status = g_vb.isr = 0;
  g_vb.last_avail = g_vb.used_idx = 0;
  vb_irq_update();
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
  if (off <= 0x13 && off + (unsigned)size > 0x13) {
    g_vb.isr = 0; // la lectura limpia ISR (y baja INTx)
    vb_irq_update();
  }
  return v;
}

// Procesa una peticion; devuelve los bytes escritos en el huesped (incluido el estado).
static uint32_t vb_request(uint16_t head) {
  struct seg { uint64_t addr; uint32_t len; uint16_t flags; } segs[VBLK_QN];
  unsigned ns = 0;
  uint64_t desc = (uint64_t)g_vb.pfn << 12;
  uint16_t i = head;
  for (unsigned guard = 0; guard < VBLK_QN; guard++) {
    const uint8_t *d = i < VBLK_QN ? dma_ptr(desc + 16ULL * i, 16) : NULL;
    if (!d)
      return 0;
    uint64_t a; uint32_t l; uint16_t f, nx;
    memcpy(&a, d, 8); memcpy(&l, d + 8, 4); memcpy(&f, d + 12, 2); memcpy(&nx, d + 14, 2);
    if (f & 4) // INDIRECT: no se ofrece
      return 0;
    segs[ns++] = (struct seg){a, l, f};
    if (!(f & 1))
      break;
    i = nx;
  }
  uint8_t *hdr = ns ? dma_ptr(segs[0].addr, 16) : NULL;
  uint8_t *st = ns ? dma_ptr(segs[ns - 1].addr, 1) : NULL;
  if (ns < 2 || segs[0].len < 16 || (segs[0].flags & 2) || !(segs[ns - 1].flags & 2) ||
      segs[ns - 1].len < 1 || !hdr || !st)
    return 0;
  uint32_t type; uint64_t sector;
  memcpy(&type, hdr, 4);
  memcpy(&sector, hdr + 8, 8);
  uint8_t status = 0; // OK
  uint32_t written = 0;
  uint64_t off = sector * 512;
  for (unsigned k = 1; k + 1 < ns && status == 0; k++) {
    struct seg *s = &segs[k];
    uint8_t *g = dma_ptr(s->addr, s->len);
    if (!g) { status = 1; break; }
    if (type == 0 || type == 8) { // IN / GET_ID: el dispositivo escribe
      if (!(s->flags & 2)) { status = 1; break; }
      if (type == 0) {
        if (off + s->len > g_disk_sectors * 512) { status = 1; break; }
        ssize_t r = vm_pread(g_disk_fd, g, s->len, (off_t)off);
        if (r != (ssize_t)s->len) { status = 1; break; }
        off += s->len;
      } else {
        char id[20] = "aurora-vm-disk";
        memset(g, 0, s->len);
        memcpy(g, id, s->len < sizeof(id) ? s->len : sizeof(id));
      }
      written += s->len;
    } else if (type == 1) { // OUT: el dispositivo lee
      if ((s->flags & 2) || g_disk_ro || off + s->len > g_disk_sectors * 512) { status = 1; break; }
      if (vm_pwrite(g_disk_fd, g, s->len, (off_t)off) != (ssize_t)s->len) { status = 1; break; }
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
  *st = status;
  return written + 1;
}

static void vb_notify(void) {
  if (!g_vb.pfn)
    return;
  uint64_t base = (uint64_t)g_vb.pfn << 12;
  uint64_t avail = base + 16ULL * VBLK_QN;
  uint64_t used = (avail + 4 + 2ULL * VBLK_QN + 2 + 4095) & ~4095ULL;
  uint8_t *ap = dma_ptr(avail, 4 + 2ULL * VBLK_QN), *up = dma_ptr(used, 4 + 8ULL * VBLK_QN);
  if (!ap || !up)
    return;
  int any = 0;
  for (;;) {
    uint16_t aidx;
    memcpy(&aidx, ap + 2, 2);
    if (aidx == g_vb.last_avail)
      break;
    uint16_t head;
    memcpy(&head, ap + 4 + 2ULL * (g_vb.last_avail % VBLK_QN), 2);
    uint32_t len = vb_request(head);
    uint8_t *u = up + 4 + 8ULL * (g_vb.used_idx % VBLK_QN);
    uint32_t id = head;
    memcpy(u, &id, 4);
    memcpy(u + 4, &len, 4);
    g_vb.used_idx++;
    g_vb.last_avail++;
    memcpy(up + 2, &g_vb.used_idx, 2);
    any = 1;
  }
  if (any) {
    uint16_t aflags;
    memcpy(&aflags, ap, 2);
    if (!(aflags & 1)) { // VRING_AVAIL_F_NO_INTERRUPT
      g_vb.isr |= 1;
      vb_irq_update();
    }
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
// PIIX4 PM (ACPI): PM1 (estado/habilitacion/control), temporizador de 3,58 MHz
// (TMR_STS al cambiar el bit 23), GPE0, SMI_CMD (0xB2: 0xF1 activa ACPI, 0xF0 lo
// desactiva) y apagado S5.
// ---------------------------------------------------------------------------
#define PM_REGS 0x40u
static struct {
  uint16_t sts, en, cnt, gpe_sts, gpe_en;
  int sci;
  uint32_t last_bit23;
} g_pm;

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
    irq_set(g_pm_cfg[0x3C] & 0x0F ? g_pm_cfg[0x3C] & 0x0F : 9, want);
  }
}

static uint64_t pm_ticks(uint64_t ns) { return (uint64_t)((__uint128_t)ns * 3579545u / 1000000000ULL); }
static uint32_t pm_tmr(void) { return (uint32_t)(pm_ticks(now_ns()) & 0xFFFFFF); }

// TMR_STS: se pone cada vez que cambia el bit 23 del contador.
static uint64_t pm_poll(uint64_t now) {
  if (!g_acpi_on)
    return ~0ULL;
  uint64_t t = pm_ticks(now);
  uint32_t b = (uint32_t)(t >> 23);
  if (b != g_pm.last_bit23) {
    g_pm.last_bit23 = b;
    g_pm.sts |= 1;
    pm_sci_update();
  }
  uint64_t next = ((uint64_t)b + 1) << 23;
  return now + (uint64_t)((__uint128_t)(next - t) * 1000000000ULL / 3579545u) + 1;
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
  int slp = 0;
  for (int i = 0; i < size; i++) {
    unsigned k = off + (unsigned)i;
    uint16_t x = (uint16_t)((v >> (8 * i)) & 0xFF), sh = (uint16_t)((k & 1) * 8);
    switch (k) {
    case 0: case 1: g_pm.sts &= (uint16_t)~(x << sh); break; // escribir 1 limpia
    case 2: case 3: g_pm.en = (uint16_t)((g_pm.en & ~(0xFF << sh)) | (x << sh)); break;
    case 4: case 5: {
      uint16_t c = (uint16_t)((g_pm.cnt & ~(0xFF << sh)) | (x << sh));
      if (c & 0x2000)
        slp = 1;
      g_pm.cnt = c & (uint16_t)~0x2000u;
      break;
    }
    default: break;
    }
  }
  if (slp) { // SLP_EN
    unsigned typ = (g_pm.cnt >> 10) & 7;
    if (typ == 0 || typ == 5) // _S5 (QEMU usa SLP_TYP=0)
      g_poweroff = 1;
  }
  pm_sci_update();
}

// ---------------------------------------------------------------------------
// Puertos de la plataforma. Devuelve 1 si el puerto es de la plataforma.
// ---------------------------------------------------------------------------
static int ide_bm_ok(unsigned port) {
  if (!g_ide_on || !(g_ide_cmd & 1))
    return 0;
  unsigned base = g_ide_bar4 & ~15u;
  return base && port >= base && port < base + 16;
}

static int plat_in(uint16_t port, int size, uint32_t *v) {
  if (!g_plat)
    return 0;
  if (g_vga_on && vga_in(port, size, v)) return 1;
  if (pic_port(port)) { *v = pic_in(port); return 1; }
  if (port >= 0x40 && port <= 0x43) { *v = pit_read(port); return 1; }
  if (port == 0x61) { *v = port61_read(); return 1; }
  if (rtc_port(port)) { *v = rtc_in(port); return 1; }
  if (i8042_port(port)) { *v = i8042_in(port); return 1; }
  if (port == 0x92) { *v = g_port92; return 1; }
  if (port == 0xCF8 && size == 4) { *v = g_pci_addr; return 1; }
  if (port >= 0xCFC && port <= 0xCFF) { *v = pci_data_in(port, size); return 1; }
  if (vb_io_base_ok(port)) { *v = vb_in(port - vb_base(), size); return 1; }
  if (g_acpi_on) {
    unsigned pb = pm_base();
    if (pb && port >= pb && port < pb + PM_REGS) { *v = pm_in(port - pb, size); return 1; }
    if (port >= 0xAFE0 && port <= 0xAFE3) {
      uint32_t w = (uint32_t)g_pm.gpe_sts | ((uint32_t)g_pm.gpe_en << 16);
      *v = w >> (8 * (port - 0xAFE0));
      return 1;
    }
    if ((port >= 0xAE00 && port <= 0xAE17) || port == 0xB2 || port == 0xB3) { *v = 0; return 1; }
  }
  if (ide_port(port)) { *v = ide_in(port, size); return 1; }
  if (ide_bm_ok(port)) { *v = ide_bm_in(port - (g_ide_bar4 & ~15u), size); return 1; }
  if (port == 0x402 || port == 0xE9) { *v = 0xE9; return 1; }
  return 0;
}

static int plat_out(uint16_t port, uint32_t v, int size) {
  if (!g_plat)
    return 0;
  if (g_vga_on && vga_out(port, v, size)) return 1;
  if (pic_port(port)) { pic_out(port, (uint8_t)v); return 1; }
  if (port >= 0x40 && port <= 0x43) { pit_write(port, (uint8_t)v); return 1; }
  if (port == 0x61) { port61_write((uint8_t)v); return 1; }
  if (rtc_port(port)) { rtc_out(port, (uint8_t)v); return 1; }
  if (i8042_port(port)) { i8042_out(port, (uint8_t)v); return 1; }
  if (port == 0x92) {
    uint8_t old = g_port92;
    g_port92 = (uint8_t)(v & ~1u) | 0x02; // A20 siempre activa (no se emula el corte)
    if ((v & 1) && !(old & 1))
      g_reset_req = 1;
    return 1;
  }
  if (port == 0xCF8 && size == 4) { g_pci_addr = v; return 1; }
  if (port == 0xCF9 && size == 1) { if (v & 4) g_reset_req = 1; return 1; }
  if (port >= 0xCFC && port <= 0xCFF) { pci_data_out(port, v, size); return 1; }
  if (vb_io_base_ok(port)) { vb_out(port - vb_base(), v, size); return 1; }
  if (g_acpi_on) {
    unsigned pb = pm_base();
    if (pb && port >= pb && port < pb + PM_REGS) { pm_out(port - pb, v, size); return 1; }
    if (port >= 0xAFE0 && port <= 0xAFE3) {
      for (int i = 0; i < size; i++) {
        unsigned k = port - 0xAFE0 + (unsigned)i;
        uint16_t x = (uint16_t)((v >> (8 * i)) & 0xFF), sh = (uint16_t)((k & 1) * 8);
        if (k < 2)
          g_pm.gpe_sts &= (uint16_t)~(x << sh);
        else if (k < 4)
          g_pm.gpe_en = (uint16_t)((g_pm.gpe_en & ~(0xFF << sh)) | (x << sh));
      }
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
  if (ide_port(port)) { ide_out(port, v, size); return 1; }
  if (ide_bm_ok(port)) { ide_bm_out(port - (g_ide_bar4 & ~15u), v, size); return 1; }
  if (port == 0x402 || port == 0xE9) {
    if (g_bios_log)
      fputc((int)(v & 0xFF), stderr);
    else if (g_dbgbuf_n < sizeof(g_dbgbuf))
      g_dbgbuf[g_dbgbuf_n++] = (char)v; // se enseña si el arranque falla
    return 1;
  }
  return 0; // el resto (DMA ISA, POST 0x80...) se ignora en io_out
}

#endif
