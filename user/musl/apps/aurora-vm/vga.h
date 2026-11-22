// vga.h — tarjeta VGA estandar (Bochs/QEMU "stdvga", PCI 1234:1111) de la mini-VM.
//
// Reproduce el comportamiento de la VGA de QEMU (hw/display/vga.c), que es lo que
// esperan SeaVGABIOS, el bootvid de Windows/ReactOS y sus drivers VGA/VBE:
//   - VRAM de 16 MiB = el memfd del BAR0 (LFB). En modo planar el byte del plano p
//     de la direccion a esta en vram[a*4 + p] (como QEMU).
//   - 0xA0000-0xBFFFF nunca es RAM: cada acceso sale al VMM (insn.h) y pasa por
//     vga_mem_readb/writeb: chain4, par/impar, modos de escritura 0-3, operaciones
//     logicas, set/reset, mascara de bits, latches, modos de lectura 0/1, banco VBE.
//   - Puertos 0x3B4/0x3B5/0x3BA, 0x3C0-0x3CF, 0x3D4/0x3D5/0x3DA (por bytes) y los
//     registros Bochs VBE en 0x1CE/0x1CF.
//   - Dibujo: texto (fuente del plano 2, atributos, cursor), graficos de 2/4/8 bits
//     planares o encadenados (CGA, doble escaneo, line compare) y VBE de 4/8/15/16/24/32.
#ifndef AVM_VGA_H
#define AVM_VGA_H

#define VGA_LFB_SIZE (16u << 20) // = VRAM
#define VGA_ROM_SIZE 0x10000u
#define VBE_MAX_XRES 16000
#define VBE_MAX_YRES 12000

static int g_vga_on;
static uint8_t g_vga_cfg[256];
static uint32_t g_vga_bar0, g_vga_rom;
static uint16_t g_vga_cmd;
static uint8_t *g_lfb; // VRAM
static uint8_t *g_vrom;
static int g_vga_lfb_fd = -1, g_vga_rom_fd = -1;
static uint64_t g_vga_lfb_gpa, g_vga_rom_gpa;

static struct {
  uint8_t sr[8], sr_vbe[8], gr[16], ar[0x15], cr[256];
  uint8_t sr_index, gr_index, ar_index, cr_index, ar_flip_flop;
  uint8_t msr, fcr, st00, st01;
  uint8_t dac_state, dac_sub_index, dac_read_index, dac_write_index, dac_cache[3], dac_8bit;
  uint8_t palette[768];
  uint32_t latch;
  uint32_t bank_offset;
  uint16_t vbe_index, vbe_regs[10];
  uint32_t vbe_start_addr, vbe_line_offset;
} V;

static const uint8_t vga_sr_mask[8] = {0x03, 0x3d, 0x0f, 0x3f, 0x0e, 0x00, 0x00, 0xff};
static const uint8_t vga_gr_mask[16] = {0x0f, 0x0f, 0x0f, 0x1f, 0x03, 0x7b, 0x0f, 0x0f,
                                        0xff, 0, 0, 0, 0, 0, 0, 0};
static const uint32_t vga_mask16[16] = {
    0x00000000, 0x000000ff, 0x0000ff00, 0x0000ffff, 0x00ff0000, 0x00ff00ff, 0x00ffff00, 0x00ffffff,
    0xff000000, 0xff0000ff, 0xff00ff00, 0xff00ffff, 0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff};
static uint32_t vga_expand4[256];
static uint16_t vga_expand2[256];
static uint8_t vga_expand4to8[16];

static void vga_remap(void);

static int vbe_enabled(void) { return V.vbe_regs[4] & 1; }
static uint8_t vga_sr(int i) { return vbe_enabled() ? V.sr_vbe[i] : V.sr[i]; }

static void vga_reset(void) {
  memset(&V, 0, sizeof(V));
  V.vbe_regs[0] = 0xB0C5;
  for (int i = 0; i < 256; i++) {
    uint32_t v = 0;
    for (int j = 0; j < 8; j++)
      v |= (uint32_t)((i >> j) & 1) << (j * 4);
    vga_expand4[i] = v;
    v = 0;
    for (int j = 0; j < 4; j++)
      v |= (uint32_t)((i >> (2 * j)) & 3) << (j * 4);
    vga_expand2[i] = (uint16_t)v;
  }
  for (int i = 0; i < 16; i++) {
    unsigned v = 0;
    for (int j = 0; j < 4; j++) {
      unsigned b = (i >> j) & 1;
      v |= b << (2 * j);
      v |= b << (2 * j + 1);
    }
    vga_expand4to8[i] = (uint8_t)v;
  }
  g_vga_lfb_gpa = g_vga_rom_gpa = 0;
  g_vga_bar0 = g_vga_rom = 0;
  g_vga_cmd = 0;
  memset(g_vga_cfg, 0, sizeof(g_vga_cfg));
  put16(g_vga_cfg + 0x00, 0x1234);
  put16(g_vga_cfg + 0x02, 0x1111);
  g_vga_cfg[0x08] = 0x02;
  g_vga_cfg[0x0A] = 0x00; // subclase VGA
  g_vga_cfg[0x0B] = 0x03; // controlador de pantalla
  put16(g_vga_cfg + 0x2C, 0x1AF4);
  put16(g_vga_cfg + 0x2E, 0x1100);
}

