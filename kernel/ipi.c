// kernel/ipi.c
#include "ipi.h"
#include "apic.h"
#include "cpu.h"
#include "klog.h"
#include "sched.h"

// ICR bits (Intel SDM Vol 3, 10.6.1).
#define ICR_DELIVERY_FIXED (0 << 8)
#define ICR_DEST_PHYSICAL (0 << 11)
#define ICR_LEVEL_ASSERT (1 << 14)
#define ICR_TRIGGER_EDGE (0 << 15)
#define ICR_DEST_ALL_EXCL_SELF (3 << 18)
#define ICR_DEST_ALL_INCL_SELF (2 << 18)
#define ICR_DELIVERY_PENDING (1 << 12)

static void icr_wait(void) {
  while (lapic_read(LAPIC_REG_ICR_LOW) & ICR_DELIVERY_PENDING) {
    __asm__ volatile("pause");
  }
}

void ipi_init(void) {
  // Nada que hacer: los vectores ya están registrados en idt.c y el
  // LAPIC está activo. Esta función existe para futura inicialización
  // (por ejemplo, si quisiéramos configurar un vector de IPI concreto
  // por AP). La dejamos como punto de extensión.
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

// ---------------------------------------------------------------------------
// Handlers
//
// Llamados desde los stubs de isr_stubs.asm (push de vector + call).
// Ya estamos en ring 0 con IF=0 (el stub no hace sti). Hay que hacer
// EOI al LAPIC antes de retornar.
//
// Nota: los handlers C normales del kernel (irq_handler) hacen EOI al
// principio. Aquí, como el handler puede cambiar de contexto (resched),
// también hacemos EOI al principio.
// ---------------------------------------------------------------------------

void ipi_handler_resched(void) {
  lapic_eoi();

  // Marcar la tarea actual con need_resched. La lógica de salir del
  // idle o forzar el switch está en sched_tick() y en idle_loop().
  sched_mark_need_resched();
}

void ipi_handler_tlb(void) {
  // TODO Fase 4.4c: leer una variable global con la dirección a
  // invalidar y hacer invlpg. Por ahora, solo EOI.
  lapic_eoi();
}