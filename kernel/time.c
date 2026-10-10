// kernel/time.c
#include "time.h"
#include "apic.h"
#include "cpu.h"
#include "klog.h"
#include "panic.h"
#include "sched.h"

// ---------------------------------------------------------------------------
// tick_count: contador global de ticks. Lo incrementa el handler del
// timer (PIT o LAPIC timer). Es la base del scheduler preemptivo.
// ---------------------------------------------------------------------------
volatile uint64_t tick_count = 0;
volatile uint64_t lapic_ticks_per_cpu[MAX_CPUS] = {0};

// ---------------------------------------------------------------------------
// PIT (Programmable Interval Timer)
// ---------------------------------------------------------------------------
#define PIT_FREQ 1193182

static void pit_init_impl(void) {
  uint16_t divisor = PIT_FREQ / KERNEL_HZ;
  __asm__ volatile("outb %0, $0x43" : : "a"((uint8_t)0x36));
  __asm__ volatile("outb %0, $0x40" : : "a"((uint8_t)(divisor & 0xFF)));
  __asm__ volatile("outb %0, $0x40" : : "a"((uint8_t)((divisor >> 8) & 0xFF)));
  LOG_INFO("[PIT] Configurado a %u Hz", KERNEL_HZ);
}

void pit_init(void) { pit_init_impl(); }

// ---------------------------------------------------------------------------
// Tick común. Lo llama el handler de IRQ (PIT o LAPIC timer).
// ---------------------------------------------------------------------------
extern void compositor_notify_clock_tick(void);

extern void sched_wake_expired(void);

extern void ahci_poll_ports(void);

// ---------------------------------------------------------------------------
// Diagnóstico del bug "python3 tarda 2s con user=0.005s"
//
// Estado por CPU para medir el gap entre ticks consecutivos del LAPIC.
// Si un CPU pasa >2 ms con IF=0 (spinlock), el LAPIC descarta los ticks
// intermedios y el siguiente tick llega con un delta grande. Ese delta
// nos dice exactamente cuándo y cuánto tiempo estuvo el CPU "sordo".
//
// g_pf_per_pid lo expone pf.c. Lo imprimimos en cada [TICKS] para
// correlacionar los gaps con picos de page faults del proceso python3.
// ---------------------------------------------------------------------------
extern volatile uint64_t g_pf_per_pid[128];

static volatile uint64_t g_lapic_last_tsc[MAX_CPUS] = {0};
static volatile uint64_t g_tick_gap_count[MAX_CPUS] = {0};

void time_tick(void) {
  int cpu = smp_processor_id();

  // [DIAG] Detectar gaps >2 ms entre ticks consecutivos.
  {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t now_tsc = ((uint64_t)hi << 32) | lo;
    uint64_t prev = g_lapic_last_tsc[cpu];
    if (prev != 0) {
      uint64_t tsc_freq = klog_get_tsc_freq();
      uint64_t delta_us =
          tsc_freq ? ((now_tsc - prev) * 1000000ULL / tsc_freq) : 0;
    }
    g_lapic_last_tsc[cpu] = now_tsc;
    lapic_ticks_per_cpu[cpu]++;
  }

  // [FIX] El tick "global" (tick_count, wake_expired, ahci_poll) solo
  // desde CPU 0. El resto solo actualiza su contador y hace sched_tick.
  if (cpu == 0) {
    tick_count++;
    if (tick_count % KERNEL_HZ == 0) {
      compositor_notify_clock_tick();
    }
    // [FIX] sched_wake_expired a 125 Hz en vez de 1 kHz.
    // Sin esto, cada tick hace un kmalloc+kfree con sched_lock tomado,
    // y el resto de CPUs pierden tiempo esperando heap_lock. Ver el
    // fix de sched_wake_expired en sched.c.
    if ((tick_count & 7) == 0) {
      sched_wake_expired();
    }
    ahci_poll_ports();
  }

  // [DIAG] Dump periódico cada 5 s. Solo desde CPU 0 para no spamear
  // el log (el tick_count es global y crece una vez por ms).
  if (cpu == 0 && (tick_count % (5 * KERNEL_HZ)) == 0) {
    LOG_INFO("[TICKS] cpu0=%lu cpu1=%lu cpu2=%lu cpu3=%lu | "
             "gaps0=%lu gaps1=%lu gaps2=%lu gaps3=%lu",
             lapic_ticks_per_cpu[0], lapic_ticks_per_cpu[1],
             lapic_ticks_per_cpu[2], lapic_ticks_per_cpu[3],
             g_tick_gap_count[0], g_tick_gap_count[1], g_tick_gap_count[2],
             g_tick_gap_count[3]);
  }

  sched_tick();
}
// ---------------------------------------------------------------------------
// LAPIC timer
// ---------------------------------------------------------------------------

// Vector de IDT para el LAPIC timer. Debe estar en el rango 48-255.
#define LAPIC_TIMER_VECTOR 48

// Ticks por milisegundo del LAPIC timer, medidos durante la calibración.
static uint32_t g_lapic_ticks_per_ms = 0;

