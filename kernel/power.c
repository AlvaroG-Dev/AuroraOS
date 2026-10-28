// kernel/power.c
//
// Politica de apagado/reinicio (Plan_Power.md, P1) y backends sin ACPI
// (0xCF9, 8042, puertos de emuladores, triple fault). El apagado ACPI (S5) y
// el reset por RESET_REG viven en acpi_pm.c.
#include "power.h"
#include "acpi_pm.h"
#include "block.h"
#include "cmdline.h"
#include "driver.h"
#include "io.h"
#include "klog.h"
#include "process.h"
#include "signal.h"
#include "spinlock.h"
#include "string.h"
#include "time.h"

#define MAX_NOTIFIERS 16
#define MAX_KILL_PIDS 256

static struct {
  power_notifier_fn fn;
  void *ctx;
  int prio;
} g_notifiers[MAX_NOTIFIERS];
static spinlock_t g_notifier_lock;
static volatile uint32_t g_power_busy;

int power_register_notifier(power_notifier_fn fn, void *ctx, int prio) {
  if (!fn)
    return -1;
  uint64_t fl = spin_lock_irqsave(&g_notifier_lock);
  for (int i = 0; i < MAX_NOTIFIERS; i++) {
    if (!g_notifiers[i].fn) {
      g_notifiers[i].fn = fn;
      g_notifiers[i].ctx = ctx;
      g_notifiers[i].prio = prio;
      spin_unlock_irqrestore(&g_notifier_lock, fl);
      return 0;
    }
  }
  spin_unlock_irqrestore(&g_notifier_lock, fl);
  return -1;
}

int power_unregister_notifier(power_notifier_fn fn, void *ctx) {
  uint64_t fl = spin_lock_irqsave(&g_notifier_lock);
  for (int i = 0; i < MAX_NOTIFIERS; i++) {
    if (g_notifiers[i].fn == fn && g_notifiers[i].ctx == ctx) {
      g_notifiers[i].fn = NULL;
      spin_unlock_irqrestore(&g_notifier_lock, fl);
      return 0;
    }
  }
  spin_unlock_irqrestore(&g_notifier_lock, fl);
  return -1;
}

static void call_notifiers(power_action_t action) {
  // Copia ordenada por prioridad; los callbacks corren sin el lock.
  struct {
    power_notifier_fn fn;
    void *ctx;
    int prio;
  } snap[MAX_NOTIFIERS];
  int n = 0;
  uint64_t fl = spin_lock_irqsave(&g_notifier_lock);
  for (int i = 0; i < MAX_NOTIFIERS; i++)
    if (g_notifiers[i].fn)
      snap[n++] = (typeof(snap[0])){g_notifiers[i].fn, g_notifiers[i].ctx,
                                    g_notifiers[i].prio};
  spin_unlock_irqrestore(&g_notifier_lock, fl);
  for (int i = 1; i < n; i++) { // insercion, n <= 16
    typeof(snap[0]) k = snap[i];
    int j = i - 1;
    while (j >= 0 && snap[j].prio < k.prio) {
      snap[j + 1] = snap[j];
      j--;
    }
    snap[j + 1] = k;
  }
  for (int i = 0; i < n; i++)
    snap[i].fn(action, snap[i].ctx);
}

// ---------------------------------------------------------------------------
// Utilidades de espera
// ---------------------------------------------------------------------------
// ~1 us por iteracion en hardware real (un acceso a 0x80 es un ciclo ISA).
// Sirve con las interrupciones desactivadas.
static void power_delay_us(uint32_t us) {
  while (us--)
    inb(0x80);
}

static int irqs_enabled(void) {
  uint64_t f;
  __asm__ volatile("pushfq; pop %0" : "=r"(f));
  return (f >> 9) & 1;
}

// Espera hasta `ms` ms de reloj si hay interrupciones; si no, por espera
// activa.
static void power_wait_ms(uint32_t ms) {
  if (!irqs_enabled()) {
    power_delay_us(ms * 1000);
    return;
  }
  uint64_t end = tick_count + (uint64_t)ms * KERNEL_HZ / 1000 + 1;
  while (tick_count < end)
    __asm__ volatile("hlt");
}

// ---------------------------------------------------------------------------
// Procesos
// ---------------------------------------------------------------------------
typedef struct {
  uint32_t pids[MAX_KILL_PIDS];
  int n;
  uint32_t self;
} pidlist_t;

static int collect_cb(process_t *p, void *arg) {
  pidlist_t *l = arg;
  if (p->pid == 0 || p->pid == l->self)
    return 0;
  if (l->n < MAX_KILL_PIDS)
    l->pids[l->n++] = p->pid;
  return l->n >= MAX_KILL_PIDS;
}

