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

// [H3] Estado del shootdown en vuelo.
//
// Solo un shootdown a la vez, serializado por tlb_shootdown_lock.
// tlb_addr es el valor que el handler remoto lee; tlb_ack_count es el
// número de CPUs que ya han hecho su invalidación.
//
// tlb_shootdown_lock es un spinlock PLANO (sin irqsave). Si
// deshabilitásemos IRQs dentro del shootdown, otro CPU que también
// estuviera en ipi_tlb_shootdown no podría recibir nuestra IPI
// (necesaria para el ack) → deadlock.
static volatile uint64_t tlb_addr = 0;
static volatile int tlb_ack_count = 0;
static spinlock_t tlb_shootdown_lock;

static void icr_wait(void) {
  while (lapic_read(LAPIC_REG_ICR_LOW) & ICR_DELIVERY_PENDING) {
    __asm__ volatile("pause");
  }
}

void ipi_init(void) {
  spin_init(&tlb_shootdown_lock);
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

void ipi_handler_tlb(void) {
  lapic_eoi();

  // Snapshot del addr. El handler corre con IRQs off, así que no
  // puede ser reinterrumpido por otro IPI hasta hacer el ack.
  uint64_t addr = tlb_addr;

  if (addr == 0) {
    // Full TLB flush: recargar CR3 fuerza invalidación de todas las
    // entradas no-globales.
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
  } else {
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
  }

  // Barrera: el ack no puede publicarse antes que la invalidación.
  __sync_synchronize();
  __sync_fetch_and_add(&tlb_ack_count, 1);
}

// [H3] TLB shootdown cross-CPU.
void ipi_tlb_shootdown(uint64_t addr) {
  // Invalidación local primero. Siempre, aunque no haya APs.
  if (addr == 0) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
  } else {
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
  }

  // Cuántos CPUs online. Si solo estamos nosotros, hemos terminado.
  int ncpus = 1;
  if (smp_boot_params) {
    ncpus = smp_boot_params->aps_ready + 1;
    if (ncpus > MAX_CPUS)
      ncpus = MAX_CPUS;
  }
  if (ncpus <= 1)
    return;

  // Serializar shootdowns concurrentes.
  while (__sync_lock_test_and_set(&tlb_shootdown_lock.locked, 1)) {
    __asm__ volatile("pause");
  }

  tlb_addr = addr;
  tlb_ack_count = 0;
  __sync_synchronize();

  int expect = ncpus - 1;
  ipi_send_allbutself(IPI_VECTOR_TLB);

  while (__atomic_load_n(&tlb_ack_count, __ATOMIC_ACQUIRE) < expect) {
    __asm__ volatile("pause");
  }

  // Limpiar para el próximo shootdown. tlb_addr = 0 sería un flush
  // completo si alguien lo leyera ahora, que es seguro de todos modos.
  tlb_addr = 0;
  __sync_lock_release(&tlb_shootdown_lock.locked);
}