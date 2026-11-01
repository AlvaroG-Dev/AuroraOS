// kernel/acpi_button.c
//
// Boton de encendido ACPI "fijo" (PM1_STS.PWRBTN_STS, Plan_Power.md P4).
//
// Se sondea PM1_STS desde un kthread en lugar de enrutar la IRQ del SCI: el
// SCI es una linea compartida y de nivel que tambien dispara con GPEs que aun
// no atendemos, y sin manejador completo provocaria una tormenta. El sondeo
// cada 200 ms basta para un boton. Los botones por metodo de control (EC,
// PNP0C0C) quedan para cuando haya GPE/EC.
//
// acpi.button=0 en la cmdline lo desactiva.
#include "acpi_button.h"
#include "acpi.h"
#include "acpi_aml.h"
#include "acpi_pm.h"
#include "cmdline.h"
#include "io.h"
#include "klog.h"
#include "kthread.h"
#include "paging.h"
#include "power.h"
#include "sched.h"
#include "string.h"
#include "wait.h"

#define PM1_PWRBTN_STS (1u << 8)
#define PM1_SLPBTN_STS (1u << 9)
#define PM1_PWRBTN_EN (1u << 8)

static void button_report(void);
static uint16_t g_sts_port;
static uint16_t g_en_port;
static int g_pressed;

static bool never_true(void *arg) {
  (void)arg;
  return false;
}

static int button_thread(void *arg) {
  (void)arg;
  uint64_t report_at = sched_get_ticks() + 30000;
  int reported = 0;
  for (;;) {
    if (!reported && sched_get_ticks() >= report_at) {
      // El log a /data empieza ~8 s despues del arranque: repetir el
      // informe para que quede en aurora.log.
      button_report();
      reported = 1;
    }
    wait_queue_t wq;
    wait_queue_init(&wq);
    wait_event_timeout(&wq, never_true, NULL, 200);

    if (!g_sts_port) {
      if (reported)
        return 0; // solo informe, sin boton fijo que sondear
      continue;
    }
    uint16_t sts = inw(g_sts_port);
    if (sts & PM1_SLPBTN_STS)
      outw(g_sts_port, PM1_SLPBTN_STS); // sin accion por ahora
    if (sts & PM1_PWRBTN_STS) {
      outw(g_sts_port, PM1_PWRBTN_STS);
      if (g_pressed)
        continue;
      g_pressed = 1;
      LOG_INFO("[ACPI] boton de encendido");
      power_request(POWER_ACTION_OFF, "boton de encendido");
    }
  }
  return 0;
}

static void button_report(void) {
  const acpi_fadt_t *f = acpi_get_fadt();
  if (f && f->valid) {
    LOG_INFO("[ACPI] FADT flags=0x%x: boton %s, boton suspension %s, "
             "HW_REDUCED=%d, SCI_INT=%u",
             f->flags,
             (f->flags & (1u << 4)) ? "por metodo de control (EC/GPE)" : "fijo",
             (f->flags & (1u << 5)) ? "por metodo de control" : "fijo",
             !!(f->flags & ACPI_FADT_HW_REDUCED), f->sci_int);
    LOG_INFO("[ACPI] GPE0_BLK=0x%x GPE1_BLK=0x%x PM1a_EVT=0x%x", f->gpe0_blk,
             f->gpe1_blk, f->pm1a_evt_blk);
  }
  aml_diag_power();
}


// ---------------------------------------------------------------------------
// Boton de encendido por controlador embebido (portatiles como la Portege).
//
// El EC avisa de sus eventos con SCI_EVT (bit 5 del estado, puerto de
// comandos). Se sondea ese bit y, si esta activo, se envia QR_EC (0x84) y se
// lee el numero de consulta. Si la consulta corresponde a un _Qxx cuyo cuerpo
// hace Notify(PNP0C0C, 0x80) (analisis estatico, ver aml_find_ec) se trata
// como pulsacion. `acpi.ecbutton=0xNN` fuerza el numero de consulta del boton
// si el analisis falla. Todas las consultas se registran en el log.
// ---------------------------------------------------------------------------
#define EC_OBF 0x01
#define EC_IBF 0x02
#define EC_SCI 0x20
#define EC_QR 0x84

static aml_ec_info_t g_ec;
static int g_ec_force_q = -1;

static int ec_wait(uint16_t cmd_port, uint8_t mask, int want_set) {
  for (int i = 0; i < 50000; i++) {
    uint8_t st = inb(cmd_port);
    if (!!(st & mask) == want_set)
      return 0;
    inb(0x80); // ~1 us
  }
  return -1;
}

// Devuelve el numero de consulta (0..255), 0 si no habia, -1 si hay error.
static int ec_query(void) {
  if (ec_wait(g_ec.cmd_port, EC_IBF, 0))
    return -1;
  outb(g_ec.cmd_port, EC_QR);
  if (ec_wait(g_ec.cmd_port, EC_OBF, 1))
    return -1;
  return inb(g_ec.data_port);
}

static int ec_is_button_q(int q) {
  if (g_ec_force_q >= 0)
    return q == g_ec_force_q;
  return (g_ec.power_q[q >> 3] >> (q & 7)) & 1;
}

