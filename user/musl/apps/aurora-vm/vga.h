// vga.h — tarjeta VGA estandar (Bochs/QEMU "stdvga", 1234:1111) de la mini-VM (F3d).
//
// SeaVGABIOS (third_party/seabios/vgabios.bin) se carga desde la ROM de expansion
// del dispositivo PCI (00:02.0) y da el int 10h. Modelo deliberadamente simple:
//   - 0xA0000-0xBFFFF es RAM normal del huesped (sin salidas): sirve el texto
//     (0xB8000, la fuente va en 0xA0000 + car*32, como el plano 2) y el modo 13h.
//     Los modos graficos planares de 16 colores NO estan soportados todavia.
//   - Modos VBE por el BAR0 (lineal, 16 MiB, un memfd mapeado por el EPT): 8/15/16/24/32
//     bits, con los registros Bochs (puertos 0x1CE/0x1CF).
//   - El VMM dibuja el fotograma leyendo esa memoria (vga_render).
#ifndef AVM_VGA_H
#define AVM_VGA_H

#define VGA_LFB_SIZE (16u << 20)
#define VGA_ROM_SIZE 0x10000u

static int g_vga_on;
static uint8_t g_vga_cfg[256];
static uint32_t g_vga_bar0, g_vga_rom;
static uint16_t g_vga_cmd;
static uint8_t *g_lfb;
static uint8_t *g_vrom;
static int g_vga_lfb_fd = -1, g_vga_rom_fd = -1;
static uint64_t g_vga_lfb_gpa, g_vga_rom_gpa;

static uint8_t v_crtc[0x19], v_seq[5], v_gc[9], v_atc[0x15], v_misc;
static unsigned v_crtc_i, v_seq_i, v_gc_i, v_atc_i, v_atc_ff;
static uint8_t v_dac[256][3];
static unsigned v_dac_rd, v_dac_wr, v_dac_rsub, v_dac_wsub;
static uint8_t v_dac_mask;
static uint16_t v_dispi[11], v_dispi_idx;
static uint64_t v_retrace_t0;

static void vga_remap(void);