// Calibra el LAPIC timer contra el TSC. Asume que el TSC ya está
// calibrado (klog_calibrate_tsc).
//
// Estrategia:
//   1. Configurar el LAPIC timer en modo oneshot con Initial Count =
//   0xFFFFFFFF.
//   2. Leer el TSC actual.
//   3. Esperar a que pasen ~10 ms según el TSC.
//   4. Leer LAPIC_TIMER_CURRENT. Los ticks transcurridos son
//      (0xFFFFFFFF - current).
//   5. ticks_per_ms = ticks_transcurridos / 10.
//
// En QEMU, el LAPIC timer cuenta a ~100 MHz (depende del CPU emulado).
// Con 10 ms, contaríamos ~1 millón de ticks. Cabe en 32 bits.
static void lapic_timer_calibrate(void) {
  uint64_t tsc_freq = klog_get_tsc_freq();
  if (tsc_freq == 0) {
    LOG_ERR(
        "[LAPIC-TIMER] TSC no calibrado, no se puede calibrar el LAPIC timer");
    return;
  }

  // 10 ms en ciclos del TSC.
  uint64_t tsc_cycles_10ms = (tsc_freq / 1000) * 10;

  // Configurar el LAPIC timer en oneshot, divisor 1.
  lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_1);
  // Vector no importa en oneshot (no disparará), pero debe ser != 0.
  lapic_write(LAPIC_REG_LVT_TIMER,
              LAPIC_TIMER_MODE_ONESHOT | LAPIC_TIMER_VECTOR);
  lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFF);

  // Leer TSC inicio.
  uint32_t tsc_lo, tsc_hi;
  __asm__ volatile("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
  uint64_t tsc_start = ((uint64_t)tsc_hi << 32) | tsc_lo;
  uint64_t tsc_target = tsc_start + tsc_cycles_10ms;

  // Esperar a que el TSC pase 10 ms. Se hace con hlt para no consumir CPU.
  // Durante este tiempo, el LAPIC timer está corriendo pero no dispara
  // (modo oneshot con initial = 0xFFFFFFFF y nadie lo lee).
  // Nota: la IRQ 0 del PIT todavía puede estar llegando, pero como no hay
  // handler de timer activo (aún no se ha llamado a irq_install_handler
  // para la IRQ 0 en esta fase), el tick se ignora.
  while (1) {
    __asm__ volatile("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
    uint64_t now = ((uint64_t)tsc_hi << 32) | tsc_lo;
    if (now >= tsc_target)
      break;
    __asm__ volatile("pause");
  }

  // Leer el contador actual del LAPIC timer.
  uint32_t current = lapic_read(LAPIC_REG_TIMER_CURRENT);
  uint32_t elapsed = 0xFFFFFFFF - current;

  // Detener el timer (initial = 0).
  lapic_write(LAPIC_REG_TIMER_INIT, 0);

  if (elapsed == 0) {
    LOG_ERR("[LAPIC-TIMER] Calibración falló: 0 ticks medidos");
    return;
  }

  g_lapic_ticks_per_ms = elapsed / 10;
  LOG_INFO("[LAPIC-TIMER] Calibrado: %u ticks/ms (LAPIC timer)",
           g_lapic_ticks_per_ms);
}

void lapic_timer_init(void) {
  // 1. Calibrar.
  lapic_timer_calibrate();
  if (g_lapic_ticks_per_ms == 0) {
    LOG_ERR("[LAPIC-TIMER] Sin calibración, abortando init");
    return;
  }

  // 2. Configurar el LAPIC timer en modo periódico a KERNEL_HZ.
  //    Periodo = 1000 / KERNEL_HZ ms. Con KERNEL_HZ = 1000, es 1 ms.
  uint32_t init_count = g_lapic_ticks_per_ms * (1000 / KERNEL_HZ);

  lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_1);
  lapic_write(LAPIC_REG_LVT_TIMER,
              LAPIC_TIMER_MODE_PERIODIC | LAPIC_TIMER_VECTOR);
  lapic_write(LAPIC_REG_TIMER_INIT, init_count);

  LOG_INFO("[LAPIC-TIMER] Modo periódico: %u ticks (%u Hz)", init_count,
           KERNEL_HZ);

  // 3. Enmascarar la IRQ 0 del IOAPIC (el PIT deja de entregar ticks).
  extern void ioapic_mask_irq(uint8_t irq, int masked);
  ioapic_mask_irq(0, 1);
  LOG_INFO("[LAPIC-TIMER] IRQ 0 enmascarada en IOAPIC (PIT desactivado)");
}

// Handler del LAPIC timer. Llamado desde irq_handler cuando llega el
// vector 48. Envuelve time_tick() con logs de debug si hiciera falta.
void lapic_timer_handler(void) { time_tick(); }
void lapic_timer_init_ap(void) {
  if (g_lapic_ticks_per_ms == 0)
    return;

  uint32_t init_count = g_lapic_ticks_per_ms * (1000 / KERNEL_HZ);

  lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_1);
  lapic_write(LAPIC_REG_LVT_TIMER,
              LAPIC_TIMER_MODE_PERIODIC | LAPIC_TIMER_VECTOR);
  lapic_write(LAPIC_REG_TIMER_INIT, init_count);
}