// ---------------------------------------------------------------------------
// Configuracion PCI (llamada desde pci_cfg_get/put de platform.h)
// ---------------------------------------------------------------------------
static uint8_t vga_cfg_get(unsigned off) {
  if (off >= 0x10 && off < 0x14)
    return (uint8_t)((((g_vga_bar0 & ~(VGA_LFB_SIZE - 1)) | 8u) >> ((off - 0x10) * 8)) & 0xFF);
  if (off >= 0x30 && off < 0x34)
    return (uint8_t)((((g_vga_rom & ~(VGA_ROM_SIZE - 1)) | (g_vga_rom & 1u)) >> ((off - 0x30) * 8)) & 0xFF);
  if (off == 0x04 || off == 0x05)
    return (uint8_t)(g_vga_cmd >> ((off - 4) * 8));
  return g_vga_cfg[off];
}
static void vga_cfg_put(unsigned off, uint8_t v) {
  if (off >= 0x10 && off < 0x14) {
    unsigned sh = (off - 0x10) * 8;
    g_vga_bar0 = (g_vga_bar0 & ~(0xFFu << sh)) | ((uint32_t)v << sh);
  } else if (off >= 0x30 && off < 0x34) {
    unsigned sh = (off - 0x30) * 8;
    g_vga_rom = (g_vga_rom & ~(0xFFu << sh)) | ((uint32_t)v << sh);
  } else if (off == 0x04 || off == 0x05) {
    unsigned sh = (off - 4) * 8;
    g_vga_cmd = (uint16_t)((g_vga_cmd & ~(0xFFu << sh)) | ((unsigned)v << sh));
    g_vga_cmd &= 0x0407;
  } else if (off == 0x3C || off == 0x0C || off == 0x0D) {
    g_vga_cfg[off] = v;
  }
  vga_remap();
}

// Mapea LFB y ROM en el espacio del huesped segun lo que haya programado la BIOS.
static void vga_remap(void) {
  if (!g_vga_on || g_vm < 0)
    return;
  uint64_t b = g_vga_bar0 & ~(VGA_LFB_SIZE - 1);
  if ((g_vga_cmd & 2) && b >= g_ram_size && b >= 0x100000 && b + VGA_LFB_SIZE <= 0xFEC00000ULL &&
      b != g_vga_lfb_gpa) {
    struct avm_memory m = {.fd = g_vga_lfb_fd, .slot = 2, .gpa = b, .size = VGA_LFB_SIZE, .offset = 0};
    if (ioctl(g_vm, AVM_SET_MEMORY, &m) < 0)
      perror("aurora-vm: AVM_SET_MEMORY (LFB)");
    else
      g_vga_lfb_gpa = b;
  }
  uint64_t r = g_vga_rom & ~(uint64_t)(VGA_ROM_SIZE - 1);
  if ((g_vga_rom & 1) && g_vrom && r >= g_ram_size && r >= 0x100000 &&
      r + VGA_ROM_SIZE <= 0xFEC00000ULL && r != g_vga_rom_gpa) {
    struct avm_memory m = {.fd = g_vga_rom_fd, .slot = 3, .gpa = r, .size = VGA_ROM_SIZE, .offset = 0};
    if (ioctl(g_vm, AVM_SET_MEMORY, &m) < 0)
      perror("aurora-vm: AVM_SET_MEMORY (ROM VGA)");
    else
      g_vga_rom_gpa = r;
  }
}

// ---------------------------------------------------------------------------
// Registros Bochs VBE
// ---------------------------------------------------------------------------
static void vbe_fixup_regs(void) {
  uint16_t *r = V.vbe_regs;
  uint32_t bits, linelength, maxy, offset;
  if (!vbe_enabled())
    return;
  switch (r[3]) {
  case 4: case 8: case 16: case 24: case 32: bits = r[3]; break;
  case 15: bits = 16; break;
  default: bits = r[3] = 8; break;
  }
  r[1] &= (uint16_t)~7u;
  if (!r[1]) r[1] = 8;
  if (r[1] > VBE_MAX_XRES) r[1] = VBE_MAX_XRES;
  r[6] &= (uint16_t)~7u;
  if (r[6] > VBE_MAX_XRES) r[6] = VBE_MAX_XRES;
  if (r[6] < r[1]) r[6] = r[1];
  linelength = (uint32_t)r[6] * bits / 8;
  maxy = VGA_LFB_SIZE / linelength;
  if (!r[2]) r[2] = 1;
  if (r[2] > VBE_MAX_YRES) r[2] = VBE_MAX_YRES;
  if (r[2] > maxy) r[2] = (uint16_t)maxy;
  if (r[8] > VBE_MAX_XRES) r[8] = VBE_MAX_XRES;
  if (r[9] > VBE_MAX_YRES) r[9] = VBE_MAX_YRES;
  offset = (uint32_t)r[8] * bits / 8 + (uint32_t)r[9] * linelength;
  if (offset + (uint32_t)r[2] * linelength > VGA_LFB_SIZE) {
    r[9] = 0;
    offset = (uint32_t)r[8] * bits / 8;
    if (offset + (uint32_t)r[2] * linelength > VGA_LFB_SIZE) {
      r[8] = 0;
      offset = 0;
    }
  }
  r[7] = (uint16_t)(maxy > 0xFFFF ? 0xFFFF : maxy);
  V.vbe_line_offset = linelength;
  V.vbe_start_addr = offset / 4;
}