static void vga_reset(void) {
  memset(v_crtc, 0, sizeof(v_crtc));
  memset(v_seq, 0, sizeof(v_seq));
  memset(v_gc, 0, sizeof(v_gc));
  memset(v_atc, 0, sizeof(v_atc));
  memset(v_dac, 0, sizeof(v_dac));
  v_misc = 0;
  v_crtc_i = v_seq_i = v_gc_i = v_atc_i = v_atc_ff = 0;
  v_dac_rd = v_dac_wr = v_dac_rsub = v_dac_wsub = 0;
  v_dac_mask = 0xFF;
  memset(v_dispi, 0, sizeof(v_dispi));
  v_dispi[0] = 0xB0C5;
  v_dispi[1] = 640;
  v_dispi[2] = 480;
  v_dispi[3] = 8;
  v_dispi[6] = 640;
  v_dispi[7] = 480;
  v_dispi[0xA] = (uint16_t)(VGA_LFB_SIZE >> 16);
  v_dispi_idx = 0;
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
// Puertos
// ---------------------------------------------------------------------------
static int vga_dac8(void) { return v_dispi[4] & 0x20; }

static int vga_port_ours(uint16_t p) {
  return (p >= 0x3C0 && p <= 0x3CF) || p == 0x3B4 || p == 0x3B5 || p == 0x3BA || p == 0x3D4 ||
         p == 0x3D5 || p == 0x3DA || p == 0x1CE || p == 0x1CF;
}

static void vga_out1(uint16_t port, uint8_t v) {
  switch (port) {
  case 0x3C0:
    if (!v_atc_ff) {
      v_atc_i = v & 0x1F;
    } else if (v_atc_i < sizeof(v_atc)) {
      v_atc[v_atc_i] = v;
    }
    v_atc_ff ^= 1;
    break;
  case 0x3C2: v_misc = v; break;
  case 0x3C4: v_seq_i = v; break;
  case 0x3C5: if (v_seq_i < sizeof(v_seq)) v_seq[v_seq_i] = v; break;
  case 0x3C6: v_dac_mask = v; break;
  case 0x3C7: v_dac_rd = v; v_dac_rsub = 0; break;
  case 0x3C8: v_dac_wr = v; v_dac_wsub = 0; break;
  case 0x3C9: {
    uint8_t c = vga_dac8() ? v : (uint8_t)(((v & 0x3F) << 2) | ((v & 0x3F) >> 4));
    v_dac[v_dac_wr & 0xFF][v_dac_wsub] = c;
    if (++v_dac_wsub == 3) { v_dac_wsub = 0; v_dac_wr++; }
    break;
  }
  case 0x3CE: v_gc_i = v; break;
  case 0x3CF: if (v_gc_i < sizeof(v_gc)) v_gc[v_gc_i] = v; break;
  case 0x3B4: case 0x3D4: v_crtc_i = v; break;
  case 0x3B5: case 0x3D5: if (v_crtc_i < sizeof(v_crtc)) v_crtc[v_crtc_i] = v; break;
  default: break;
  }
}

static uint8_t vga_in1(uint16_t port) {
  switch (port) {
  case 0x3C0: return (uint8_t)v_atc_i;
  case 0x3C1: return v_atc_i < sizeof(v_atc) ? v_atc[v_atc_i] : 0;
  case 0x3C2: return 0;
  case 0x3C4: return (uint8_t)v_seq_i;
  case 0x3C5: return v_seq_i < sizeof(v_seq) ? v_seq[v_seq_i] : 0;
  case 0x3C6: return v_dac_mask;
  case 0x3C7: return 0;
  case 0x3C8: return (uint8_t)v_dac_wr;
  case 0x3C9: {
    uint8_t c = v_dac[v_dac_rd & 0xFF][v_dac_rsub];
    if (!vga_dac8()) c >>= 2;
    if (++v_dac_rsub == 3) { v_dac_rsub = 0; v_dac_rd++; }
    return c;
  }
  case 0x3CC: return v_misc;
  case 0x3CE: return (uint8_t)v_gc_i;
  case 0x3CF: return v_gc_i < sizeof(v_gc) ? v_gc[v_gc_i] : 0;
  case 0x3B4: case 0x3D4: return (uint8_t)v_crtc_i;
  case 0x3B5: case 0x3D5: return v_crtc_i < sizeof(v_crtc) ? v_crtc[v_crtc_i] : 0;
  case 0x3BA: case 0x3DA: {
    v_atc_ff = 0;
    // Alterna retrazo vertical / pantalla activa: las esperas de la BIOS terminan.
    uint64_t t = (now_ns() - v_retrace_t0) / 4000; // ~4 us por fase
    return (uint8_t)(((t & 1) ? 1 : 0) | (((t >> 3) & 1) ? 8 : 0));
  }
  default: return 0;
  }
}

static void vga_dispi_write(uint16_t val) {
  unsigned i = v_dispi_idx;
  if (i > 10)
    return;
  switch (i) {
  case 0: return; // ID solo lectura
  case 1: case 2: case 6: case 7:
    v_dispi[i] = val;
    break;
  case 3:
    if (val == 4 || val == 8 || val == 15 || val == 16 || val == 24 || val == 32)
      v_dispi[3] = val;
    break;
  case 4:
    if ((val & 1) && !(val & 0x80)) { // al activar, limpia la memoria de video
      size_t bytes = (size_t)v_dispi[2] * v_dispi[6] * ((v_dispi[3] + 7u) / 8u);
      if (g_lfb && bytes > VGA_LFB_SIZE) bytes = VGA_LFB_SIZE;
      if (g_lfb) memset(g_lfb, 0, bytes);
    }
    if (val & 1) { // la pantalla virtual por omision es la visible
      if (!v_dispi[6] || v_dispi[6] < v_dispi[1]) v_dispi[6] = v_dispi[1];
      if (!v_dispi[7] || v_dispi[7] < v_dispi[2]) v_dispi[7] = v_dispi[2];
    }
    v_dispi[4] = val;
    break;
  default:
    v_dispi[i] = val;
  }
}

static uint16_t vga_dispi_read(void) {
  unsigned i = v_dispi_idx;
  if (i > 10)
    return 0;
  if (v_dispi[4] & 2) { // GETCAPS: maximos
    if (i == 1) return 2560;
    if (i == 2) return 1600;
    if (i == 3) return 32;
  }
  return v_dispi[i];
}

static int vga_out(uint16_t port, uint32_t v, int size) {
  if (!g_vga_on || !vga_port_ours(port))
    return 0;
  if (port == 0x1CE) { v_dispi_idx = (uint16_t)v; return 1; }
  if (port == 0x1CF) { vga_dispi_write((uint16_t)v); return 1; }
  vga_out1(port, (uint8_t)v);
  if (size >= 2 && (port == 0x3C4 || port == 0x3CE || port == 0x3D4 || port == 0x3B4))
    vga_out1((uint16_t)(port + 1), (uint8_t)(v >> 8));
  return 1;
}

static int vga_in(uint16_t port, int size, uint32_t *v) {
  if (!g_vga_on || !vga_port_ours(port))
    return 0;
  if (port == 0x1CE) { *v = v_dispi_idx; return 1; }
  if (port == 0x1CF) { *v = vga_dispi_read(); return 1; }
  *v = vga_in1(port);
  if (size >= 2 && (port == 0x3C4 || port == 0x3CE || port == 0x3D4 || port == 0x3B4))
    *v |= (uint32_t)vga_in1((uint16_t)(port + 1)) << 8;
  return 1;
}

// ---------------------------------------------------------------------------
// Dibujo
// ---------------------------------------------------------------------------
static uint32_t *g_vga_px;
static size_t g_vga_px_cap;
static const char *g_vga_unsup; // motivo si el modo actual no se puede dibujar

static uint32_t dac_argb(unsigned i) {
  return 0xFF000000u | ((uint32_t)v_dac[i & 0xFF][0] << 16) | ((uint32_t)v_dac[i & 0xFF][1] << 8) |
         v_dac[i & 0xFF][2];
}

static int vga_alloc(int w, int h) {
  size_t need = (size_t)w * (size_t)h;
  if (need > g_vga_px_cap) {
    uint32_t *n = realloc(g_vga_px, need * 4);
    if (!n)
      return -1;
    g_vga_px = n;
    g_vga_px_cap = need;
  }
  return 0;
}

static unsigned vga_text_cols(void) { return (unsigned)v_crtc[1] + 1; }
static unsigned vga_text_char_h(void) { return (unsigned)(v_crtc[9] & 0x1F) + 1; }
static unsigned vga_text_rows(void) {
  unsigned vde = v_crtc[0x12] | ((unsigned)(v_crtc[7] & 2) << 7) | ((unsigned)(v_crtc[7] & 0x40) << 3);
  return (vde + 1) / vga_text_char_h();
}
static uint32_t vga_text_base(void) {
  switch ((v_gc[6] >> 2) & 3) {
  case 0: case 1: return 0xA0000; // el modo texto normal usa 0xB8000, pero 0/1 son validos
  case 2: return 0xB0000;
  default: return 0xB8000;
  }
}

// Devuelve el fotograma (ARGB) o NULL si no hay nada que dibujar.
static uint32_t *vga_render(int *pw, int *ph) {
  g_vga_unsup = NULL;
  if (v_dispi[4] & 1) { // modo VBE lineal
    int w = v_dispi[1], h = v_dispi[2], bpp = v_dispi[3];
    if (w <= 0 || h <= 0 || w > 2560 || h > 1600 || !g_lfb)
      return NULL;
    if (bpp == 4) {
      g_vga_unsup = "VBE de 4 bits";
      return NULL;
    }
    if (vga_alloc(w, h) < 0)
      return NULL;
    unsigned bpl = (unsigned)((bpp + 7) / 8);
    size_t stride = (size_t)(v_dispi[6] ? v_dispi[6] : w) * bpl;
    size_t base = (size_t)v_dispi[9] * stride + (size_t)v_dispi[8] * bpl;
    for (int y = 0; y < h; y++) {
      size_t row = base + (size_t)y * stride;
      uint32_t *o = g_vga_px + (size_t)y * w;
      if (row + (size_t)w * bpl > VGA_LFB_SIZE) {
        memset(o, 0, (size_t)w * 4);
        continue;
      }
      const uint8_t *s = g_lfb + row;
      for (int x = 0; x < w; x++) {
        uint32_t c;
        switch (bpp) {
        case 8: c = dac_argb(s[x]); break;
        case 15: {
          unsigned p = s[2 * x] | (s[2 * x + 1] << 8);
          c = 0xFF000000u | (((p >> 10) & 31) * 255 / 31) << 16 | (((p >> 5) & 31) * 255 / 31) << 8 |
              ((p & 31) * 255 / 31);
          break;
        }
        case 16: {
          unsigned p = s[2 * x] | (s[2 * x + 1] << 8);
          c = 0xFF000000u | (((p >> 11) & 31) * 255 / 31) << 16 | (((p >> 5) & 63) * 255 / 63) << 8 |
              ((p & 31) * 255 / 31);
          break;
        }
        case 24: c = 0xFF000000u | (uint32_t)s[3 * x + 2] << 16 | (uint32_t)s[3 * x + 1] << 8 | s[3 * x]; break;
        default: c = 0xFF000000u | (uint32_t)s[4 * x + 2] << 16 | (uint32_t)s[4 * x + 1] << 8 | s[4 * x]; break;
        }
        o[x] = c;
      }
    }
    *pw = w;
    *ph = h;
    return g_vga_px;
  }
  if (v_gc[6] & 1) { // modo grafico
    if (v_gc[5] & 0x40) { // 256 colores (modo 13h), doble escaneo y reloj/2 -> 320x200
      int w = (int)(vga_text_cols() * 8) / ((v_seq[1] & 8) ? 2 : 1);
      int vde = (int)(v_crtc[0x12] | ((unsigned)(v_crtc[7] & 2) << 7) | ((unsigned)(v_crtc[7] & 0x40) << 3)) + 1;
      int h = vde / ((v_crtc[9] & 0x80) ? 2 : 1);
      if (w < 8 || h < 8 || w > 1024 || h > 768)
        return NULL;
      int sc = w <= 400 ? 2 : 1;
      if (vga_alloc(w * sc, h * sc) < 0)
        return NULL;
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
          size_t a = 0xA0000 + (size_t)y * (size_t)w + (size_t)x;
          uint32_t c = a < g_ram_size ? dac_argb(g_ram[a]) : 0xFF000000u;
          for (int dy = 0; dy < sc; dy++)
            for (int dx = 0; dx < sc; dx++)
              g_vga_px[(size_t)(y * sc + dy) * (size_t)(w * sc) + (size_t)(x * sc + dx)] = c;
        }
      *pw = w * sc;
      *ph = h * sc;
      return g_vga_px;
    }
    g_vga_unsup = "modo grafico planar de 16 colores";
    return NULL;
  }
  // Modo texto
  unsigned cols = vga_text_cols(), rows = vga_text_rows(), ch = vga_text_char_h();
  unsigned cw = (v_seq[1] & 1) ? 8 : 9;
  if (cols < 20 || cols > 200 || rows < 4 || rows > 100 || ch < 4 || ch > 32)
    return NULL;
  int w = (int)(cols * cw), h = (int)(rows * ch);
  if (vga_alloc(w, h) < 0)
    return NULL;
  uint32_t tbase = vga_text_base() + 2u * (((unsigned)v_crtc[0xC] << 8) | v_crtc[0xD]);
  uint32_t fbase = 0xA0000;
  int blink_en = v_atc[0x10] & 8;
  int blink_ph = (int)((now_ns() / 500000000ULL) & 1);
  unsigned curpos = ((unsigned)v_crtc[0xE] << 8) | v_crtc[0xF];
  int cur_on = !(v_crtc[0xA] & 0x20) && blink_ph;
  unsigned cs = v_crtc[0xA] & 0x1F, ce = v_crtc[0xB] & 0x1F;
  for (unsigned r = 0; r < rows; r++) {
    for (unsigned c = 0; c < cols; c++) {
      size_t a = tbase + 2u * (r * cols + c);
      uint8_t chr = 0, at = 0x07;
      if (a + 1 < g_ram_size) {
        chr = g_ram[a];
        at = g_ram[a + 1];
      }
      unsigned fg = at & 0x0F, bg = (at >> 4) & 0x0F;
      if (blink_en) {
        bg &= 7;
        if ((at & 0x80) && blink_ph) fg = bg;
      }
      uint32_t fgc = dac_argb(v_atc[fg & 0xF] & 0x3F), bgc = dac_argb(v_atc[bg & 0xF] & 0x3F);
      int is_cur = cur_on && (r * cols + c) == curpos;
      for (unsigned y = 0; y < ch; y++) {
        uint8_t bits = fbase + (size_t)chr * 32 + y < g_ram_size ? g_ram[fbase + (size_t)chr * 32 + y] : 0;
        if (is_cur && y >= cs && y <= ce) bits = 0xFF;
        uint32_t *o = g_vga_px + (size_t)(r * ch + y) * (size_t)w + (size_t)(c * cw);
        for (unsigned x = 0; x < cw; x++) {
          int on = x < 8 ? (bits >> (7 - x)) & 1 : (chr >= 0xC0 && chr <= 0xDF && (v_atc[0x10] & 4)) ? (bits & 1) : 0;
          o[x] = on ? fgc : bgc;
        }
      }
    }
  }
  *pw = w;
  *ph = h;
  return g_vga_px;
}

