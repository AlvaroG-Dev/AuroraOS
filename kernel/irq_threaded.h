// kernel/irq_threaded.h
#ifndef KERNEL_IRQ_THREADED_H
#define KERNEL_IRQ_THREADED_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Threaded IRQ handlers.
//
// Modelo:
//
//   irq_request_threaded(irq, ack, thread_fn, name, dev_id)
//
//   * `ack` corre en contexto HARD-IRQ (IF=0, sin scheduler, sin dormir).
//     Debe hacer el mínimo trabajo para SILENCIAR la fuente: leer el
//     registro de estado del dispositivo, enmascarar la causa y devolver.
//     Si no silencia la fuente, el IOAPIC volverá a disparar la línea
//     (level-triggered) y tendrás un IRQ storm.
//
//   * `thread_fn` corre en un kthread DEDICADO (creado con kthread_run).
//     Puede dormir, tomar mutexes, hacer I/O. Aquí va el trabajo pesado:
//     procesar paquetes RX, atender transfers completados, etc.
//
// Si `thread_fn == NULL`, equivale a `irq_install_handler_ex`: todo el
// trabajo (y el ack) corre en hard-IRQ. Útil para migrar un driver
// poco a poco.
//
// Si `ack == NULL`, el kthread se despierta siempre que llegue la IRQ.
// Esto solo es seguro si la fuente es edge-triggered y el propio
// dispositivo la silencia al ser leído en el thread_fn. En la práctica,
// provee siempre un `ack` para IRQs level-triggered.
//
// Semántica de `irq`:
//   - 0..15                 → IRQs legacy del IOAPIC (INTx)
//   - MSI_VECTOR_BASE..END  → vectores MSI (0x60..0x7F)
//
// Este rango unificado simplifica el dispatch: tanto `irq_handler` como
// `msi_handler` indexan la misma tabla `g_actions[]`.
// ---------------------------------------------------------------------------

typedef enum {
  IRQ_NONE = 0,       // La IRQ no era para este handler.
  IRQ_HANDLED = 1,    // Atendida (y, si hay thread_fn, no se dispara).
  IRQ_WAKE_THREAD = 2 // Atendida; despertar el thread_fn.
} irqreturn_t;

typedef irqreturn_t (*irq_handler_fn)(int irq, void *dev_id);

// Registra un handler threaded en `irq`. Devuelve 0, -EINVAL si los
// argumentos son inválidos o el irq está fuera de rango, -EBUSY si ya
// hay un handler registrado en ese irq, -ENOMEM si falla la creación
// del kthread.
int irq_request_threaded(unsigned int irq, irq_handler_fn ack,
                         irq_handler_fn thread_fn, const char *name,
                         void *dev_id);

// Desregistra y libera el kthread asociado. Si el thread_fn está
// corriendo, esta función BLOQUEA hasta que termine. No-op si `irq` no
// tiene handler.
void irq_free(unsigned int irq);

// Init. Debe llamarse después de workqueue_init() y antes de cualquier
// irq_request_threaded. Idempotente.
int irq_threaded_init(void);

// Simula el disparo de una IRQ threaded. Llama al `ack` desde el
// contexto actual y despierta el thread si retorna IRQ_WAKE_THREAD.
//
// SOLO PARA TESTS. En producción, la IRQ llega desde el hardware a
// través de `irq_handler`/`msi_handler`. Expuesta con este nombre
// explícito para que quede claro que no se usa fuera de test.c.
void irq_threaded_simulate_for_test(unsigned int irq);

#endif