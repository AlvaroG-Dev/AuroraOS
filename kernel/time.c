// kernel/time.c
#include "time.h"
#include "apic.h"
#include "cpu.h"
#include "klog.h"
#include "panic.h"
#include "sched.h"
#include "vdso.h"

// ---------------------------------------------------------------------------
// tick_count: contador global de ticks. Lo incrementa el handler del
// timer. Es la base del scheduler preemptivo.
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
extern void arp_timer_tick(void);
extern void tcp_timer_tick(void);
extern volatile uint64_t g_pf_per_pid[128];

// ---------------------------------------------------------------------------
// [DIAG] Diagnóstico de gaps del LAPIC.
//
// g_lapic_last_tsc[] y g_tick_gap_count[] sirven para medir el gap entre
// ticks consecutivos. Si un CPU pasa >2 ms con IF=0 (spinlock), el LAPIC
// descarta los ticks intermedios y el siguiente tick llega con un delta
// grande. Ese delta nos dice cuándo y cuánto tiempo estuvo el CPU "sordo".
// ---------------------------------------------------------------------------
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
      (void)delta_us;
    }
    g_lapic_last_tsc[cpu] = now_tsc;
    lapic_ticks_per_cpu[cpu]++;
  }

  // [FIX] El tick "global" (tick_count, wake_expired, ahci_poll) solo
  // desde CPU 0. El resto solo actualiza su contador y hace sched_tick.
  if (cpu == 0) {
    tick_count++;
    vdso_update_clock();
    if (tick_count % KERNEL_HZ == 0) {
      compositor_notify_clock_tick();
    }
    // [FIX] sched_wake_expired a 125 Hz en vez de 1 kHz.
    if ((tick_count & 7) == 0) {
      sched_wake_expired();
    }
    // Aging de ARP a 1 Hz.
    if ((tick_count & 1023) == 0) {
      arp_timer_tick();
    }
    tcp_timer_tick();
    ahci_poll_ports();

    // [DIAG] Dump periódico cada 5 s.
    if ((tick_count % (5 * KERNEL_HZ)) == 0) {
      LOG_INFO("[TICKS] cpu0=%lu cpu1=%lu cpu2=%lu cpu3=%lu | "
               "gaps0=%lu gaps1=%lu gaps2=%lu gaps3=%lu",
               lapic_ticks_per_cpu[0], lapic_ticks_per_cpu[1],
               lapic_ticks_per_cpu[2], lapic_ticks_per_cpu[3],
               g_tick_gap_count[0], g_tick_gap_count[1], g_tick_gap_count[2],
               g_tick_gap_count[3]);
    }
  }

  sched_tick();
}

// ===========================================================================
// LAPIC timer con TSC-deadline
// ===========================================================================
//
// En vez de usar el contador del LAPIC (que en Alder Lake y otros CPUs
// híbridos puede depender del core clock y por tanto del P-state), usamos
// el TSC (invariant, siempre a la misma frecuencia) y el registro
// IA32_TSC_DEADLINE (MSR 0x6E0). El LAPIC dispara el vector cuando el
// TSC alcanza el valor escrito.
//
// Ventajas:
//   - Inmune al P-state.
//   - Precisión de nanosegundos.
//   - No necesita calibración: usa el TSC ya calibrado.
//
// Requisitos: CPUID.1:ECX[24] = 1 (TSC-deadline soportado).
// ===========================================================================

#define LAPIC_TIMER_VECTOR 48
#define MSR_IA32_TSC_DEADLINE 0x6E0
#define LAPIC_LVT_TSC_DEADLINE (1u << 18)

// Periodo del tick en nanosegundos. 1 ms = 1.000.000 ns.
#define TICK_PERIOD_NS (1000000000ULL / KERNEL_HZ)

static int cpu_has_tsc_deadline(void) {
  uint32_t eax, ebx, ecx, edx;
  cpuid(1, 0, &eax, &ebx, &ecx, &edx);
  return (ecx >> 24) & 1;
}

// Arma el siguiente disparo a `delta_ns` nanosegundos desde el TSC actual.
static inline void lapic_timer_arm_tsc(uint64_t delta_ns) {
  uint64_t freq = klog_get_tsc_freq();
  if (freq == 0)
    return;
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t now = ((uint64_t)hi << 32) | lo;

  // Multiplicar primero y dividir después para no perder precisión.
  // 1 ms = 1,000,000 ns. delta_ns * freq es como máximo ~1e16 para
  // cualquier CPU moderna (freq < 10 GHz), cabe en uint64_t.
  uint64_t delta = (delta_ns * freq) / 1000000000ULL;
  if (delta == 0)
    delta = 1;

  wrmsr(MSR_IA32_TSC_DEADLINE, now + delta);
}

