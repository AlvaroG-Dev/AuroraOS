// kernel/usb/xhci/xhci_ring.h
#ifndef KERNEL_USB_XHCI_RING_H
#define KERNEL_USB_XHCI_RING_H

#include "xhci.h"
#include <stdint.h>

// Inicializa un ring. `trb_count` se redondea hacia arriba a la potencia
// de 2 más cercana (mínimo 4). Si `is_producer` es 1, reserva el último
// TRB como Link y deja `size - 1` slots usables.
//
// Devuelve 0 si OK, -ENOMEM si usb_dma_alloc falla, -EINVAL si args malos.
int xhci_ring_init(xhci_ring_t *ring, uint32_t trb_count, int is_producer);

void xhci_ring_destroy(xhci_ring_t *ring);

// Encola un TRB (solo producer). Copia `trb` al slot actual y OR-ea el
// cycle bit. Devuelve 0 en éxito, -EINVAL si ring es NULL o no es producer.
//
// Si el ring está a punto de wrap, actualiza el Link TRB al cycle nuevo,
// resetea enqueue a 0 y toggla el cycle. Esto es correcto en el modelo
// "polling síncrono" que usamos en Fase 1 (nunca hay un comando en vuelo
// mientras enqueuamos el siguiente).
int xhci_ring_enqueue(xhci_ring_t *ring, const xhci_trb_t *trb);

// Desencola un evento (solo event ring). Si no hay eventos disponibles
// (cycle bit del slot != cycle del consumer), devuelve -EAGAIN y no
// modifica nada. Si hay evento, copia a `out`, avanza dequeue y
// posiblemente toggla cycle al wrap. Devuelve 0.
int xhci_ring_dequeue(xhci_ring_t *ring, xhci_trb_t *out);

// Devuelve la dirección física del TRB que ocupa el slot `idx`.
// Útil para escribir ERDP (dequeue pointer) en el registro del interrupter.
uint64_t xhci_ring_trb_phys(const xhci_ring_t *ring, uint32_t idx);

// Self-test unitario. Devuelve 0 si OK, >0 = fallos. No toca hardware.
int xhci_ring_self_test(void);

#endif