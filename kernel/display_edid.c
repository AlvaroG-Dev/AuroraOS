// kernel/display_edid.c
#include "display_edid.h"
#include "klog.h"
#include "string.h"
#include "test.h"

static uint8_t g_edid[256];
static uint32_t g_edid_len;
static struct edid_info g_info;

static bool block_ok(const uint8_t *b) {
  uint8_t s = 0;
  for (int i = 0; i < 128; i++)
    s = (uint8_t)(s + b[i]);
  return s == 0;
}

bool edid_parse(const uint8_t *e, uint32_t len, struct edid_info *o) {
  static const uint8_t hdr[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
  memset(o, 0, sizeof(*o));
  if (!e || len < 128 || (len & 127) || len > 256 || memcmp(e, hdr, 8) != 0)
    return false;
  for (uint32_t b = 0; b < len; b += 128)
    if (!block_ok(e + b))
      return false;
  o->blocks = len / 128;
  // Fabricante: 3 letras de 5 bits en bytes 8-9 (big endian).
  uint16_t m = (uint16_t)((e[8] << 8) | e[9]);
  o->vendor[0] = (char)('@' + ((m >> 10) & 31));
  o->vendor[1] = (char)('@' + ((m >> 5) & 31));
  o->vendor[2] = (char)('@' + (m & 31));
  o->product = (uint16_t)(e[10] | (e[11] << 8));
  // Tamano fisico en cm (bytes 21,22); 0 = proyector/desconocido.
  o->width_mm = (uint32_t)e[21] * 10;
  o->height_mm = (uint32_t)e[22] * 10;
  // Descriptores de 18 bytes en 54, 72, 90, 108.
  for (int i = 0; i < 4; i++) {
    const uint8_t *d = e + 54 + 18 * i;
    if (d[0] || d[1]) { // temporizacion detallada
      if (i == 0) {
        uint32_t clk = (uint32_t)(d[0] | (d[1] << 8)) * 10; // kHz
        uint32_t ha = d[2] | ((uint32_t)(d[4] & 0xF0) << 4);
        uint32_t hb = d[3] | ((uint32_t)(d[4] & 0x0F) << 8);
        uint32_t va = d[5] | ((uint32_t)(d[7] & 0xF0) << 4);
        uint32_t vb = d[6] | ((uint32_t)(d[7] & 0x0F) << 8);
        uint32_t hs = d[12] | ((uint32_t)(d[14] & 0xF0) << 4); // mm
        uint32_t vs = d[13] | ((uint32_t)(d[14] & 0x0F) << 8);
        o->pref_w = ha;
        o->pref_h = va;
        o->pref_khz = clk;
        if ((ha + hb) && (va + vb))
          o->pref_mhz_x100 =
              (uint32_t)((uint64_t)clk * 1000 * 100 / ((uint64_t)(ha + hb) * (va + vb)));
        if (hs && vs) { // mas preciso que los bytes 21/22
          o->width_mm = hs;
          o->height_mm = vs;
        }
      }
    } else if (d[3] == 0xFC && !o->name[0]) { // nombre del monitor
      int n = 0;
      for (int k = 5; k < 18 && n < 13; k++) {
        if (d[k] == '\n' || d[k] == 0)
          break;
        o->name[n++] = (char)d[k];
      }
      o->name[n] = 0;
    }
  }
  o->valid = true;
  return true;
}

static int mode_add(struct edid_mode *out, int n, int max, const struct edid_mode *m) {
  for (int i = 0; i < n; i++)
    if (out[i].hdisplay == m->hdisplay && out[i].vdisplay == m->vdisplay &&
        out[i].refresh_hz == m->refresh_hz) {
      if (m->detailed && !out[i].detailed)
        out[i] = *m; // la temporizacion exacta gana a la sintetica
      return n;
    }
  if (n >= max)
    return n;
  out[n] = *m;
  return n + 1;
}

// Temporizacion sintetica (GTF aproximada con blanking fijo) para los modos
// establecidos/estandar del EDID, que solo dan resolucion y refresco.
static void synth_mode(struct edid_mode *m, uint32_t w, uint32_t h, uint32_t hz) {
  memset(m, 0, sizeof(*m));
  m->hdisplay = (uint16_t)w;
  m->hsync_start = (uint16_t)(w + 48);
  m->hsync_end = (uint16_t)(w + 80);
  m->htotal = (uint16_t)(w + 160);
  m->vdisplay = (uint16_t)h;
  m->vsync_start = (uint16_t)(h + 3);
  m->vsync_end = (uint16_t)(h + 8);
  m->vtotal = (uint16_t)(h + 40);
  m->refresh_hz = hz;
  m->clock_khz = (uint32_t)((uint64_t)m->htotal * m->vtotal * hz / 1000);
  m->flags = EDID_MODE_PHSYNC | EDID_MODE_PVSYNC;
}

int edid_modes(const uint8_t *e, uint32_t len, struct edid_mode *out, int max) {
  struct edid_info info;
  if (!edid_parse(e, len, &info))
    return 0;
  int n = 0;
  // 1. Descriptores de temporizacion detallada (el primero es el preferido).
  for (int i = 0; i < 4; i++) {
    const uint8_t *d = e + 54 + 18 * i;
    if (!d[0] && !d[1])
      continue;
    struct edid_mode m;
    memset(&m, 0, sizeof(m));
    uint32_t ha = d[2] | ((uint32_t)(d[4] & 0xF0) << 4);
    uint32_t hb = d[3] | ((uint32_t)(d[4] & 0x0F) << 8);
    uint32_t va = d[5] | ((uint32_t)(d[7] & 0xF0) << 4);
    uint32_t vb = d[6] | ((uint32_t)(d[7] & 0x0F) << 8);
    uint32_t hso = d[8] | ((uint32_t)(d[11] & 0xC0) << 2);
    uint32_t hsw = d[9] | ((uint32_t)(d[11] & 0x30) << 4);
    uint32_t vso = (d[10] >> 4) | ((uint32_t)(d[11] & 0x0C) << 2);
    uint32_t vsw = (d[10] & 0x0F) | ((uint32_t)(d[11] & 0x03) << 4);
    if (!ha || !va || !(ha + hb) || !(va + vb))
      continue;
    m.hdisplay = (uint16_t)ha;
    m.hsync_start = (uint16_t)(ha + hso);
    m.hsync_end = (uint16_t)(ha + hso + hsw);
    m.htotal = (uint16_t)(ha + hb);
    m.vdisplay = (uint16_t)va;
    m.vsync_start = (uint16_t)(va + vso);
    m.vsync_end = (uint16_t)(va + vso + vsw);
    m.vtotal = (uint16_t)(va + vb);
    m.clock_khz = (uint32_t)(d[0] | (d[1] << 8)) * 10;
    uint64_t pix = (uint64_t)m.htotal * m.vtotal;
    m.refresh_hz = pix ? (uint32_t)(((uint64_t)m.clock_khz * 1000 + pix / 2) / pix) : 60;
    uint8_t f = d[17];
    if (f & 0x80)
      m.flags |= EDID_MODE_INTERLACED;
    if (f & 0x02)
      m.flags |= EDID_MODE_PHSYNC;
    if (f & 0x04)
      m.flags |= EDID_MODE_PVSYNC;
    m.detailed = true;
    n = mode_add(out, n, max, &m);
  }
  // 2. Temporizaciones establecidas (bytes 35-37).
  static const struct { uint8_t byte, bit; uint16_t w, h, hz; } est[] = {
      {35, 5, 640, 480, 60},  {35, 3, 640, 480, 72},  {35, 2, 640, 480, 75},
      {35, 1, 800, 600, 56},  {35, 0, 800, 600, 60},  {36, 7, 800, 600, 72},
      {36, 6, 800, 600, 75},  {36, 3, 1024, 768, 60}, {36, 2, 1024, 768, 70},
      {36, 1, 1024, 768, 75}, {36, 0, 1280, 1024, 75}};
  for (unsigned i = 0; i < sizeof(est) / sizeof(est[0]); i++)
    if (e[est[i].byte] & (1u << est[i].bit)) {
      struct edid_mode m;
      synth_mode(&m, est[i].w, est[i].h, est[i].hz);
      n = mode_add(out, n, max, &m);
    }
  // 3. Temporizaciones estandar (8 pares en 38-53).
  for (int i = 0; i < 8; i++) {
    uint8_t b0 = e[38 + 2 * i], b1 = e[39 + 2 * i];
    if ((b0 == 0x01 && b1 == 0x01) || b0 == 0x00)
      continue;
    uint32_t w = ((uint32_t)b0 + 31) * 8, h;
    switch (b1 >> 6) {
    case 0: h = w * 10 / 16; break; // 16:10
    case 1: h = w * 3 / 4; break;   // 4:3
    case 2: h = w * 4 / 5; break;   // 5:4
    default: h = w * 9 / 16; break; // 16:9
    }
    uint32_t hz = (uint32_t)(b1 & 0x3F) + 60;
    if (w < 64 || h < 64)
      continue;
    struct edid_mode m;
    synth_mode(&m, w, h, hz);
    n = mode_add(out, n, max, &m);
  }
  return n;
}

void display_edid_init(const uint8_t *edid, uint32_t len) {
  struct edid_info i;
  g_edid_len = 0;
  memset(&g_info, 0, sizeof(g_info));
  if (!edid_parse(edid, len, &i)) {
    LOG_INFO("[EDID] el firmware no entrego un EDID valido (len=%u)", len);
    return;
  }
  memcpy(g_edid, edid, len);
  g_edid_len = len;
  g_info = i;
  LOG_INFO("[EDID] %s '%s' %ux%u (%u mm x %u mm) pref %u.%02u Hz, %u bloque(s)",
           i.vendor, i.name, i.pref_w, i.pref_h, i.width_mm, i.height_mm,
           i.pref_mhz_x100 / 100, i.pref_mhz_x100 % 100, i.blocks);
}

const uint8_t *display_edid_data(uint32_t *len) {
  if (!g_edid_len)
    return NULL;
  if (len)
    *len = g_edid_len;
  return g_edid;
}

const struct edid_info *display_edid_info(void) { return g_info.valid ? &g_info : NULL; }

// Autotest con un EDID sintetico (1920x1080 @60, 60x34 cm... "AURORA").
static void edid_selftest(void) {
  uint8_t e[128] = {0};
  e[1] = e[2] = e[3] = e[4] = e[5] = e[6] = 0xFF;
  e[8] = 0x06; e[9] = 0xB2; // fabricante 'AUR' (A=1,U=21,R=18)
  e[21] = 53; e[22] = 30;
  uint8_t *d = e + 54;
  d[0] = 0x02; d[1] = 0x3A; // 148.5 MHz
  d[2] = 0x80; d[3] = 0x18; d[4] = 0x71; // ha=1920 hb=280
  d[5] = 0x38; d[6] = 0x2D; d[7] = 0x40; // va=1080 vb=45
  d[12] = 0x0A; d[13] = 0x00; d[14] = 0x00; // 10x0 mm (no se usa: vs=0)
  d = e + 72;
  d[3] = 0xFC; memcpy(d + 5, "AURORA\n", 7);
  uint8_t s = 0;
  for (int i = 0; i < 127; i++) s = (uint8_t)(s + e[i]);
  e[127] = (uint8_t)(0 - s);
  struct edid_info i;
  TEST_ASSERT(edid_parse(e, 128, &i), "EDID sintetico valido");
  TEST_ASSERT(i.pref_w == 1920 && i.pref_h == 1080, "EDID: modo preferido 1920x1080");
  TEST_ASSERT(i.pref_mhz_x100 >= 5990 && i.pref_mhz_x100 <= 6010, "EDID: refresco ~60 Hz");
  TEST_ASSERT(i.width_mm == 530 && i.height_mm == 300, "EDID: tamano fisico");
  TEST_ASSERT(strcmp(i.name, "AURORA") == 0, "EDID: nombre del monitor");
  TEST_ASSERT(strcmp(i.vendor, "AUR") == 0, "EDID: fabricante (PNP ID)");
  {
    // modos: el detallado 1920x1080@60 + establecidos 640x480@60 y 1024x768@60 + estandar 1280x720@60
    uint8_t m2[128];
    memcpy(m2, e, 128);
    m2[35] = 0x20;              // 640x480@60
    m2[36] = 0x08;              // 1024x768@60
    m2[38] = 0x81; m2[39] = 0xC0; // (0x81+31)*8 = 1280, 16:9 -> 720, 60 Hz
    uint8_t s2 = 0;
    for (int k = 0; k < 127; k++) s2 = (uint8_t)(s2 + m2[k]);
    m2[127] = (uint8_t)(0 - s2);
    struct edid_mode em[16];
    int nm = edid_modes(m2, 128, em, 16);
    TEST_ASSERT(nm == 4, "EDID: 4 modos (detallado + 2 establecidos + estandar), got %d", nm);
    TEST_ASSERT(em[0].hdisplay == 1920 && em[0].vdisplay == 1080 && em[0].detailed &&
                    em[0].refresh_hz == 60 && em[0].clock_khz == 148500 &&
                    em[0].hsync_start == 1920 + 0 && em[0].htotal == 2200,
                "EDID: el primero es el detallado con su temporizacion exacta");
    int has720 = 0, has480 = 0, has768 = 0;
    for (int k = 0; k < nm; k++) {
      has720 |= em[k].hdisplay == 1280 && em[k].vdisplay == 720;
      has480 |= em[k].hdisplay == 640 && em[k].vdisplay == 480;
      has768 |= em[k].hdisplay == 1024 && em[k].vdisplay == 768;
    }
    TEST_ASSERT(has720 && has480 && has768, "EDID: modos establecidos y estandar presentes");
  }
  e[40] ^= 1; // rompe el checksum
  TEST_ASSERT(!edid_parse(e, 128, &i), "EDID con checksum roto se rechaza");
  e[40] ^= 1;
  e[0] = 1;
  TEST_ASSERT(!edid_parse(e, 128, &i), "EDID sin cabecera se rechaza");
}

REGISTER_TEST("edid: analisis, checksum y modo preferido", edid_selftest);