static int ec_button_thread(void *arg) {
  (void)arg;
  int logged = 0;
  for (;;) {
    wait_queue_t wq;
    wait_queue_init(&wq);
    wait_event_timeout(&wq, never_true, NULL, 200);
    uint8_t st = inb(g_ec.cmd_port);
    if (st == 0xFF)
      continue; // sin EC / bus flotante
    int budget = 8; // vaciar la cola de eventos sin quedarse en bucle
    while ((st & EC_SCI) && budget--) {
      int q = ec_query();
      if (q < 0) {
        if (logged++ < 5)
          LOG_WARN("[ACPI] EC: fallo al leer la consulta (estado=0x%x)", st);
        break;
      }
      if (q == 0)
        break;
      if (logged++ < 40)
        LOG_INFO("[ACPI] EC: consulta _Q%02X%s", q,
                 ec_is_button_q(q) ? " (boton de encendido)" : "");
      if (ec_is_button_q(q) && !g_pressed) {
        g_pressed = 1;
        LOG_INFO("[ACPI] boton de encendido (EC)");
        power_request(POWER_ACTION_OFF, "boton de encendido");
      }
      st = inb(g_ec.cmd_port);
    }
  }
  return 0;
}

static int ec_button_init(void) {
  char v[16];
  if (cmdline_get("acpi.ec", v, sizeof(v)) && !strcmp(v, "0")) {
    LOG_INFO("[ACPI] boton por EC desactivado por acpi.ec=0");
    return -1;
  }
  if (aml_find_ec(&g_ec) != 0 || !g_ec.found) {
    LOG_INFO("[ACPI] boton por EC: no hay dispositivo PNP0C09");
    return -1;
  }
  LOG_INFO("[ACPI] EC: datos=0x%x comandos=0x%x (%s), %d metodos _Qxx, "
           "%d notifican al boton",
           g_ec.data_port, g_ec.cmd_port, g_ec.crs_ok ? "_CRS" : "por defecto",
           g_ec.nq, g_ec.power_q_count);
  if (cmdline_get("acpi.ecbutton", v, sizeof(v))) {
    unsigned x = 0;
    const char *p = v;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
      p += 2;
    for (; *p; p++) {
      int d = (*p >= '0' && *p <= '9')   ? *p - '0'
              : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10
              : (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10
                                         : -1;
      if (d < 0)
        break;
      x = x * 16 + (unsigned)d;
    }
    g_ec_force_q = (int)(x & 0xFF);
    LOG_INFO("[ACPI] EC: consulta del boton forzada a _Q%02X", g_ec_force_q);
  } else if (!g_ec.power_q_count) {
    LOG_WARN("[ACPI] EC: ningun _Qxx notifica al boton; pulsa el boton y "
             "mira el log (acpi.ecbutton=0xNN lo fuerza)");
  }
  if (inb(g_ec.cmd_port) == 0xFF) {
    LOG_WARN("[ACPI] EC: el puerto de comandos 0x%x lee 0xFF; sin EC",
             g_ec.cmd_port);
    return -1;
  }
  if (!kthread_run(ec_button_thread, NULL, "acpi-ec-button"))
    return -1;
  return 0;
}

int acpi_button_init(void) {
  char v[8];
  if (cmdline_get("acpi.button", v, sizeof(v)) && !strcmp(v, "0")) {
    LOG_INFO("[ACPI] boton de encendido desactivado por acpi.button=0");
    return -1;
  }
  const acpi_fadt_t *f = acpi_get_fadt();
  button_report();
  if (!f || !f->valid || !f->pm1a_evt_blk || (f->flags & ACPI_FADT_HW_REDUCED))
    return -1;
  // Flag FIXED_PWR_BUTTON (bit 4): si esta a 1 el boton NO es fijo, lo
  // gestiona un dispositivo del espacio de nombres (EC).
  if (f->flags & (1u << 4)) {
    LOG_INFO("[ACPI] boton de encendido por metodo de control (EC)");
    kthread_run(button_thread, NULL, "acpi-report"); // informe diferido
    // No se llama a acpi_pm_enable() aqui: activar el modo ACPI en el
    // arranque (SMI_CMD) cambia el comportamiento del firmware (SCI, USB
    // legacy) y no hace falta para sondear el EC.
    return ec_button_init();
  }
  if (acpi_pm_enable() != 0)
    return -1;

  // PM1a_STS en pm1a_evt_blk, PM1a_EN a mitad del bloque (len/2).
  g_sts_port = (uint16_t)f->pm1a_evt_blk;
  uint16_t half = f->pm1_evt_len ? f->pm1_evt_len / 2 : 2;
  g_en_port = (uint16_t)(f->pm1a_evt_blk + half);

  outw(g_sts_port, PM1_PWRBTN_STS | PM1_SLPBTN_STS); // limpiar pendientes
  outw(g_en_port, inw(g_en_port) | PM1_PWRBTN_EN);

  if (!kthread_run(button_thread, NULL, "acpi-button"))
    return -1;
  LOG_INFO("[ACPI] boton de encendido fijo (PM1_STS=0x%x, PM1_EN=0x%x)",
           g_sts_port, g_en_port);
  return 0;
}