static void vbe_update_vgaregs(void) {
  if (!vbe_enabled())
    return;
  V.gr[6] = (uint8_t)((V.gr[6] & ~0x0c) | 0x04 | 1);
  V.cr[0x17] |= 3;
  V.cr[0x13] = (uint8_t)(V.vbe_line_offset >> 3);
  V.cr[0x01] = (uint8_t)((V.vbe_regs[1] >> 3) - 1);
  int h = V.vbe_regs[2] - 1;
  V.cr[0x12] = (uint8_t)h;
  V.cr[0x07] = (uint8_t)((V.cr[0x07] & ~0x42) | ((h >> 7) & 0x02) | ((h >> 3) & 0x40));
  V.cr[0x18] = 0xff;
  V.cr[0x07] |= 0x10;
  V.cr[0x09] |= 0x40;
  int shift_control;
  if (V.vbe_regs[3] == 4) {
    shift_control = 0;
    V.sr_vbe[1] &= (uint8_t)~8;
  } else {
    shift_control = 2;
    V.sr_vbe[4] |= 0x08; // chain 4
    V.sr_vbe[2] |= 0x0F; // todos los planos
  }
  V.gr[5] = (uint8_t)((V.gr[5] & ~0x60) | (shift_control << 5));
  V.cr[0x09] &= (uint8_t)~0x9f;
}

static uint16_t vbe_read_data(void) {
  unsigned i = V.vbe_index;
  if (i < 10) {
    if (V.vbe_regs[4] & 2) {
      if (i == 1) return VBE_MAX_XRES;
      if (i == 2) return VBE_MAX_YRES;
      if (i == 3) return 32;
    }
    return V.vbe_regs[i];
  }
  if (i == 10)
    return (uint16_t)(VGA_LFB_SIZE >> 16);
  return 0;
}

static void vbe_write_data(uint16_t val) {
  unsigned i = V.vbe_index;
  if (i > 10)
    return;
  switch (i) {
  case 0:
    if (val >= 0xB0C0 && val <= 0xB0C5)
      V.vbe_regs[0] = val;
    break;
  case 1: case 2: case 3: case 6: case 8: case 9:
    V.vbe_regs[i] = val;
    vbe_fixup_regs();
    vbe_update_vgaregs();
    break;
  case 5:
    val &= (uint16_t)((VGA_LFB_SIZE >> 16) - 1);
    V.vbe_regs[5] = val;
    V.bank_offset = (uint32_t)val << 16;
    break;
  case 4:
    if ((val & 1) && !(V.vbe_regs[4] & 1)) {
      V.vbe_regs[6] = 0;
      V.vbe_regs[8] = 0;
      V.vbe_regs[9] = 0;
      V.vbe_regs[4] |= 1;
      vbe_fixup_regs();
      vbe_update_vgaregs();
      if (!(val & 0x80) && g_lfb) {
        size_t n = (size_t)V.vbe_regs[2] * V.vbe_line_offset;
        memset(g_lfb, 0, n > VGA_LFB_SIZE ? VGA_LFB_SIZE : n);
      }
    } else {
      V.bank_offset = 0;
    }
    V.dac_8bit = (val & 0x20) != 0;
    V.vbe_regs[4] = val;
    break;
  default:
    break;
  }
}

// ---------------------------------------------------------------------------
// Puertos VGA (por bytes, como QEMU)
// ---------------------------------------------------------------------------
static int vga_port_invalid(uint16_t a) {
  if (V.msr & 1)
    return a >= 0x3b0 && a <= 0x3bf; // color: los puertos mono no responden
  return a >= 0x3d0 && a <= 0x3df;
}

static uint8_t vga_ioport_read(uint16_t a) {
  if (vga_port_invalid(a))
    return 0xff;
  switch (a) {
  case 0x3c0: return V.ar_flip_flop == 0 ? V.ar_index : 0;
  case 0x3c1: { unsigned i = V.ar_index & 0x1f; return i < 0x15 ? V.ar[i] : 0; }
  case 0x3c2: return V.st00;
  case 0x3c4: return V.sr_index;
  case 0x3c5: return V.sr[V.sr_index];
  case 0x3c7: return V.dac_state;
  case 0x3c8: return V.dac_write_index;
  case 0x3c9: {
    uint8_t v = V.palette[V.dac_read_index * 3 + V.dac_sub_index];
    if (++V.dac_sub_index == 3) {
      V.dac_sub_index = 0;
      V.dac_read_index++;
    }
    return v;
  }
  case 0x3ca: return V.fcr;
  case 0x3cc: return V.msr;
  case 0x3ce: return V.gr_index;
  case 0x3cf: return V.gr[V.gr_index];
  case 0x3b4: case 0x3d4: return V.cr_index;
  case 0x3b5: case 0x3d5: return V.cr[V.cr_index];
  case 0x3ba: case 0x3da:
    V.st01 ^= 0x09; // retrazo vertical / pantalla: alterna en cada lectura
    V.ar_flip_flop = 0;
    return V.st01;
  default: return 0x00;
  }
}

