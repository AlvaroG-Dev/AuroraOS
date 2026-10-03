// kernel/ipi.c
#include "ipi.h"
#include "apic.h"
#include "cpu.h"
#include "klog.h"
#include "sched.h"
#include "smp_boot.h"
#include "spinlock.h"

// ICR bits (Intel SDM Vol 3, 10.6.1).
#define ICR_DELIVERY_FIXED (0 << 8)
#define ICR_DEST_PHYSICAL (0 << 11)
#define ICR_LEVEL_ASSERT (1 << 14)
#define ICR_TRIGGER_EDGE (0 << 15)
#define ICR_DEST_ALL_EXCL_SELF (3 << 18)
#define ICR_DEST_ALL_INCL_SELF (2 << 18)
#define ICR_DELIVERY_PENDING (1 << 12)

// ---------------------------------------------------------------------------
// [H3] TLB shootdown.
//
// Protocolo (sin deadlock aunque el llamante tenga IRQs apagadas):
//
//   - Solo un shootdown a la vez, serializado por tlb_shootdown_lock
//     (spinlock PLANO, sin irqsave).
//   - tlb_addr es la dirección a invalidar (0 = flush completo).
//   - tlb_pending es una máscara: el bit i está a 1 mientras la CPU i
//     todavía no ha invalidado. El emisor la rellena con las CPUs
//     destino y espera a que llegue a 0.
//   - Una CPU atiende su bit desde tlb_service(), que se llama:
//       * desde el handler de la IPI (ipi_handler_tlb), y
//       * desde los bucles de espera del propio shootdown.
//     Esto último es lo que evita el deadlock: una CPU que gira
//     esperando tlb_shootdown_lock con IRQs apagadas ya no necesita
//     recibir la IPI para dar su ack; atiende la petición ella misma.
//
// tlb_service() es idempotente: si la IPI llega después de que la CPU ya
// atendió su bit por polling, no hace nada.
//
// Suposición: las CPUs online son los ids 0..ncpus-1 y ncpus <= 64.
// ---------------------------------------------------------------------------
static volatile uint64_t tlb_addr = 0;
static volatile uint64_t tlb_pending = 0;
static spinlock_t tlb_shootdown_lock;

static void icr_wait(void) {
  while (lapic_read(LAPIC_REG_ICR_LOW) & ICR_DELIVERY_PENDING) {
    __asm__ volatile("pause");
  }
}

void ipi_init(void) {
  spin_init(&tlb_shootdown_lock);
  tlb_addr = 0;
  tlb_pending = 0;
  LOG_INFO("[IPI] Subsistema de IPIs inicializado (vectores 0x%x, 0x%x, "
           "0x%x, 0x%x)",
           IPI_VECTOR_RESCHED, IPI_VECTOR_TLB, IPI_VECTOR_CALL,
           IPI_VECTOR_HALT);
}

void ipi_send(uint32_t apic_id, uint8_t vector) {
  icr_wait();
  lapic_write(LAPIC_REG_ICR_HIGH, (apic_id & 0xFF) << 24);
  lapic_write(LAPIC_REG_ICR_LOW, ICR_DELIVERY_FIXED | ICR_DEST_PHYSICAL |
                                     ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE |
                                     (uint32_t)vector);
}

void ipi_send_allbutself(uint8_t vector) {
  icr_wait();
  lapic_write(LAPIC_REG_ICR_HIGH, 0);
  lapic_write(LAPIC_REG_ICR_LOW, ICR_DELIVERY_FIXED | ICR_DEST_ALL_EXCL_SELF |
                                     ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE |
                                     (uint32_t)vector);
}

void ipi_send_all(uint8_t vector) {
  icr_wait();
  lapic_write(LAPIC_REG_ICR_HIGH, 0);
  lapic_write(LAPIC_REG_ICR_LOW, ICR_DELIVERY_FIXED | ICR_DEST_ALL_INCL_SELF |
                                     ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE |
                                     (uint32_t)vector);
}

void ipi_handler_resched(void) {
  lapic_eoi();
  sched_mark_need_resched();
}

// Invalida el TLB de la CPU actual. addr == 0 => flush completo de las
// entradas no globales (recargar CR3).
static inline void tlb_flush_local(uint64_t addr) {
  if (addr == 0) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
  } else {
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
  }
}

// Atiende la petición pendiente de ESTA CPU, si existe. Segura desde el
// handler de la IPI y desde bucles de espera con IRQs apagadas.
static void tlb_service(void) {
  uint64_t bit = 1ULL << smp_processor_id();
  if (!(__atomic_load_n(&tlb_pending, __ATOMIC_ACQUIRE) & bit))
    return;

  // El emisor escribe tlb_addr ANTES de publicar tlb_pending con
  // release, y nosotros hemos leído el bit con acquire: tlb_addr es
  // el valor de esta ronda.
  tlb_flush_local(tlb_addr);

  // El ack (limpiar el bit) no puede publicarse antes que la
  // invalidación.
  __atomic_fetch_and(&tlb_pending, ~bit, __ATOMIC_RELEASE);
}

void ipi_handler_tlb(void) {
  lapic_eoi();
  tlb_service();
}

// [H3] TLB shootdown cross-CPU.
void ipi_tlb_shootdown(uint64_t addr) {
  // smp_processor_id() debe ser estable mientras dura el protocolo:
  // si la tarea migrase, calcularíamos mal la máscara de destinos.
  preempt_disable();

  // Invalidación local primero. Siempre, aunque no haya APs.
  tlb_flush_local(addr);

  // Cuántas CPUs online. Si solo estamos nosotros, hemos terminado.
  int ncpus = 1;
  if (smp_boot_params) {
    ncpus = smp_boot_params->aps_ready + 1;
    if (ncpus > MAX_CPUS)
      ncpus = MAX_CPUS;
  }
  if (ncpus <= 1) {
    preempt_enable();
    return;
  }

  uint64_t me = 1ULL << smp_processor_id();
  uint64_t targets = ((1ULL << ncpus) - 1) & ~me;

  // Serializar shootdowns concurrentes. Mientras giramos atendemos las
  // peticiones ajenas: así otro CPU que sostiene el lock y espera
  // nuestro ack lo recibe aunque nosotros tengamos IRQs apagadas.
  while (__sync_lock_test_and_set(&tlb_shootdown_lock.locked, 1)) {
    tlb_service();
    __asm__ volatile("pause");
  }

  tlb_addr = addr;
  __atomic_store_n(&tlb_pending, targets, __ATOMIC_RELEASE);
  ipi_send_allbutself(IPI_VECTOR_TLB);

  // Esperar a que todas las CPUs destino hayan invalidado.
  uint64_t spins = 0;
  while (__atomic_load_n(&tlb_pending, __ATOMIC_ACQUIRE) != 0) {
    __asm__ volatile("pause");
    if (++spins == 400000000ULL) {
      LOG_ERR("[IPI] shootdown atascado: pending=0x%lx addr=0x%lx cpu=%d",
              (unsigned long)tlb_pending, (unsigned long)addr,
              (int)smp_processor_id());
      spins = 0;
    }
  }

  __sync_lock_release(&tlb_shootdown_lock.locked);
  preempt_enable();
}

void ipi_tlb_shootdown_all(void) { ipi_tlb_shootdown(0); }