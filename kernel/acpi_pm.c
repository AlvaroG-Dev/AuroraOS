// kernel/acpi_pm.c
#include "acpi_pm.h"
#include "acpi.h"
#include "acpi_aml.h"
#include "cpu.h"
#include "io.h"
#include "klog.h"
#include "paging.h"
#include "pci.h"
#include "power.h"
#include "string.h"

// ---------------------------------------------------------------------------
// Control de P-states vía ACPI.
//
// _PCT (Performance Control) devuelve un Package de 2 Buffers:
//   - buffer[0]: control register (típicamente MSR 0x199, IA32_PERF_CTL)
//   - buffer[1]: status register  (típicamente MSR 0x198, IA32_PERF_STATUS)
// En muchas máquinas son MSRs (space = 0x7F "Functional Fixed Hardware").
//
// _PSS (Performance Supported States) devuelve un Package de N Packages.
// Cada uno es:
//   [0] core_frequency (MHz)
//   [1] power (mW)
//   [2] transition_latency (us)
//   [3] bus_master_latency (us)
//   [4] control (valor a escribir en el control register)
//   [5] status (valor esperado en status register)
//
// _PPC (Performance Present Capabilities): índice del P-state más alto
// permitido. 0 = todos permitidos.
// ---------------------------------------------------------------------------

#define MAX_PSTATES 32

typedef struct {
  uint32_t core_freq_mhz;
  uint32_t power_mw;
  uint32_t transition_latency_us;
  uint32_t bus_master_latency_us;
  uint32_t control;
  uint32_t status;
} pstate_t;

static struct {
  int valid;
  uint64_t ctrl_reg;
  uint64_t status_reg;
  int space_ctrl;
  int space_status;
  pstate_t states[MAX_PSTATES];
  int state_count;
  uint32_t ppc;
} g_ps = {0};

static struct {
  int valid;
  uint8_t slp_typa;
  uint8_t slp_typb;
} g_s5 = {0}, g_s4 = {0}, g_s3 = {0};

// ---------------------------------------------------------------------------
// Escritura genérica de un registro _PCT
// ---------------------------------------------------------------------------
static void acpi_write_reg(uint64_t reg, int space, uint64_t val) {
  if (space == 0x7F || space == 0) {
    wrmsr((uint32_t)reg, val);
    return;
  }
  if (space == 1) {
    outl((uint16_t)reg, (uint32_t)val);
    return;
  }
}

// ---------------------------------------------------------------------------
// Helpers de decodificación AML de bajo nivel (para el byte-scan)
// ---------------------------------------------------------------------------
static int aml_decode_pkglen(const uint8_t *d, size_t len, size_t *pos,
                             uint32_t *out_len) {
  if (*pos >= len)
    return -1;
  uint8_t b0 = d[*pos];
  int extra = (b0 >> 6) & 3;
  if (extra == 0) {
    *out_len = b0 & 0x3F;
    (*pos)++;
    return 0;
  }
  if (*pos + 1 + extra > len)
    return -1;
  uint32_t v = b0 & 0x0F;
  (*pos)++;
  for (int i = 0; i < extra; i++) {
    v |= ((uint32_t)d[*pos]) << (4 + i * 8);
    (*pos)++;
  }
  *out_len = v;
  return 0;
}

