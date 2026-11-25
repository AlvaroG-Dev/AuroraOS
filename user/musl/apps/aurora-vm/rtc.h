// rtc.h — reloj MC146818 + CMOS (0x70/0x71) de la mini-VM, con la semantica de
// QEMU (hw/rtc/mc146818rtc.c): hora del huesped modificable (base + desfase sobre
// CLOCK_REALTIME del host), BCD/binario y 12/24 h, bit UIP en los ultimos 244 us de
// cada segundo, modo SET, interrupcion periodica (PF, tasas de 2 Hz a 8 kHz; los
// codigos 1 y 2 son 128/64 Hz como en QEMU), de fin de actualizacion (UF) y de
// alarma (AF, con "no importa" 0xC0). La linea IRQ8 sube con IRQF y baja al leer
// el registro C (como el chip real: si el SO no lee C, no hay mas interrupciones).
//
// Periodos que pasan sin que nadie lea C se funden en uno (como el chip y como
// QEMU sin driftfix): Windows calibra sus temporizadores sondeando PF contra el
// TSC y una rafaga de periodos "recuperados" le hacia rechazar el reloj (pantalla
// azul 0x5C, 0x110). PF se activa aunque PIE este apagado; IRQF solo con PIE.
#ifndef AVM_RTC_H
#define AVM_RTC_H

static void irq_set(int irq, int level);

static struct {
  uint8_t cmos[128];
  uint8_t index;
  int64_t base_rtc;      // segundos (epoch) en last_update
  uint64_t last_update;  // CLOCK_MONOTONIC (ns)
  int64_t offset;        // ns
  uint32_t period;       // periodo en ciclos de 32768 Hz (0 = sin PIE)
  uint64_t next_periodic;
  unsigned backlog;
  int64_t last_sec;      // ultimo segundo del huesped ya procesado (UF/AF)
  int irq_level;
} g_rtc;

#define REG_A 10
#define REG_B 11
#define REG_C 12
#define REG_D 13

static int rtc_running(void) {
  return !(g_rtc.cmos[REG_B] & 0x80) && (g_rtc.cmos[REG_A] & 0x70) <= 0x20;
}

static int64_t rtc_guest_ns(void) {
  return g_rtc.base_rtc * 1000000000LL + (int64_t)(now_ns() - g_rtc.last_update) + g_rtc.offset;
}

static int rtc_to_bcd(int a) {
  return (g_rtc.cmos[REG_B] & 0x04) ? a : ((a / 10) << 4) | (a % 10);
}
static int rtc_from_bcd(int a) {
  if ((a & 0xc0) == 0xc0)
    return -1;
  return (g_rtc.cmos[REG_B] & 0x04) ? a : ((a >> 4) * 10) + (a & 0x0f);
}

static void rtc_set_cmos(const struct tm *tm) {
  uint8_t *c = g_rtc.cmos;
  c[0] = (uint8_t)rtc_to_bcd(tm->tm_sec);
  c[2] = (uint8_t)rtc_to_bcd(tm->tm_min);
  if (c[REG_B] & 0x02) {
    c[4] = (uint8_t)rtc_to_bcd(tm->tm_hour);
  } else {
    int h = (tm->tm_hour % 12) ? tm->tm_hour % 12 : 12;
    c[4] = (uint8_t)rtc_to_bcd(h);
    if (tm->tm_hour >= 12)
      c[4] |= 0x80;
  }
  c[6] = (uint8_t)rtc_to_bcd(tm->tm_wday + 1);
  c[7] = (uint8_t)rtc_to_bcd(tm->tm_mday);
  c[8] = (uint8_t)rtc_to_bcd(tm->tm_mon + 1);
  int year = tm->tm_year + 1900;
  c[9] = (uint8_t)rtc_to_bcd(year % 100);
  c[0x32] = (uint8_t)rtc_to_bcd(year / 100);
}