static void vga_ioport_write(uint16_t a, uint8_t val) {
  if (vga_port_invalid(a))
    return;
  switch (a) {
  case 0x3c0:
    if (V.ar_flip_flop == 0) {
      V.ar_index = val & 0x3f;
    } else {
      unsigned i = V.ar_index & 0x1f;
      if (i <= 0x0f) V.ar[i] = val & 0x3f;
      else if (i == 0x10) V.ar[i] = val & ~0x10;
      else if (i == 0x11) V.ar[i] = val;
      else if (i == 0x12) V.ar[i] = val & ~0xc0;
      else if (i == 0x13) V.ar[i] = val & ~0xf0;
      else if (i == 0x14) V.ar[i] = val & ~0xf0;
    }
    V.ar_flip_flop ^= 1;
    break;
  case 0x3c2: V.msr = val & ~0x10; break;
  case 0x3c4: V.sr_index = val & 7; break;
  case 0x3c5: V.sr[V.sr_index] = val & vga_sr_mask[V.sr_index]; break;
  case 0x3c7: V.dac_read_index = val; V.dac_sub_index = 0; V.dac_state = 3; break;
  case 0x3c8: V.dac_write_index = val; V.dac_sub_index = 0; V.dac_state = 0; break;
  case 0x3c9:
    V.dac_cache[V.dac_sub_index] = val;
    if (++V.dac_sub_index == 3) {
      memcpy(&V.palette[V.dac_write_index * 3], V.dac_cache, 3);
      V.dac_sub_index = 0;
      V.dac_write_index++;
    }
    break;
  case 0x3ce: V.gr_index = val & 0x0f; break;
  case 0x3cf:
    V.gr[V.gr_index] = val & vga_gr_mask[V.gr_index];
    vbe_update_vgaregs();
    break;
  case 0x3b4: case 0x3d4: V.cr_index = val; break;
  case 0x3b5: case 0x3d5:
    if ((V.cr[0x11] & 0x80) && V.cr_index <= 7) { // CR0-7 protegidos
      if (V.cr_index == 7) {
        V.cr[7] = (uint8_t)((V.cr[7] & ~0x10) | (val & 0x10));
        vbe_update_vgaregs();
      }
      return;
    }
    V.cr[V.cr_index] = val;
    vbe_update_vgaregs();
    break;
  case 0x3ba: case 0x3da: V.fcr = val & 0x10; break;
  default: break;
  }
}

static int vga_port_ours(uint16_t p) {
  return (p >= 0x3B4 && p <= 0x3B5) || p == 0x3BA || (p >= 0x3C0 && p <= 0x3CF) ||
         (p >= 0x3D4 && p <= 0x3D5) || p == 0x3DA;
}

// Accesos de 16/32 bits: un byte por puerto consecutivo (como QEMU).
static int vga_out(uint16_t port, uint32_t v, int size) {
  if (!g_vga_on)
    return 0;
  if (port == 0x1CE || port == 0x1CF) {
    if (port == 0x1CE)
      V.vbe_index = (uint16_t)v;
    else
      vbe_write_data((uint16_t)v);
    return 1;
  }
  if (!vga_port_ours(port))
    return 0;
  for (int i = 0; i < size; i++)
    if (vga_port_ours((uint16_t)(port + i)))
      vga_ioport_write((uint16_t)(port + i), (uint8_t)(v >> (8 * i)));
  return 1;
}

static int vga_in(uint16_t port, int size, uint32_t *v) {
  if (!g_vga_on)
    return 0;
  if (port == 0x1CE || port == 0x1CF) {
    *v = port == 0x1CE ? V.vbe_index : vbe_read_data();
    return 1;
  }
  if (!vga_port_ours(port))
    return 0;
  uint32_t r = 0;
  for (int i = 0; i < size; i++) {
    uint8_t b = vga_port_ours((uint16_t)(port + i)) ? vga_ioport_read((uint16_t)(port + i)) : 0xff;
    r |= (uint32_t)b << (8 * i);
  }
  *v = r;
  return 1;
}

// ---------------------------------------------------------------------------
// Memoria 0xA0000-0xBFFFF
// ---------------------------------------------------------------------------
static uint8_t vga_mem_readb(uint32_t addr) {
  int map = (V.gr[6] >> 2) & 3;
  addr &= 0x1ffff;
  switch (map) {
  case 0: break;
  case 1:
    if (addr >= 0x10000) return 0xff;
    addr += V.bank_offset;
    break;
  case 2:
    addr -= 0x10000;
    if (addr >= 0x8000) return 0xff;
    break;
  default:
    addr -= 0x18000;
    if (addr >= 0x8000) return 0xff;
    break;
  }
  if (vga_sr(4) & 0x08) // chain 4
    return addr < VGA_LFB_SIZE ? g_lfb[addr] : 0xff;
  if (V.gr[5] & 0x10) { // par/impar (texto)
    unsigned plane = (V.gr[4] & 2) | (addr & 1);
    addr = ((addr & ~1u) << 1) | plane;
    return addr < VGA_LFB_SIZE ? g_lfb[addr] : 0xff;
  }
  if ((uint64_t)addr * 4 >= VGA_LFB_SIZE)
    return 0xff;
  memcpy(&V.latch, g_lfb + (size_t)addr * 4, 4);
  if (!(V.gr[5] & 0x08)) // modo de lectura 0
    return (uint8_t)(V.latch >> ((V.gr[4] & 3) * 8));
  uint32_t r = (V.latch ^ vga_mask16[V.gr[2]]) & vga_mask16[V.gr[7]]; // modo 1: comparacion
  r |= r >> 16;
  r |= r >> 8;
  return (uint8_t)~r;
}

