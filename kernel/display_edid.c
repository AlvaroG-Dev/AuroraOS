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
  e[40] ^= 1; // rompe el checksum
  TEST_ASSERT(!edid_parse(e, 128, &i), "EDID con checksum roto se rechaza");
  e[40] ^= 1;
  e[0] = 1;
  TEST_ASSERT(!edid_parse(e, 128, &i), "EDID sin cabecera se rechaza");
}

REGISTER_TEST("edid: analisis, checksum y modo preferido", edid_selftest);