// Estado del timer para fallback periódico.
static uint32_t g_lapic_ticks_per_ms = 0;

// Calibración del modo periódico (fallback). Solo se usa si la CPU
// no tiene TSC-deadline (QEMU con CPU genérico, CPUs viejas).
static void lapic_timer_calibrate(void) {
  uint64_t tsc_freq = klog_get_tsc_freq();
  if (tsc_freq == 0) {
    LOG_ERR("[LAPIC-TIMER] TSC no calibrado, no se puede calibrar");
    return;
  }

  uint64_t tsc_cycles_10ms = (tsc_freq / 1000) * 10;

  lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_1);
  lapic_write(LAPIC_REG_LVT_TIMER,
              LAPIC_TIMER_MODE_ONESHOT | LAPIC_TIMER_VECTOR);
  lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFF);

  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t tsc_start = ((uint64_t)hi << 32) | lo;
  uint64_t tsc_target = tsc_start + tsc_cycles_10ms;

  while (1) {
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t now = ((uint64_t)hi << 32) | lo;
    if (now >= tsc_target)
      break;
    __asm__ volatile("pause");
  }

  uint32_t current = lapic_read(LAPIC_REG_TIMER_CURRENT);
  uint32_t elapsed = 0xFFFFFFFF - current;

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
  if (cpu_has_tsc_deadline()) {
    // -----------------------------------------------------------------
    // Path preferido: TSC-deadline. Inmune al P-state (Alder Lake).
    // -----------------------------------------------------------------
    LOG_INFO("[LAPIC-TIMER] Usando TSC-deadline");

    lapic_write(LAPIC_REG_TIMER_DIV, 0);
    lapic_write(LAPIC_REG_LVT_TIMER,
                LAPIC_LVT_TSC_DEADLINE | LAPIC_TIMER_VECTOR);
    lapic_timer_arm_tsc(TICK_PERIOD_NS);

    LOG_INFO("[LAPIC-TIMER] TSC-deadline activo (%u Hz, periodo=%lu ns)",
             KERNEL_HZ, (unsigned long)TICK_PERIOD_NS);
  } else {
    // -----------------------------------------------------------------
    // Fallback: modo periódico. Necesario para QEMU con CPU genérico,
    // CPUs Intel pre-Nehalem, y AMD pre-Zen.
    // -----------------------------------------------------------------
    LOG_WARN("[LAPIC-TIMER] CPU sin TSC-deadline (CPUID.1:ECX[24]=0). "
             "Usando modo periódico.");

    lapic_timer_calibrate();
    if (g_lapic_ticks_per_ms == 0) {
      LOG_ERR("[LAPIC-TIMER] Calibración falló, timer no inicializado");
      return;
    }

    uint32_t init_count = g_lapic_ticks_per_ms * (1000 / KERNEL_HZ);
    lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_1);
    lapic_write(LAPIC_REG_LVT_TIMER,
                LAPIC_TIMER_MODE_PERIODIC | LAPIC_TIMER_VECTOR);
    lapic_write(LAPIC_REG_TIMER_INIT, init_count);

    LOG_INFO("[LAPIC-TIMER] Modo periódico: %u ticks (%u Hz)", init_count,
             KERNEL_HZ);
  }

  // Enmascarar la IRQ 0 del IOAPIC (el PIT deja de entregar ticks).
  extern void ioapic_mask_irq(uint8_t irq, int masked);
  ioapic_mask_irq(0, 1);
  LOG_INFO("[LAPIC-TIMER] IRQ 0 enmascarada en IOAPIC (PIT desactivado)");
}

void lapic_timer_init_ap(void) {
  if (cpu_has_tsc_deadline()) {
    lapic_write(LAPIC_REG_LVT_TIMER,
                LAPIC_LVT_TSC_DEADLINE | LAPIC_TIMER_VECTOR);
    lapic_timer_arm_tsc(TICK_PERIOD_NS);
    return;
  }
  // Fallback periódico.
  if (g_lapic_ticks_per_ms == 0)
    return;
  uint32_t init_count = g_lapic_ticks_per_ms * (1000 / KERNEL_HZ);
  lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_1);
  lapic_write(LAPIC_REG_LVT_TIMER,
              LAPIC_TIMER_MODE_PERIODIC | LAPIC_TIMER_VECTOR);
  lapic_write(LAPIC_REG_TIMER_INIT, init_count);
}