static void vga_mem_writeb(uint32_t addr, uint8_t v8) {
  int map = (V.gr[6] >> 2) & 3;
  addr &= 0x1ffff;
  switch (map) {
  case 0: break;
  case 1:
    if (addr >= 0x10000) return;
    addr += V.bank_offset;
    break;
  case 2:
    addr -= 0x10000;
    if (addr >= 0x8000) return;
    break;
  default:
    addr -= 0x18000;
    if (addr >= 0x8000) return;
    break;
  }
  if (vga_sr(4) & 0x08) { // chain 4
    unsigned mask = 1u << (addr & 3);
    if ((vga_sr(2) & mask) && addr < VGA_LFB_SIZE)
      g_lfb[addr] = v8;
    return;
  }
  if (V.gr[5] & 0x10) { // par/impar
    unsigned plane = (V.gr[4] & 2) | (addr & 1);
    if (vga_sr(2) & (1u << plane)) {
      addr = ((addr & ~1u) << 1) | plane;
      if (addr < VGA_LFB_SIZE)
        g_lfb[addr] = v8;
    }
    return;
  }
  uint32_t val = v8, bit_mask = 0;
  int b;
  switch (V.gr[5] & 3) {
  default:
  case 0:
    b = V.gr[3] & 7;
    val = ((val >> b) | (val << (8 - b))) & 0xff;
    val |= val << 8;
    val |= val << 16;
    {
      uint32_t set_mask = vga_mask16[V.gr[1]];
      val = (val & ~set_mask) | (vga_mask16[V.gr[0]] & set_mask);
    }
    bit_mask = V.gr[8];
    break;
  case 1:
    val = V.latch;
    goto do_write;
  case 2:
    val = vga_mask16[val & 0x0f];
    bit_mask = V.gr[8];
    break;
  case 3:
    b = V.gr[3] & 7;
    val = (val >> b) | (val << (8 - b));
    bit_mask = V.gr[8] & val;
    val = vga_mask16[V.gr[0]];
    break;
  }
  switch (V.gr[3] >> 3) {
  case 1: val &= V.latch; break;
  case 2: val |= V.latch; break;
  case 3: val ^= V.latch; break;
  default: break;
  }
  bit_mask &= 0xff;
  bit_mask |= bit_mask << 8;
  bit_mask |= bit_mask << 16;
  val = (val & bit_mask) | (V.latch & ~bit_mask);
do_write:;
  uint32_t write_mask = vga_mask16[vga_sr(2) & 0x0f];
  if ((uint64_t)addr * 4 >= VGA_LFB_SIZE)
    return;
  uint32_t old;
  memcpy(&old, g_lfb + (size_t)addr * 4, 4);
  old = (old & ~write_mask) | (val & write_mask);
  memcpy(g_lfb + (size_t)addr * 4, &old, 4);
}

// Accesos del huesped a 0xA0000-0xBFFFF (gpa absoluta): byte a byte, en orden.
static void vga_mmio_read(uint64_t gpa, uint8_t *dst, unsigned n) {
  for (unsigned i = 0; i < n; i++)
    dst[i] = g_vga_on ? vga_mem_readb((uint32_t)(gpa + i - 0xA0000)) : 0xff;
}
static void vga_mmio_write(uint64_t gpa, const uint8_t *src, unsigned n) {
  if (!g_vga_on)
    return;
  for (unsigned i = 0; i < n; i++)
    vga_mem_writeb((uint32_t)(gpa + i - 0xA0000), src[i]);
}

// ---------------------------------------------------------------------------
// Dibujo (como vga_update_display de QEMU, siempre completo)
// ---------------------------------------------------------------------------
static uint32_t *g_vga_px;
static size_t g_vga_px_cap;
static int g_vga_last_w, g_vga_last_h;
static uint32_t g_pal[256];
static uint64_t g_cursor_t0;
static int g_vga_cursor_hide; // las comparaciones pixel a pixel no dibujan el cursor

static int c6_to_8(int v) {
  v &= 0x3f;
  int b = v & 1;
  return (v << 2) | (b << 1) | b;
}
static uint32_t rgb32(unsigned r, unsigned g, unsigned b) {
  return 0xFF000000u | ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff);
}

static void vga_palette16(void) {
  for (int i = 0; i < 16; i++) {
    unsigned v = V.ar[i];
    if (V.ar[0x10] & 0x80)
      v = ((V.ar[0x14] & 0xf) << 4) | (v & 0xf);
    else
      v = ((V.ar[0x14] & 0xc) << 4) | (v & 0x3f);
    v *= 3;
    g_pal[i] = rgb32((unsigned)c6_to_8(V.palette[v]), (unsigned)c6_to_8(V.palette[v + 1]),
                     (unsigned)c6_to_8(V.palette[v + 2]));
  }
}
static void vga_palette256(void) {
  for (int i = 0; i < 256; i++) {
    const uint8_t *p = &V.palette[i * 3];
    g_pal[i] = V.dac_8bit ? rgb32(p[0], p[1], p[2])
                          : rgb32((unsigned)c6_to_8(p[0]), (unsigned)c6_to_8(p[1]), (unsigned)c6_to_8(p[2]));
  }
}