static int64_t rtc_timegm(int year, int mon, int mday, int hour, int min, int sec) {
  // dias desde 1970 (algoritmo civil de Howard Hinnant)
  int y = year - (mon <= 2);
  int era = (y >= 0 ? y : y - 399) / 400;
  int yoe = y - era * 400;
  int doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + mday - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = (int64_t)era * 146097 + doe - 719468;
  return days * 86400 + hour * 3600 + min * 60 + sec;
}

static void rtc_set_time(void) {
  const uint8_t *c = g_rtc.cmos;
  int sec = rtc_from_bcd(c[0]), min = rtc_from_bcd(c[2]);
  int hour = rtc_from_bcd(c[4] & 0x7f);
  if (!(c[REG_B] & 0x02)) {
    hour %= 12;
    if (c[4] & 0x80)
      hour += 12;
  }
  int mday = rtc_from_bcd(c[7]), mon = rtc_from_bcd(c[8]);
  int year = rtc_from_bcd(c[9]) + rtc_from_bcd(c[0x32]) * 100;
  if (sec < 0) sec = 0;
  if (min < 0) min = 0;
  if (hour < 0) hour = 0;
  if (mday < 1) mday = 1;
  if (mon < 1 || mon > 12) mon = 1;
  g_rtc.base_rtc = rtc_timegm(year, mon, mday, hour, min, sec);
  g_rtc.last_update = now_ns();
}

static void rtc_update_time(void) {
  int64_t ns = rtc_guest_ns();
  time_t s = (time_t)(ns / 1000000000LL);
  struct tm tm;
  gmtime_r(&s, &tm);
  if (!(g_rtc.cmos[REG_B] & 0x80))
    rtc_set_cmos(&tm);
}

static int rtc_update_in_progress(void) {
  if (!rtc_running())
    return 0;
  int64_t ns = rtc_guest_ns() % 1000000000LL;
  return ns >= 1000000000LL - 244000;
}

static void rtc_irq_update(void) {
  int lvl = (g_rtc.cmos[REG_C] & 0x80) != 0;
  if (lvl != g_rtc.irq_level) {
    g_rtc.irq_level = lvl;
    irq_set(8, lvl);
  }
}

static uint32_t rtc_period_ticks(void) {
  if (!rtc_running())
    return 0;
  int code = g_rtc.cmos[REG_A] & 0x0f;
  if (!code)
    return 0;
  if (code <= 2)
    code += 7;
  return 1u << (code - 1);
}

static uint64_t rtc_ticks_to_ns(uint64_t t) { return t * 1000000000ULL / 32768; }

static void rtc_periodic_restart(void) {
  uint32_t p = rtc_period_ticks();
  g_rtc.period = p;
  g_rtc.backlog = 0;
  if (!p)
    return;
  // Alineado a la base de 32 kHz (como QEMU: el siguiente multiplo del periodo).
  uint64_t now = now_ns();
  uint64_t clk = (uint64_t)((__uint128_t)now * 32768 / 1000000000ULL);
  uint64_t next = (clk / p + 1) * p;
  g_rtc.next_periodic = rtc_ticks_to_ns(next) + 1;
}