static int signal_all(int sig) {
  pidlist_t l = {.n = 0, .self = 0};
  process_t *cur = process_current();
  if (cur)
    l.self = cur->pid;
  process_for_each(collect_cb, &l);
  for (int i = 0; i < l.n; i++)
    process_kill(l.pids[i], sig);
  return l.n;
}

// ---------------------------------------------------------------------------
// Backends
// ---------------------------------------------------------------------------
static int running_in_vm(void) {
  uint32_t a = 1, b = 0, c = 0, d = 0;
  __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
  return (c >> 31) & 1;
}

static void restart_cf9(void) {
  LOG_INFO("[POWER] reinicio: puerto 0xCF9");
  outb(0xCF9, 0x02); // SYS_RST suave, flanco 0->1 en RST_CPU abajo
  power_delay_us(50000);
  outb(0xCF9, 0x06); // RST_CPU | SYS_RST
  power_delay_us(100000);
  outb(0xCF9, 0x0E); // FULL_RST | RST_CPU | SYS_RST
  power_delay_us(100000);
}

static void restart_kbd(void) {
  LOG_INFO("[POWER] reinicio: controlador de teclado (0x64 <- 0xFE)");
  for (int i = 0; i < 3; i++) {
    for (int t = 0; t < 0x10000 && (inb(0x64) & 0x02); t++)
      inb(0x80);
    outb(0x64, 0xFE);
    power_delay_us(50000);
  }
}

static void restart_triple_fault(void) {
  LOG_INFO("[POWER] reinicio: triple fault");
  struct {
    uint16_t limit;
    uint64_t base;
  } __attribute__((packed)) idt = {0, 0};
  __asm__ volatile("lidt %0; int3" : : "m"(idt));
}

void power_backend_restart(void) {
  char opt[16] = "";
  cmdline_get("reboot", opt, sizeof(opt));

  // Backend forzado por cmdline: va primero, luego la cadena completa.
  if (!strcmp(opt, "pci"))
    restart_cf9();
  else if (!strcmp(opt, "kbd"))
    restart_kbd();
  else if (!strcmp(opt, "triple"))
    restart_triple_fault();

  acpi_pm_restart(); // RESET_REG si el FADT lo declara; vuelve si falla
  restart_cf9();
  restart_kbd();
  restart_triple_fault();
}

void power_backend_poweroff(void) {
  if (acpi_pm_poweroff() != 0)
    LOG_WARN("[POWER] S5 ACPI no disponible o sin efecto");
  if (running_in_vm()) {
    LOG_INFO("[POWER] apagado: puertos de emuladores");
    outw(0x604, 0x2000);  // QEMU (PIIX4/ICH9 ACPI), Bochs
    outw(0xB004, 0x2000); // Bochs / QEMU antiguo
    outw(0x4004, 0x3400); // VirtualBox
    power_delay_us(100000);
  }
}

// ---------------------------------------------------------------------------
// Peticion de apagado
// ---------------------------------------------------------------------------
static void halt_forever(void) {
  for (;;)
    __asm__ volatile("cli; hlt");
}

__attribute__((noreturn)) void power_request(power_action_t action,
                                             const char *reason) {
  const char *name = action == POWER_ACTION_OFF       ? "apagado"
                     : action == POWER_ACTION_RESTART ? "reinicio"
                                                      : "parada";
  if (__sync_lock_test_and_set(&g_power_busy, 1)) {
    // Ya hay un apagado en curso en otra CPU/hilo.
    for (;;)
      __asm__ volatile("hlt");
  }
  LOG_INFO("[POWER] %s solicitado (%s)", name, reason ? reason : "-");

  call_notifiers(action);

  // Procesos de usuario: SIGTERM, margen corto, SIGKILL.
  if (signal_all(SIGTERM) > 0) {
    power_wait_ms(500);
    signal_all(SIGKILL);
    power_wait_ms(100);
  }

  // FAT32 escribe de forma sincrona, pero la cache de escritura del disco
  // no: vaciarla en todos los dispositivos.
  for (int i = 0;; i++) {
    block_device_t *b = blk_get_by_index(i);
    if (!b)
      break;
    int rc = bdev_flush(b);
    if (rc)
      LOG_WARN("[POWER] bdev_flush(%s) = %d", b->name, rc);
  }

  drivers_shutdown_all();

  __asm__ volatile("cli");
  LOG_INFO("[POWER] ejecutando %s", name);

  if (action == POWER_ACTION_OFF)
    power_backend_poweroff();
  else if (action == POWER_ACTION_RESTART)
    power_backend_restart();
  else
    LOG_INFO("[POWER] sistema parado");

  if (action != POWER_ACTION_HALT)
    LOG_ERR("[POWER] ningun backend de %s funciono; parando", name);
  halt_forever();
  __builtin_unreachable();
}