static int vga_alloc(int w, int h) {
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
    return -1;
  size_t need = (size_t)w * (size_t)h;
  if (need > g_vga_px_cap) {
    uint32_t *n = realloc(g_vga_px, need * 4);
    if (!n)
      return -1;
    g_vga_px = n;
    g_vga_px_cap = need;
  }
  g_vga_last_w = w;
  g_vga_last_h = h;
  return 0;
}

static void vga_offsets(uint32_t *line_offset, uint32_t *start_addr, uint32_t *line_compare) {
  if (vbe_enabled()) {
    *line_offset = V.vbe_line_offset;
    *start_addr = V.vbe_start_addr;
    *line_compare = 65535;
  } else {
    *line_offset = (uint32_t)V.cr[0x13] << 3;
    *start_addr = V.cr[0x0d] | ((uint32_t)V.cr[0x0c] << 8);
    *line_compare = V.cr[0x18] | ((uint32_t)(V.cr[0x07] & 0x10) << 4) | ((uint32_t)(V.cr[0x09] & 0x40) << 3);
  }
}

static void vga_text_res(int *pw, int *ph, int *pcw, int *pch) {
  int ch = (V.cr[0x09] & 0x1f) + 1;
  int cw = 8;
  if (!(vga_sr(1) & 1)) cw = 9;
  if (vga_sr(1) & 0x08) cw = 16;
  int w = V.cr[0x01] + 1, h;
  if (V.cr[0x06] == 100) {
    h = 100;
  } else {
    h = V.cr[0x12] | ((V.cr[0x07] & 0x02) << 7) | ((V.cr[0x07] & 0x40) << 3);
    h = (h + 1) / ch;
  }
  *pw = w; *ph = h; *pcw = cw; *pch = ch;
}

static uint8_t vram8(uint32_t a) { return g_lfb[a & (VGA_LFB_SIZE - 1)]; }
static uint32_t vram32(uint32_t a) {
  uint32_t v;
  memcpy(&v, g_lfb + (a & (VGA_LFB_SIZE - 1) & ~3u), 4);
  return v;
}

static void glyph_line(uint32_t *d, unsigned bits, int cw, uint32_t fg, uint32_t bg, int dup9) {
  if (cw == 16) {
    unsigned hi = vga_expand4to8[(bits >> 4) & 0xf], lo = vga_expand4to8[bits & 0xf];
    for (int x = 0; x < 8; x++) d[x] = ((hi >> (7 - x)) & 1) ? fg : bg;
    for (int x = 0; x < 8; x++) d[8 + x] = ((lo >> (7 - x)) & 1) ? fg : bg;
    return;
  }
  for (int x = 0; x < 8; x++)
    d[x] = ((bits >> (7 - x)) & 1) ? fg : bg;
  if (cw == 9)
    d[8] = dup9 ? d[7] : bg;
}

static int vga_draw_text(void) {
  int width, height, cw, cheight;
  vga_text_res(&width, &height, &cw, &cheight);
  if (height * width <= 1 || height * width > 160 * 100)
    return -1;
  unsigned v = vga_sr(3);
  uint32_t font_off[2] = {(((v >> 4) & 1) | ((v << 1) & 6)) * 8192u * 4 + 2,
                          (((v >> 5) & 1) | ((v >> 1) & 6)) * 8192u * 4 + 2};
  uint32_t line_offset, start_addr, line_compare;
  vga_offsets(&line_offset, &start_addr, &line_compare);
  if (vga_alloc(width * cw, height * cheight) < 0)
    return -1;
  vga_palette16();
  uint32_t cursor_offset = (((uint32_t)V.cr[0x0e] << 8) | V.cr[0x0f]) - start_addr;
  uint32_t cursor_addr = (start_addr + cursor_offset) * 4;
  int cursor_vis = !g_vga_cursor_hide && (((now_ns() - g_cursor_t0) / 66666666ULL) & 1) == 0;
  int stride = width * cw;
  uint32_t offset = start_addr * 4;
  int line = 0;
  for (int cy = 0; cy < height; cy++) {
    uint32_t src = offset;
    for (int cx = 0; cx < width; cx++) {
      if ((uint64_t)src + 2 > VGA_LFB_SIZE)
        break;
      unsigned ch = g_lfb[src], at = g_lfb[src + 1];
      uint32_t font = font_off[(at >> 3) & 1] + 32 * 4 * ch;
      uint32_t bg = g_pal[at >> 4], fg = g_pal[at & 0x0f];
      int dup9 = cw == 9 && ch >= 0xb0 && ch <= 0xdf && (V.ar[0x10] & 0x04);
      uint32_t *d = g_vga_px + (size_t)cy * cheight * stride + (size_t)cx * cw;
      for (int y = 0; y < cheight; y++)
        glyph_line(d + (size_t)y * stride, vram8(font + (uint32_t)y * 4), cw, fg, bg, dup9);
      if (src == cursor_addr && !(V.cr[0x0a] & 0x20) && cursor_vis) {
        int ls = V.cr[0x0a] & 0x1f, ll = V.cr[0x0b] & 0x1f;
        if (ll > cheight - 1)
          ll = cheight - 1;
        if (ll >= ls && ls < cheight)
          for (int y = ls; y <= ll; y++)
            glyph_line(d + (size_t)y * stride, 0xff, cw, fg, bg, 1);
      }
      src += 4;
    }
    int line1 = line + cheight;
    offset += line_offset;
    if ((uint32_t)line < line_compare && (uint32_t)line1 >= line_compare)
      offset = 0;
    line = line1;
  }
  return 0;
}