static void cmos_init(uint64_t ram_bytes, int boot_cd) {
  memset(&g_rtc, 0, sizeof(g_rtc));
  uint8_t *c = g_rtc.cmos;
  c[REG_A] = 0x26;
  c[REG_B] = 0x02; // 24 h, BCD
  c[REG_D] = 0x80; // bateria buena
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  // Hora LOCAL (Windows espera el reloj CMOS en hora local, como "-rtc base=localtime").
  struct tm lt;
  time_t now_s = ts.tv_sec;
  long gmtoff = localtime_r(&now_s, &lt) ? lt.tm_gmtoff : 0;
  g_rtc.base_rtc = ts.tv_sec + gmtoff;
  g_rtc.offset = ts.tv_nsec;
  g_rtc.last_update = now_ns();
  g_rtc.last_sec = -1;
  rtc_update_time();
  // Datos de la BIOS (como pc_cmos_init de QEMU)
  uint64_t kb = ram_bytes >> 10;
  uint64_t ext = kb > 1024 ? kb - 1024 : 0;
  if (ext > 0xFFFF)
    ext = 0xFFFF;
  c[0x10] = 0x00; // sin disqueteras
  c[0x14] = 0x06; // equipo: VGA + FPU
  c[0x15] = 0x80; // memoria base: 640 KiB
  c[0x16] = 0x02;
  c[0x17] = c[0x30] = (uint8_t)ext;
  c[0x18] = c[0x31] = (uint8_t)(ext >> 8);
  if (ram_bytes > (16ULL << 20)) {
    uint64_t blocks = (ram_bytes - (16ULL << 20)) >> 16;
    if (blocks > 0xFFFF)
      blocks = 0xFFFF;
    c[0x34] = (uint8_t)blocks;
    c[0x35] = (uint8_t)(blocks >> 8);
  }
  c[0x5f] = 0; // CPU - 1
  // Orden de arranque de SeaBIOS (sin fw_cfg): nibbles 0x3D bajo, 0x3D alto,
  // 0x38 alto; 2 = disco, 3 = CD. Bit 0 de 0x38 = no comprobar firma de disquete.
  c[0x3D] = boot_cd ? 0x23 : 0x32;
  c[0x38] = 0x01;
  rtc_periodic_restart(); // A = 0x26: PF ya late a 1024 Hz desde el encendido
}

static void rtc_check_alarm_update(void) {
  if (!rtc_running() || (g_rtc.cmos[REG_A] & 0x60) == 0x60)
    return;
  int64_t sec = rtc_guest_ns() / 1000000000LL;
  if (g_rtc.last_sec < 0) {
    g_rtc.last_sec = sec;
    return;
  }
  if (sec == g_rtc.last_sec)
    return;
  g_rtc.last_sec = sec;
  rtc_update_time();
  uint8_t irqs = 0x10; // UF
  const uint8_t *c = g_rtc.cmos;
  int as = rtc_from_bcd(c[1]), am = rtc_from_bcd(c[3]), ah = rtc_from_bcd(c[5] & 0x7f);
  int cs = rtc_from_bcd(c[0]), cm = rtc_from_bcd(c[2]), ch = rtc_from_bcd(c[4] & 0x7f);
  if (!(c[REG_B] & 0x02)) {
    if (ah >= 0) { ah %= 12; if (c[5] & 0x80) ah += 12; }
    ch %= 12;
    if (c[4] & 0x80) ch += 12;
  }
  if ((as < 0 || as == cs) && (am < 0 || am == cm) && (ah < 0 || ah == ch))
    irqs |= 0x20; // AF
  uint8_t newi = irqs & ~g_rtc.cmos[REG_C];
  g_rtc.cmos[REG_C] |= irqs;
  if (newi & g_rtc.cmos[REG_B])
    g_rtc.cmos[REG_C] |= 0x80;
  rtc_irq_update();
}

// Llamada en cada vuelta del bucle. Devuelve el proximo instante de interes.
// Proximo instante de rtc_poll sin contar el tick periodico (lo genera el kernel
// cuando puede: avm_fast.rtc_vec).
static uint64_t g_rtc_next_nopf = ~0ULL;
static uint64_t rtc_poll(uint64_t now) {
  rtc_check_alarm_update();
  uint64_t next = ~0ULL;
  if ((g_rtc.cmos[REG_B] & 0x30) && rtc_running()) {
    int64_t ns = rtc_guest_ns() % 1000000000LL;
    next = now + (uint64_t)(1000000000LL - ns);
  }
  g_rtc_next_nopf = next;
  if (!g_rtc.period)
    return next;
  if (now >= g_rtc.next_periodic) {
    uint64_t pns = rtc_ticks_to_ns(g_rtc.period);
    if (!pns)
      pns = 1;
    uint64_t n = (now - g_rtc.next_periodic) / pns + 1;
    g_rtc.next_periodic += n * pns;
    g_rtc.cmos[REG_C] |= 0x40; // PF
    if (g_rtc.cmos[REG_B] & 0x40)
      g_rtc.cmos[REG_C] |= 0x80; // IRQF
    rtc_irq_update();
  }
  g_rtc_next_nopf = next;
  // Sin PIE el VMM no se despierta por PF: se calcula al leer C.
  if (!(g_rtc.cmos[REG_B] & 0x40))
    return next;
  return g_rtc.next_periodic < next ? g_rtc.next_periodic : next;
}