// Pantalla de texto como ASCII (pruebas sin ventana).
static void vga_dump_text(FILE *f) {
  unsigned cols = vga_text_cols(), rows = vga_text_rows();
  if (cols < 20 || cols > 200 || rows < 4 || rows > 100) {
    cols = 80;
    rows = 25;
  }
  uint32_t tbase = vga_text_base() + 2u * (((unsigned)v_crtc[0xC] << 8) | v_crtc[0xD]);
  for (unsigned r = 0; r < rows; r++) {
    char line[256];
    unsigned n = 0;
    for (unsigned c = 0; c < cols; c++) {
      size_t a = tbase + 2u * (r * cols + c);
      uint8_t chr = a < g_ram_size ? g_ram[a] : ' ';
      line[n++] = (chr >= 32 && chr < 127) ? (char)chr : ' ';
    }
    while (n && line[n - 1] == ' ') n--;
    line[n] = 0;
    if (n || r < 3) fprintf(f, "%s\n", line);
  }
}

static int vga_text_contains(const char *s) {
  unsigned cols = vga_text_cols(), rows = vga_text_rows();
  if (cols < 20 || cols > 200 || rows < 4 || rows > 100) {
    cols = 80;
    rows = 25;
  }
  uint32_t tbase = vga_text_base() + 2u * (((unsigned)v_crtc[0xC] << 8) | v_crtc[0xD]);
  size_t n = strlen(s);
  for (unsigned r = 0; r < rows; r++)
    for (unsigned c = 0; c + n <= cols; c++) {
      size_t k = 0;
      while (k < n) {
        size_t a = tbase + 2u * (r * cols + c + k);
        if (a >= g_ram_size || g_ram[a] != (uint8_t)s[k]) break;
        k++;
      }
      if (k == n) return 1;
    }
  return 0;
}

// Memorias del dispositivo (LFB y ROM). rom/rom_n pueden ser NULL.
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
  v_retrace_t0 = now_ns();
  g_vga_on = 1;
  return 0;
}

#endif