static void vga_line(uint32_t *d, uint32_t addr, int width, int kind, int bpp) {
  uint32_t pm = vga_mask16[V.ar[0x12] & 0xf];
  switch (kind) {
  case 0: case 1: { // LINE4 / LINE4D2
    int dbl = kind == 1;
    for (int x = 0; x < (width >> 3); x++) {
      uint32_t data = vram32(addr) & pm;
      uint32_t v = vga_expand4[data & 0xff] | (vga_expand4[(data >> 8) & 0xff] << 1) |
                   (vga_expand4[(data >> 16) & 0xff] << 2) | (vga_expand4[data >> 24] << 3);
      for (int k = 0; k < 8; k++) {
        uint32_t c = g_pal[(v >> (28 - 4 * k)) & 0xf];
        if (dbl) { d[2 * k] = d[2 * k + 1] = c; } else d[k] = c;
      }
      d += dbl ? 16 : 8;
      addr += 4;
    }
    break;
  }
  case 2: case 3: { // LINE2 / LINE2D2 (CGA)
    int dbl = kind == 3;
    for (int x = 0; x < (width >> 3); x++) {
      uint32_t data = vram32(addr) & pm;
      uint32_t v = vga_expand2[data & 0xff] | ((uint32_t)vga_expand2[(data >> 16) & 0xff] << 2);
      uint32_t w = vga_expand2[(data >> 8) & 0xff] | ((uint32_t)vga_expand2[data >> 24] << 2);
      uint32_t px[8] = {g_pal[v >> 12], g_pal[(v >> 8) & 0xf], g_pal[(v >> 4) & 0xf], g_pal[v & 0xf],
                        g_pal[w >> 12], g_pal[(w >> 8) & 0xf], g_pal[(w >> 4) & 0xf], g_pal[w & 0xf]};
      for (int k = 0; k < 8; k++) {
        if (dbl) { d[2 * k] = d[2 * k + 1] = px[k]; } else d[k] = px[k];
      }
      d += dbl ? 16 : 8;
      addr += 4;
    }
    break;
  }
  case 4: // LINE8D2 (modo 13h)
    for (int x = 0; x < (width >> 3); x++) {
      for (int k = 0; k < 4; k++)
        d[2 * k] = d[2 * k + 1] = g_pal[vram8(addr + (uint32_t)k)];
      d += 8;
      addr += 4;
    }
    break;
  case 5: // LINE8
    for (int x = 0; x < (width >> 3); x++) {
      for (int k = 0; k < 8; k++)
        d[k] = g_pal[vram8(addr + (uint32_t)k)];
      d += 8;
      addr += 8;
    }
    break;
  default: // 15/16/24/32 bits
    for (int x = 0; x < width; x++) {
      uint32_t c;
      if (bpp == 15 || bpp == 16) {
        uint32_t off = addr & (VGA_LFB_SIZE - 1) & ~1u;
        unsigned p = g_lfb[off] | (g_lfb[off + 1] << 8);
        if (bpp == 15)
          c = rgb32((p >> 7) & 0xf8, (p >> 2) & 0xf8, (p << 3) & 0xf8);
        else
          c = rgb32((p >> 8) & 0xf8, (p >> 3) & 0xfc, (p << 3) & 0xf8);
        addr += 2;
      } else if (bpp == 24) {
        c = rgb32(vram8(addr + 2), vram8(addr + 1), vram8(addr));
        addr += 3;
      } else {
        c = rgb32(vram8(addr + 2), vram8(addr + 1), vram8(addr));
        addr += 4;
      }
      d[x] = c;
    }
    break;
  }
}

static int vga_draw_graphic(void) {
  uint32_t line_offset, start_addr, line_compare;
  vga_offsets(&line_offset, &start_addr, &line_compare);
  int width, height;
  if (vbe_enabled()) {
    width = V.vbe_regs[1];
    height = V.vbe_regs[2];
  } else {
    width = (V.cr[0x01] + 1) * 8;
    height = (V.cr[0x12] | ((V.cr[0x07] & 0x02) << 7) | ((V.cr[0x07] & 0x40) << 3)) + 1;
  }
  int disp_width = width;
  int shift_control = (V.gr[5] >> 5) & 3;
  int double_scan = V.cr[0x09] >> 7;
  int multi_scan;
  if (V.cr[0x17] & 1)
    multi_scan = (((V.cr[0x09] & 0x1f) + 1) << double_scan) - 1;
  else
    multi_scan = double_scan;
  int multi_run = multi_scan;
  if ((shift_control == 0 || shift_control == 1) && (vga_sr(1) & 8))
    disp_width <<= 1;
  int kind, bpp = vbe_enabled() ? V.vbe_regs[3] : 0;
  if (shift_control == 0) {
    vga_palette16();
    kind = (vga_sr(1) & 8) ? 1 : 0;
  } else if (shift_control == 1) {
    vga_palette16();
    kind = (vga_sr(1) & 8) ? 3 : 2;
  } else if (bpp == 0) {
    vga_palette256();
    kind = 4;
  } else if (bpp == 8) {
    vga_palette256();
    kind = 5;
  } else {
    kind = 6;
  }
  if (vga_alloc(disp_width, height) < 0)
    return -1;
  uint32_t addr1 = start_addr * 4;
  int y1 = 0;
  for (int y = 0; y < height; y++) {
    uint32_t addr = addr1;
    if (!(V.cr[0x17] & 1)) {
      int shift = 14 + ((V.cr[0x17] >> 6) & 1);
      addr = (addr & ~(1u << shift)) | ((uint32_t)(y1 & 1) << shift);
    }
    if (!(V.cr[0x17] & 2))
      addr = (addr & ~0x8000u) | ((uint32_t)(y1 & 2) << 14);
    vga_line(g_vga_px + (size_t)y * disp_width, addr, width, kind, bpp);
    if (!multi_run) {
      int mask = (V.cr[0x17] & 3) ^ 3;
      if ((y1 & mask) == mask)
        addr1 += line_offset;
      y1++;
      multi_run = multi_scan;
    } else {
      multi_run--;
    }
    if ((uint32_t)y == line_compare)
      addr1 = 0;
  }
  return 0;
}