static uint8_t rtc_read(void) {
  uint8_t idx = g_rtc.index, ret;
  switch (idx) {
  case 0x37: // IBM PS/2: siglo
    g_rtc.index = idx = 0x32;
    /* fall through */
  case 0: case 2: case 4: case 6: case 7: case 8: case 9: case 0x32:
    if (rtc_running())
      rtc_update_time();
    return g_rtc.cmos[idx];
  case REG_A:
    ret = g_rtc.cmos[REG_A];
    if (rtc_update_in_progress())
      ret |= 0x80;
    return ret;
  case REG_C:
    rtc_poll(now_ns()); // PF/UF/AF al dia en el momento de la lectura
    ret = g_rtc.cmos[REG_C];
    g_rtc.cmos[REG_C] = 0;
    rtc_irq_update();
    return ret;
  default:
    return g_rtc.cmos[idx & 0x7f];
  }
}

static void rtc_write(uint8_t data) {
  uint8_t idx = g_rtc.index;
  uint8_t *c = g_rtc.cmos;
  switch (idx) {
  case 1: case 3: case 5:
    c[idx] = data;
    break;
  case 0x37:
    g_rtc.index = idx = 0x32;
    /* fall through */
  case 0: case 2: case 4: case 6: case 7: case 8: case 9: case 0x32:
    c[idx] = data;
    if (rtc_running())
      rtc_set_time();
    break;
  case REG_A: {
    int upd = ((c[REG_A] ^ data) & 0x7f) != 0; // tasa o divisor
    if ((data & 0x60) == 0x60) {
      if (rtc_running())
        rtc_update_time();
    } else if ((c[REG_A] & 0x60) == 0x60 && (data & 0x70) <= 0x20) {
      // al quitar el reset del divisor, la primera actualizacion llega medio segundo despues
      if (!(c[REG_B] & 0x80)) {
        g_rtc.offset = 500000000;
        rtc_set_time();
      }
    }
    c[REG_A] = data & 0x7f;
    if (upd)
      rtc_periodic_restart();
    break;
  }
  case REG_B: {
    int upd = ((c[REG_B] ^ data) & 0xC0) != 0; // PIE o SET
    if (data & 0x80) {
      if (rtc_running())
        rtc_update_time();
      data &= (uint8_t)~0x10;
    } else if ((c[REG_B] & 0x80) && (c[REG_A] & 0x70) <= 0x20) {
      g_rtc.offset = rtc_guest_ns() % 1000000000LL;
      rtc_set_time();
    }
    if (data & c[REG_C] & 0x70)
      c[REG_C] |= 0x80;
    else
      c[REG_C] &= (uint8_t)~0x80;
    c[REG_B] = data;
    rtc_irq_update();
    if (upd)
      rtc_periodic_restart();
    break;
  }
  case REG_C: case REG_D:
    break;
  default:
    c[idx & 0x7f] = data;
    break;
  }
}

static int rtc_port(uint16_t p) { return p == 0x70 || p == 0x71; }
static uint8_t rtc_in(uint16_t p) { return p == 0x70 ? 0xff : rtc_read(); }
static void rtc_out(uint16_t p, uint8_t v) {
  if (p == 0x70)
    g_rtc.index = v & 0x7f;
  else
    rtc_write(v);
}

#endif