// Lee un integer AML (0x00, 0x01, 0x0A xx, 0x0B xxxx, 0x0C xxxxxxxx,
// 0x0E x8). Actualiza *pos. Devuelve 0 si OK.
static int aml_read_integer(const uint8_t *d, size_t len, size_t *pos,
                            uint64_t *out) {
  if (*pos >= len)
    return -1;
  uint8_t op = d[*pos];
  if (op == 0x00) {
    *out = 0;
    (*pos)++;
    return 0;
  }
  if (op == 0x01) {
    *out = 1;
    (*pos)++;
    return 0;
  }
  if (op == 0xFF) {
    *out = 0xFFFFFFFFFFFFFFFFULL;
    (*pos)++;
    return 0;
  }
  if (op == 0x0A) {
    if (*pos + 2 > len)
      return -1;
    *out = d[*pos + 1];
    *pos += 2;
    return 0;
  }
  if (op == 0x0B) {
    if (*pos + 3 > len)
      return -1;
    *out = d[*pos + 1] | ((uint64_t)d[*pos + 2] << 8);
    *pos += 3;
    return 0;
  }
  if (op == 0x0C) {
    if (*pos + 5 > len)
      return -1;
    *out = d[*pos + 1] | ((uint64_t)d[*pos + 2] << 8) |
           ((uint64_t)d[*pos + 3] << 16) | ((uint64_t)d[*pos + 4] << 24);
    *pos += 5;
    return 0;
  }
  if (op == 0x0E) {
    if (*pos + 9 > len)
      return -1;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
      v |= ((uint64_t)d[*pos + 1 + i]) << (i * 8);
    *out = v;
    *pos += 9;
    return 0;
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Byte-scan: _PSS
//
// Encoding: 08 5F 50 53 53 12 <PkgLen> <NumElem> <Elem0> <Elem1> ...
// Cada Elem: 12 <PkgLen> <6 integers>
// ---------------------------------------------------------------------------
extern const uint8_t *acpi_get_dsdt(void);
extern int acpi_get_ssdt_count(void);
extern const uint8_t *acpi_get_ssdt(int i);

static int parse_pss_block(const uint8_t *data, size_t len, size_t pos,
                           pstate_t *out, int *out_count, int max) {
  if (pos + 6 > len)
    return -1;
  if (data[pos] != 0x08)
    return -1;
  if (data[pos + 1] != 0x5F || data[pos + 2] != 0x50 || data[pos + 3] != 0x53 ||
      data[pos + 4] != 0x53)
    return -1;
  if (data[pos + 5] != 0x12 && data[pos + 5] != 0x13)
    return -1;

  size_t cursor = pos + 6;
  uint32_t pkglen;
  if (aml_decode_pkglen(data, len, &cursor, &pkglen) != 0)
    return -1;
  size_t pkg_end = cursor + pkglen;

  if (cursor >= pkg_end)
    return -1;
  uint8_t nelem = data[cursor++];

  LOG_INFO("[ACPI PM] _PSS directo: %u elementos en off=0x%zx", nelem, pos);

  int n = 0;
  for (uint32_t i = 0; i < nelem && cursor < pkg_end && n < max; i++) {
    if (data[cursor] != 0x12) {
      LOG_WARN("[ACPI PM] _PSS[%u]: esperaba 0x12, leí 0x%02x @ 0x%zx", i,
               data[cursor], cursor);
      break;
    }
    cursor++;
    uint32_t inner_len;
    if (aml_decode_pkglen(data, len, &cursor, &inner_len) != 0)
      break;
    size_t inner_end = cursor + inner_len;

    if (cursor >= inner_end)
      break;
    uint8_t inner_n = data[cursor++];
    uint64_t vals[6] = {0};
    int got = 0;
    for (uint32_t j = 0; j < inner_n && cursor < inner_end && got < 6; j++) {
      if (aml_read_integer(data, len, &cursor, &vals[got]) != 0) {
        LOG_WARN("[ACPI PM] _PSS[%u][%d]: integer ilegible @ 0x%zx", i, got,
                 cursor);
        break;
      }
      got++;
    }

    if (got >= 6) {
      out[n].core_freq_mhz = (uint32_t)vals[0];
      out[n].power_mw = (uint32_t)vals[1];
      out[n].transition_latency_us = (uint32_t)vals[2];
      out[n].bus_master_latency_us = (uint32_t)vals[3];
      out[n].control = (uint32_t)vals[4];
      out[n].status = (uint32_t)vals[5];
      LOG_INFO("[ACPI PM]   _PSS[%d]: freq=%u MHz power=%u mW ctrl=0x%x", n,
               out[n].core_freq_mhz, out[n].power_mw, out[n].control);
      n++;
    }
    cursor = inner_end;
  }

  *out_count = n;
  return n > 0 ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Byte-scan: _PCT
//
// Encoding: 08 5F 50 43 54 12 <PkgLen> <NumElem=2> <Buf1> <Buf2>
// Cada Buf: 11 <PkgLen> <BufSize> <Bytes>
// El buffer es un Generic Address Structure (GAS) de 12 bytes:
//   [0] space_id (0=Memory, 1=IO, 0x7F=FFH/MSR)
//   [1] bit_width
//   [2] bit_offset
//   [3] access_size
//   [4..11] address (u64 LE)
// ---------------------------------------------------------------------------
static int parse_pct_block(const uint8_t *data, size_t len, size_t pos,
                           uint64_t *ctrl_addr, int *ctrl_space,
                           uint64_t *stat_addr, int *stat_space) {
  if (pos + 6 > len)
    return -1;
  if (data[pos] != 0x08)
    return -1;
  if (data[pos + 1] != 0x5F || data[pos + 2] != 0x50 || data[pos + 3] != 0x43 ||
      data[pos + 4] != 0x54)
    return -1;
  if (data[pos + 5] != 0x12)
    return -1;

  size_t cursor = pos + 6;
  uint32_t pkglen;
  if (aml_decode_pkglen(data, len, &cursor, &pkglen) != 0)
    return -1;
  size_t pkg_end = cursor + pkglen;

  if (cursor >= pkg_end)
    return -1;
  uint8_t nelem = data[cursor++];
  if (nelem < 2)
    return -1;

  uint64_t addrs[2] = {0};
  int spaces[2] = {0};
  int got = 0;

  for (uint32_t i = 0; i < 2 && cursor < pkg_end; i++) {
    if (data[cursor] != 0x11) {
      LOG_WARN("[ACPI PM] _PCT: buf %u no empieza con 0x11 (0x%02x @ 0x%zx)", i,
               data[cursor], cursor);
      break;
    }
    cursor++;
    uint32_t buf_pkg;
    if (aml_decode_pkglen(data, len, &cursor, &buf_pkg) != 0)
      break;
    size_t buf_end = cursor + buf_pkg;

    if (cursor >= buf_end)
      break;
    uint8_t buf_size = data[cursor++];
    if (buf_size < 12 || cursor + 12 > buf_end) {
      LOG_WARN("[ACPI PM] _PCT: buf %u GAS demasiado corto (%u bytes)", i,
               buf_size);
      cursor = buf_end;
      continue;
    }

    // Parsear Generic Address Structure
    spaces[i] = data[cursor + 0];
    uint64_t addr = 0;
    for (int k = 0; k < 8; k++)
      addr |= ((uint64_t)data[cursor + 4 + k]) << (k * 8);
    addrs[i] = addr;
    LOG_INFO("[ACPI PM] _PCT[%u]: space=0x%x addr=0x%lx", i, spaces[i],
             (unsigned long)addr);
    got++;

    cursor = buf_end;
  }

  if (got < 2)
    return -1;

  *ctrl_addr = addrs[0];
  *ctrl_space = spaces[0];
  *stat_addr = addrs[1];
  *stat_space = spaces[1];
  return 0;
}

// ---------------------------------------------------------------------------
// Parseadores AML normales
// ---------------------------------------------------------------------------
static void parse_pct(aml_object_t *pct) {
  if (!pct || pct->type != AML_PACKAGE || pct->package.count < 2)
    return;
  aml_object_t *ctrl = pct->package.elems[0];
  aml_object_t *stat = pct->package.elems[1];
  if (!ctrl || !stat || ctrl->type != AML_BUFFER || stat->type != AML_BUFFER)
    return;
  if (ctrl->buffer.len < 12 || stat->buffer.len < 12)
    return;

  uint8_t space_c = ctrl->buffer.data[0];
  uint8_t space_s = stat->buffer.data[0];

  uint64_t addr_c = 0, addr_s = 0;
  for (int i = 0; i < 8 && i + 4 < (int)ctrl->buffer.len; i++)
    addr_c |= ((uint64_t)ctrl->buffer.data[4 + i]) << (i * 8);
  for (int i = 0; i < 8 && i + 4 < (int)stat->buffer.len; i++)
    addr_s |= ((uint64_t)stat->buffer.data[4 + i]) << (i * 8);

  g_ps.ctrl_reg = addr_c;
  g_ps.status_reg = addr_s;
  g_ps.space_ctrl = space_c;
  g_ps.space_status = space_s;

  LOG_INFO("[ACPI PM] _PCT (AML): ctrl_reg=0x%lx space=%u status_reg=0x%lx "
           "space=%u",
           (unsigned long)addr_c, space_c, (unsigned long)addr_s, space_s);
}

static void parse_pss(aml_object_t *pss) {
  if (!pss || pss->type != AML_PACKAGE)
    return;
  int n = 0;
  for (uint32_t i = 0; i < pss->package.count && n < MAX_PSTATES; i++) {
    aml_object_t *e = pss->package.elems[i];
    if (!e || e->type != AML_PACKAGE || e->package.count < 6)
      continue;
    pstate_t *ps = &g_ps.states[n];
    ps->core_freq_mhz = (uint32_t)aml_to_integer(e->package.elems[0]);
    ps->power_mw = (uint32_t)aml_to_integer(e->package.elems[1]);
    ps->transition_latency_us = (uint32_t)aml_to_integer(e->package.elems[2]);
    ps->bus_master_latency_us = (uint32_t)aml_to_integer(e->package.elems[3]);
    ps->control = (uint32_t)aml_to_integer(e->package.elems[4]);
    ps->status = (uint32_t)aml_to_integer(e->package.elems[5]);
    LOG_INFO("[ACPI PM]   _PSS[%d]: freq=%u MHz ctrl=0x%x", n,
             ps->core_freq_mhz, ps->control);
    n++;
  }
  g_ps.state_count = n;
  LOG_INFO("[ACPI PM] _PSS (AML): %d P-states disponibles", n);
}

static int parse_sleep_state(const char *name, uint8_t *a, uint8_t *b) {
  aml_object_t *s = aml_eval(name);
  if (!s)
    return -1;
  if (s->type != AML_PACKAGE || s->package.count < 2)
    return -1;
  *a = (uint8_t)aml_to_integer(s->package.elems[0]);
  *b = (uint8_t)aml_to_integer(s->package.elems[1]);
  return 0;
}

// ---------------------------------------------------------------------------
// Byte-scan sobre todas las tablas (DSDT + SSDTs).
// Recorre buscando el patrón Name(_PSS, ...) y Name(_PCT, ...).
// ---------------------------------------------------------------------------
static const uint8_t *g_tables[16];
static uint32_t g_table_lens[16];
static int g_table_count = 0;

static void collect_tables(void) {
  if (g_table_count > 0)
    return;
  const uint8_t *dsdt = acpi_get_dsdt();
  if (dsdt) {
    uint32_t l = *(const uint32_t *)(dsdt + 4);
    if (l > 36) {
      g_tables[g_table_count] = dsdt + 36;
      g_table_lens[g_table_count] = l - 36;
      g_table_count++;
    }
  }
  int n = acpi_get_ssdt_count();
  for (int i = 0; i < n && g_table_count < 16; i++) {
    const uint8_t *s = acpi_get_ssdt(i);
    if (!s)
      continue;
    uint32_t l = *(const uint32_t *)(s + 4);
    if (l <= 36)
      continue;
    g_tables[g_table_count] = s + 36;
    g_table_lens[g_table_count] = l - 36;
    g_table_count++;
  }
}

static int scan_pss(void) {
  collect_tables();
  for (int t = 0; t < g_table_count; t++) {
    const uint8_t *body = g_tables[t];
    size_t len = g_table_lens[t];
    for (size_t off = 0; off + 6 < len; off++) {
      if (body[off] == 0x08 && body[off + 1] == 0x5F && body[off + 2] == 0x50 &&
          body[off + 3] == 0x53 && body[off + 4] == 0x53) {
        pstate_t tmp[MAX_PSTATES];
        int cnt = 0;
        if (parse_pss_block(body, len, off, tmp, &cnt, MAX_PSTATES) == 0 &&
            cnt > 0) {
          memcpy(g_ps.states, tmp, sizeof(pstate_t) * cnt);
          g_ps.state_count = cnt;
          LOG_INFO("[ACPI PM] _PSS obtenido por byte-scan: %d P-states", cnt);
          return 0;
        }
      }
    }
  }
  return -1;
}

static int scan_pct(void) {
  collect_tables();
  for (int t = 0; t < g_table_count; t++) {
    const uint8_t *body = g_tables[t];
    size_t len = g_table_lens[t];
    for (size_t off = 0; off + 6 < len; off++) {
      if (body[off] == 0x08 && body[off + 1] == 0x5F && body[off + 2] == 0x50 &&
          body[off + 3] == 0x43 && body[off + 4] == 0x54) {
        uint64_t ca = 0, sa = 0;
        int cs = 0, ss = 0;
        if (parse_pct_block(body, len, off, &ca, &cs, &sa, &ss) == 0) {
          g_ps.ctrl_reg = ca;
          g_ps.space_ctrl = cs;
          g_ps.status_reg = sa;
          g_ps.space_status = ss;
          LOG_INFO("[ACPI PM] _PCT obtenido por byte-scan: ctrl=0x%lx/%u "
                   "status=0x%lx/%u",
                   (unsigned long)ca, cs, (unsigned long)sa, ss);
          return 0;
        }
      }
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
// Reemplaza la función acpi_pm_init completa en acpi_pm.c
int acpi_pm_init(void) {
  // -------- 1) Intentar por parser AML --------
  aml_object_t *pss = aml_eval("\\_PR_.CPU0._PSS");
  if (!pss)
    pss = aml_eval("\\_SB_.PR00._PSS");
  if (pss)
    parse_pss(pss);

  aml_object_t *pct = aml_eval("\\_PR_.CPU0._PCT");
  if (!pct)
    pct = aml_eval("\\_SB_.PR00._PCT");
  if (pct)
    parse_pct(pct);

  uint64_t ppc = 0;
  if (aml_get_integer("\\_PR_.CPU0._PPC", &ppc) == 0)
    g_ps.ppc = (uint32_t)ppc;

  // -------- 2) Fallback byte-scan para lo que falte --------
  if (g_ps.state_count == 0) {
    LOG_INFO("[ACPI PM] AML no encontró _PSS, probando byte-scan...");
    if (scan_pss() != 0)
      LOG_WARN("[ACPI PM] Byte-scan de _PSS tampoco encontró nada");
  }

  if (g_ps.ctrl_reg == 0) {
    LOG_INFO("[ACPI PM] AML no encontró _PCT, probando byte-scan...");
    if (scan_pct() != 0) {
      LOG_WARN("[ACPI PM] Byte-scan de _PCT tampoco encontró nada");

      // [NUEVO] Fallback para procesadores Intel
      uint32_t eax, ebx, ecx, edx;
      cpuid(0, 0, &eax, &ebx, &ecx, &edx);
      // Verificar si es "GenuineIntel"
      if (ebx == 0x756E6547 && edx == 0x49656E69 && ecx == 0x6C65746E) {
        if (g_ps.state_count > 0) {
          LOG_WARN("[ACPI PM] Fallback Intel activado: Usando IA32_PERF_CTL "
                   "(MSR 0x199)");
          g_ps.ctrl_reg = 0x199;
          g_ps.space_ctrl = 0x7F; // 0x7F = Functional Fixed Hardware (MSR)
          g_ps.status_reg = 0x198;
          g_ps.space_status = 0x7F;
        }
      }
    }
  }

  // -------- 3) Resumen --------
  g_ps.valid = (g_ps.state_count > 0 && g_ps.ctrl_reg != 0);
  LOG_INFO("[ACPI PM] P-states: valid=%d count=%d ppc=%u ctrl=0x%lx "
           "space=%d status=0x%lx",
           g_ps.valid, g_ps.state_count, g_ps.ppc, (unsigned long)g_ps.ctrl_reg,
           g_ps.space_ctrl, (unsigned long)g_ps.status_reg);

  // -------- 4) Sleep states --------
  if (parse_sleep_state("\\_S5_", &g_s5.slp_typa, &g_s5.slp_typb) == 0) {
    g_s5.valid = 1;
    LOG_INFO("[ACPI PM] _S5_: SLP_TYPa=0x%x SLP_TYPb=0x%x", g_s5.slp_typa,
             g_s5.slp_typb);
  }
  if (parse_sleep_state("\\_S4_", &g_s4.slp_typa, &g_s4.slp_typb) == 0)
    g_s4.valid = 1;
  if (parse_sleep_state("\\_S3_", &g_s3.slp_typa, &g_s3.slp_typb) == 0)
    g_s3.valid = 1;

  // -------- 5) Auto-aplicar máximo si tenemos control --------
  if (g_ps.valid) {
    LOG_INFO("[ACPI PM] Aplicando P-state máximo automáticamente...");
    acpi_pm_set_max_performance();
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Aplicar P-state máximo
// ---------------------------------------------------------------------------
int acpi_pm_set_max_performance(void) {
  if (!g_ps.valid)
    return -1;

  int best = 0;
  for (int i = 1; i < g_ps.state_count; i++) {
    if (g_ps.states[i].core_freq_mhz > g_ps.states[best].core_freq_mhz)
      best = i;
  }

  if (g_ps.ppc > 0 && (uint32_t)best > g_ps.ppc) {
    LOG_WARN("[ACPI PM] _PPC=%u limita el P-state máximo (queríamos %d)",
             g_ps.ppc, best);
    best = (int)g_ps.ppc;
  }

  uint32_t ctrl = g_ps.states[best].control;
  acpi_write_reg(g_ps.ctrl_reg, g_ps.space_ctrl, ctrl);

  // Verificar
  uint64_t current = 0;
  if (g_ps.space_status == 0x7F || g_ps.space_status == 0)
    current = rdmsr((uint32_t)g_ps.status_reg);

  LOG_INFO("[ACPI PM] P-state máximo aplicado: freq=%u MHz ctrl=0x%x "
           "(status leído=0x%lx)",
           g_ps.states[best].core_freq_mhz, ctrl, (unsigned long)current);
  return 0;
}

void acpi_pm_dump(void) {
  LOG_INFO("[ACPI PM] Estado:");
  LOG_INFO("  P-states: valid=%d count=%d ppc=%u", g_ps.valid, g_ps.state_count,
           g_ps.ppc);
  LOG_INFO("  ctrl_reg=0x%lx space=%d  status_reg=0x%lx space=%d",
           (unsigned long)g_ps.ctrl_reg, g_ps.space_ctrl,
           (unsigned long)g_ps.status_reg, g_ps.space_status);
  for (int i = 0; i < g_ps.state_count; i++) {
    LOG_INFO("    P%d: %u MHz (ctrl=0x%x)", i, g_ps.states[i].core_freq_mhz,
             g_ps.states[i].control);
  }
  LOG_INFO("  S5: valid=%d S4: valid=%d S3: valid=%d", g_s5.valid, g_s4.valid,
           g_s3.valid);
}

// ---------------------------------------------------------------------------
// Shutdown / Reboot / Suspend
// ---------------------------------------------------------------------------
// Acceso a PM1x_CNT: bloque legacy (E/S) y, si no hay, X_PM1x_CNT_BLK (GAS).
static uint16_t pm1_cnt_read(uint32_t blk, uint8_t space, uint64_t xaddr) {
  if (blk)
    return inw((uint16_t)blk);
  if (space == 1 && xaddr)
    return inw((uint16_t)xaddr);
  if (space == 0 && xaddr)
    return *(volatile uint16_t *)phys_to_virt(xaddr);
  return 0;
}

static void pm1_cnt_write(uint32_t blk, uint8_t space, uint64_t xaddr,
                          uint16_t v) {
  if (blk)
    outw((uint16_t)blk, v);
  else if (space == 1 && xaddr)
    outw((uint16_t)xaddr, v);
  else if (space == 0 && xaddr)
    *(volatile uint16_t *)phys_to_virt(xaddr) = v;
}

static int pm1a_present(const acpi_fadt_t *f) {
  return f->pm1a_cnt_blk || f->x_pm1a_cnt;
}

static void pm_delay_us(uint32_t us) {
  while (us--)
    inb(0x80);
}

// Activa el modo ACPI si el firmware lo dejo en modo legacy (SCI_EN=0):
// ACPI_ENABLE en SMI_CMD y espera a que SCI_EN suba. 0 = ACPI activo.
static int acpi_pm_ensure_enabled(const acpi_fadt_t *f) {
  if (f->flags & ACPI_FADT_HW_REDUCED)
    return 0;
  if (pm1_cnt_read(f->pm1a_cnt_blk, f->x_pm1a_cnt_space, f->x_pm1a_cnt) & 1)
    return 0;
  if (!f->smi_cmd || !f->acpi_enable) {
    LOG_WARN("[ACPI PM] SCI_EN=0 y sin SMI_CMD/ACPI_ENABLE");
    return -1;
  }
  LOG_INFO("[ACPI PM] SCI_EN=0: ACPI_ENABLE (0x%x) en SMI_CMD 0x%x",
           f->acpi_enable, f->smi_cmd);
  outb((uint16_t)f->smi_cmd, f->acpi_enable);
  for (int i = 0; i < 3000; i++) { // hasta ~3 s
    if (pm1_cnt_read(f->pm1a_cnt_blk, f->x_pm1a_cnt_space, f->x_pm1a_cnt) & 1)
      return 0;
    pm_delay_us(1000);
  }
  LOG_WARN("[ACPI PM] SCI_EN no subio tras ACPI_ENABLE");
  return -1;
}

// Entrada a un S-state. Como Linux (acpi_hw_legacy_sleep): primero SLP_TYP
// sin SLP_EN, luego con SLP_EN (varios chipsets Intel lo exigen). Vuelve si
// la maquina no se durmio/apago.
static int acpi_enter_sleep(uint8_t slp_typa, uint8_t slp_typb) {
  const acpi_fadt_t *f = acpi_get_fadt();
  if (!f || !f->valid || !pm1a_present(f)) {
    LOG_ERR("[ACPI PM] FADT sin PM1a_CNT, no puedo hacer S-state");
    return -1;
  }
  acpi_pm_ensure_enabled(f); // si falla se intenta igualmente

  __asm__ volatile("cli");

  // Limpiar WAK_STS (bit 15 de PM1_STS, escribir 1 lo borra).
  if (f->pm1a_evt_blk)
    outw((uint16_t)f->pm1a_evt_blk, 0x8000);
  if (f->pm1b_evt_blk)
    outw((uint16_t)f->pm1b_evt_blk, 0x8000);

  uint16_t a = pm1_cnt_read(f->pm1a_cnt_blk, f->x_pm1a_cnt_space, f->x_pm1a_cnt);
  a = (uint16_t)((a & ~(0x7u << 10) & ~(1u << 13)) |
                 ((uint16_t)(slp_typa & 0x7) << 10));
  uint16_t b = 0;
  int have_b = f->pm1b_cnt_blk || f->x_pm1b_cnt;
  if (have_b) {
    b = pm1_cnt_read(f->pm1b_cnt_blk, f->x_pm1b_cnt_space, f->x_pm1b_cnt);
    b = (uint16_t)((b & ~(0x7u << 10) & ~(1u << 13)) |
                   ((uint16_t)(slp_typb & 0x7) << 10));
  }

  pm1_cnt_write(f->pm1a_cnt_blk, f->x_pm1a_cnt_space, f->x_pm1a_cnt, a);
  if (have_b)
    pm1_cnt_write(f->pm1b_cnt_blk, f->x_pm1b_cnt_space, f->x_pm1b_cnt, b);

  a |= (1u << 13);
  pm1_cnt_write(f->pm1a_cnt_blk, f->x_pm1a_cnt_space, f->x_pm1a_cnt, a);
  if (have_b) {
    b |= (1u << 13);
    pm1_cnt_write(f->pm1b_cnt_blk, f->x_pm1b_cnt_space, f->x_pm1b_cnt, b);
  }

  // En S5 la maquina se apaga en milisegundos; esperar hasta 2 s.
  pm_delay_us(2000000);
  return -1;
}

int acpi_pm_poweroff(void) {
  const acpi_fadt_t *f = acpi_get_fadt();
  if (!f || !f->valid) {
    LOG_WARN("[ACPI PM] poweroff: sin FADT");
    return -1;
  }
  if (!g_s5.valid) {
    // No se inventa SLP_TYP: dejar que actue el siguiente backend.
    LOG_WARN("[ACPI PM] poweroff: el DSDT no define _S5_");
    return -1;
  }
  LOG_INFO("[ACPI PM] Apagado S5 (SLP_TYPa=%u SLP_TYPb=%u)", g_s5.slp_typa,
           g_s5.slp_typb);
  return acpi_enter_sleep(g_s5.slp_typa, g_s5.slp_typb);
}

// Reinicio por RESET_REG (FADT). Vuelve si no hay o no surte efecto.
int acpi_pm_restart(void) {
  const acpi_fadt_t *f = acpi_get_fadt();
  if (!f || !f->valid || !(f->flags & ACPI_FADT_RESET_REG_SUP) ||
      !f->reset_reg) {
    LOG_INFO("[ACPI PM] reinicio: sin RESET_REG");
    return -1;
  }
  LOG_INFO("[ACPI PM] reinicio: RESET_REG space=%u addr=0x%lx val=0x%x",
           f->reset_reg_space, (unsigned long)f->reset_reg, f->reset_value);
  switch (f->reset_reg_space) {
  case 0:
    *(volatile uint8_t *)phys_to_virt(f->reset_reg) = f->reset_value;
    break;
  case 1:
    outb((uint16_t)f->reset_reg, f->reset_value);
    break;
  case 2: // PCI config: bus 0, device en bits 32-47, funcion en 16-31
    pci_write_config_byte(0, (uint8_t)((f->reset_reg >> 32) & 0x1F),
                          (uint8_t)((f->reset_reg >> 16) & 0x7),
                          (uint8_t)(f->reset_reg & 0xFF), f->reset_value);
    break;
  default:
    return -1;
  }
  pm_delay_us(500000);
  return -1;
}

void acpi_pm_shutdown(void) { power_request(POWER_ACTION_OFF, "acpi_pm"); }

void acpi_pm_reboot(void) { power_request(POWER_ACTION_RESTART, "acpi_pm"); }

void acpi_pm_suspend(void) {
  LOG_INFO("[ACPI PM] Suspend (S3)");
  if (!g_s3.valid) {
    LOG_ERR("[ACPI PM] S3 no soportado");
    return;
  }
  (void)acpi_enter_sleep(g_s3.slp_typa, g_s3.slp_typb);
}