// Devuelve el fotograma (ARGB) o NULL si no hay nada que dibujar.
static uint32_t *vga_render(int *pw, int *ph) {
  if (!g_vga_on || !g_lfb)
    return NULL;
  int rc;
  if (!(V.ar_index & 0x20)) { // pantalla apagada (PAS = 0): negro con el ultimo tamano
    if (g_vga_last_w <= 0 || g_vga_last_h <= 0 || !g_vga_px)
      return NULL;
    memset(g_vga_px, 0, (size_t)g_vga_last_w * g_vga_last_h * 4);
    rc = 0;
  } else if (V.gr[6] & 1) {
    rc = vga_draw_graphic();
  } else {
    rc = vga_draw_text();
  }
  if (rc < 0)
    return NULL;
  *pw = g_vga_last_w;
  *ph = g_vga_last_h;
  return g_vga_px;
}

// Texto de la pantalla (pruebas sin ventana): caracteres del plano 0.
static int vga_text_cell(unsigned r, unsigned c) {
  uint32_t lo, sa, lc;
  vga_offsets(&lo, &sa, &lc);
  uint64_t a = (uint64_t)sa * 4 + (uint64_t)r * lo + (uint64_t)c * 4;
  return a < VGA_LFB_SIZE ? g_lfb[a] : ' ';
}

static void vga_text_dims(int *cols, int *rows) {
  int cw, ch;
  vga_text_res(cols, rows, &cw, &ch);
  if (*cols < 20 || *cols > 200 || *rows < 4 || *rows > 100) {
    *cols = 80;
    *rows = 25;
  }
}

static void vga_dump_text(FILE *f) {
  int cols, rows;
  vga_text_dims(&cols, &rows);
  for (int r = 0; r < rows; r++) {
    char line[256];
    int n = 0;
    for (int c = 0; c < cols && n < 255; c++) {
      int chr = vga_text_cell((unsigned)r, (unsigned)c);
      line[n++] = (chr >= 32 && chr < 127) ? (char)chr : ' ';
    }
    while (n && line[n - 1] == ' ') n--;
    line[n] = 0;
    if (n || r < 3) fprintf(f, "%s\n", line);
  }
}

static int vga_text_contains(const char *s) {
  int cols, rows;
  vga_text_dims(&cols, &rows);
  int n = (int)strlen(s);
  for (int r = 0; r < rows; r++)
    for (int c = 0; c + n <= cols; c++) {
      int k = 0;
      while (k < n && vga_text_cell((unsigned)r, (unsigned)(c + k)) == (uint8_t)s[k])
        k++;
      if (k == n) return 1;
    }
  return 0;
}

// Memorias del dispositivo (VRAM/LFB y ROM). rom/rom_n pueden ser NULL.
static int vga_setup(const uint8_t *rom, size_t rom_n) {
  vga_reset();
  g_vga_lfb_fd = memfd_create("aurora-vm-lfb", MFD_CLOEXEC);
  if (g_vga_lfb_fd < 0 || ftruncate(g_vga_lfb_fd, VGA_LFB_SIZE) < 0)
    return -1;
  g_lfb = mmap(NULL, VGA_LFB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, g_vga_lfb_fd, 0);
  if (g_lfb == MAP_FAILED) {
    g_lfb = NULL;
    return -1;
  }
  g_vga_rom_fd = memfd_create("aurora-vm-vrom", MFD_CLOEXEC);
  if (g_vga_rom_fd < 0 || ftruncate(g_vga_rom_fd, VGA_ROM_SIZE) < 0)
    return -1;
  g_vrom = mmap(NULL, VGA_ROM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, g_vga_rom_fd, 0);
  if (g_vrom == MAP_FAILED) {
    g_vrom = NULL;
    return -1;
  }
  memset(g_vrom, 0, VGA_ROM_SIZE);
  if (rom && rom_n)
    memcpy(g_vrom, rom, rom_n > VGA_ROM_SIZE ? VGA_ROM_SIZE : rom_n);
  g_cursor_t0 = now_ns();
  g_vga_last_w = g_vga_last_h = 0;
  g_vga_on = 1;
  return 0;
}

#endif
