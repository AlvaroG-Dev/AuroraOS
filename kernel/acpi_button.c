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

static uint16_t g_sts_port;
static uint16_t g_en_port;
static int g_pressed;

static bool never_true(void *arg) {
  (void)arg;
  return false;
}

static int button_thread(void *arg) {
  (void)arg;
  for (;;) {
    wait_queue_t wq;
    wait_queue_init(&wq);
    wait_event_timeout(&wq, never_true, NULL, 200);

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

int acpi_button_init(void) {
  char v[8];
  if (cmdline_get("acpi.button", v, sizeof(v)) && !strcmp(v, "0")) {
    LOG_INFO("[ACPI] boton de encendido desactivado por acpi.button=0");
    return -1;
  }
  const acpi_fadt_t *f = acpi_get_fadt();
  if (!f || !f->valid || !f->pm1a_evt_blk || (f->flags & ACPI_FADT_HW_REDUCED))
    return -1;
  // Flag FIXED_PWR_BUTTON (bit 4): si esta a 1 el boton NO es fijo, lo
  // gestiona un dispositivo del espacio de nombres (EC).
  if (f->flags & (1u << 4)) {
    LOG_INFO("[ACPI] boton de encendido por metodo de control: sin soporte aun");
    return -1;
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