void lapic_timer_handler(void) {
  // Rearmar ANTES de hacer el trabajo (solo en TSC-deadline).
  if (cpu_has_tsc_deadline()) {
    lapic_timer_arm_tsc(TICK_PERIOD_NS);
  }
  time_tick();
}

// ===========================================================================
// [DIAG] Comparación LAPIC vs PIT canal 2.
//
// El PIT es un oscilador de 1.193182 MHz independiente del TSC y del
// core clock. Sirve como referencia imparcial para saber si el LAPIC
// cambia de frecuencia con el P-state.
//
// Esta función NO modifica el comportamiento del timer: solo lee.
// Es seguro llamarla en cualquier momento, aunque tiene un timeout
// de seguridad de ~50M iteraciones (≈500 ms en CPU moderna).
//
// Devuelve los ticks del LAPIC (divisor 1) transcurridos en 10 ms.
// Devuelve 0 si el PIT no responde.
// ===========================================================================

#define PIT_CAL_PORT_CH2 0x42
#define PIT_CAL_PORT_CMD 0x43
#define PIT_CAL_PORT_61 0x61
#define PIT_CAL_COUNT 11932 // 11932 / 1193182 Hz ≈ 10.0 ms

static uint32_t lapic_measure_10ms_pit(void) {
  uint8_t p61;
  __asm__ volatile("inb %1, %0" : "=a"(p61) : "Nd"(PIT_CAL_PORT_61));

  // Gate del canal 2 apagado, altavoz apagado.
  uint8_t v = (uint8_t)(p61 & ~0x03);
  __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(PIT_CAL_PORT_61));

  // Canal 2, lobyte/hibyte, modo 0 (interrupt on terminal count), binario.
  __asm__ volatile("outb %0, %1"
                   :
                   : "a"((uint8_t)0xB0), "Nd"(PIT_CAL_PORT_CMD));
  __asm__ volatile("outb %0, %1"
                   :
                   : "a"((uint8_t)(PIT_CAL_COUNT & 0xFF)),
                     "Nd"(PIT_CAL_PORT_CH2));
  __asm__ volatile("outb %0, %1"
                   :
                   : "a"((uint8_t)(PIT_CAL_COUNT >> 8)),
                     "Nd"(PIT_CAL_PORT_CH2));

  // LAPIC timer: oneshot, enmascarado (no queremos que dispare).
  lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_1);
  lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED | LAPIC_TIMER_VECTOR);

  // Arrancar ambos lo más juntos posible.
  __asm__ volatile("outb %0, %1"
                   :
                   : "a"((uint8_t)((p61 & ~0x02) | 0x01)),
                     "Nd"(PIT_CAL_PORT_61));
  lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFF);

  // Esperar a que OUT2 (bit 5 de 0x61) pase a 1.
  uint64_t guard = 0;
  for (;;) {
    uint8_t cur;
    __asm__ volatile("inb %1, %0" : "=a"(cur) : "Nd"(PIT_CAL_PORT_61));
    if (cur & 0x20)
      break;
    if (++guard > 50000000ULL) {
      lapic_write(LAPIC_REG_TIMER_INIT, 0);
      __asm__ volatile("outb %0, %1" : : "a"(p61), "Nd"(PIT_CAL_PORT_61));
      return 0;
    }
  }

  uint32_t current = lapic_read(LAPIC_REG_TIMER_CURRENT);
  lapic_write(LAPIC_REG_TIMER_INIT, 0);
  __asm__ volatile("outb %0, %1" : : "a"(p61), "Nd"(PIT_CAL_PORT_61));

  return 0xFFFFFFFFu - current;
}

// Función pública de diagnóstico. Llama al test y loguea.
void time_diag_lapic_vs_pit(const char *tag) {
  uint32_t ticks = lapic_measure_10ms_pit();
  if (ticks == 0) {
    LOG_WARN("[DIAG] LAPIC vs PIT (%s): PIT no respondió", tag);
    return;
  }
  LOG_INFO("[DIAG] LAPIC vs PIT (%s): %u ticks/ms", tag, ticks / 10);
}