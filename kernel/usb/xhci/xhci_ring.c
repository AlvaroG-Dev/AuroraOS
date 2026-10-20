// kernel/usb/xhci/xhci_ring.c
#include "xhci_ring.h"
#include "../../klog.h"
#include "../../string.h"
#include "../../uaccess.h" // EINVAL, ENOMEM, EAGAIN
#include "../usb_dma.h"

// Redondea a la potencia de 2 >= v. Mínimo 4.
static uint32_t round_pow2(uint32_t v) {
  if (v < 4)
    return 4;
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  return v + 1;
}

int xhci_ring_init(xhci_ring_t *ring, uint32_t trb_count, int is_producer) {
  if (!ring || trb_count == 0)
    return -EINVAL;

  trb_count = round_pow2(trb_count);
  if (trb_count > 4096)
    return -EINVAL;

  memset(ring, 0, sizeof(*ring));

  size_t bytes = (size_t)trb_count * sizeof(xhci_trb_t);
  usb_dma_buf_t buf;
  int rc = usb_dma_alloc(bytes, &buf);
  if (rc != 0)
    return rc;

  ring->trbs = (xhci_trb_t *)buf.virt;
  ring->phys = buf.phys;
  ring->size = trb_count;
  ring->enqueue = 0;
  ring->dequeue = 0;
  ring->cycle = 1;
  ring->is_producer = is_producer ? 1 : 0;

  // Producer rings: último slot es Link TRB. Cycle inicial = 1.
  if (is_producer) {
    xhci_trb_t *link = &ring->trbs[trb_count - 1];
    memset(link, 0, sizeof(*link));
    link->param = ring->phys;
    link->control = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC | XHCI_TRB_CYCLE;
  }

  return 0;
}

void xhci_ring_destroy(xhci_ring_t *ring) {
  if (!ring || !ring->trbs)
    return;
  usb_dma_buf_t buf = {
      .virt = ring->trbs,
      .phys = ring->phys,
      .size = (size_t)ring->size * sizeof(xhci_trb_t),
  };
  usb_dma_free(&buf);
  memset(ring, 0, sizeof(*ring));
}

int xhci_ring_enqueue(xhci_ring_t *ring, const xhci_trb_t *trb) {
  if (!ring || !ring->trbs || !trb || !ring->is_producer)
    return -EINVAL;

  // Wrap: último slot del array es el Link TRB (no usable por el
  // producer). Cuando enqueue llega ahí, hay que (xHCI §4.11.5.1, y lo
  // mismo que hace Linux en inc_enq):
  //   1. dar al Link TRB el cycle ACTUAL (antes del toggle): el HC solo
  //      sigue el Link si su cycle coincide con el que espera en esta
  //      vuelta. Si se le pone el cycle nuevo, el HC cree que el ring
  //      está vacío y se queda parado.
  //   2. copiarle el bit CH del TRB anterior (si el wrap cae en mitad de
  //      un TD, el Link debe ir encadenado),
  //   3. volver enqueue a 0,
  //   4. togglar el cycle del producer.
  if (ring->enqueue == ring->size - 1) {
    xhci_trb_t *link = &ring->trbs[ring->size - 1];
    uint32_t chain = ring->trbs[ring->size - 2].control & XHCI_TRB_CH;
    link->control =
        (link->control & ~(XHCI_TRB_CYCLE | XHCI_TRB_CH)) | ring->cycle | chain;
    __asm__ volatile("mfence" ::: "memory");
    ring->enqueue = 0;
    ring->cycle ^= 1;
  }

  // kernel/usb/xhci/xhci_ring.c

  xhci_trb_t *dst = &ring->trbs[ring->enqueue];
  *dst = *trb;
  dst->control = (dst->control & ~XHCI_TRB_CYCLE) | ring->cycle;

  // [FIX] Asegurar que la escritura del TRB sea visible para el hardware
  __asm__ volatile("mfence" ::: "memory");

  ring->enqueue++;
  return 0;
}

int xhci_ring_dequeue(xhci_ring_t *ring, xhci_trb_t *out) {
  if (!ring || !ring->trbs || !out || ring->is_producer)
    return -EINVAL;

  xhci_trb_t *src = &ring->trbs[ring->dequeue];
  if ((src->control & XHCI_TRB_CYCLE) != ring->cycle)
    return -EAGAIN; // ring vacío

  *out = *src;
  ring->dequeue++;
  if (ring->dequeue == ring->size) {
    ring->dequeue = 0;
    ring->cycle ^= 1;
  }
  return 0;
}

uint64_t xhci_ring_trb_phys(const xhci_ring_t *ring, uint32_t idx) {
  if (!ring || idx >= ring->size)
    return 0;
  return ring->phys + (uint64_t)idx * sizeof(xhci_trb_t);
}

// ===========================================================================
// Self-test (sin hardware)
// ===========================================================================
static int test_enqueue_no_wrap(void) {
  xhci_ring_t ring;
  if (xhci_ring_init(&ring, 8, 1) != 0)
    return 1;

  xhci_trb_t trb = {0};
  trb.control = XHCI_TRB_TYPE(XHCI_TRB_NORMAL);

  if (xhci_ring_enqueue(&ring, &trb) != 0) {
    xhci_ring_destroy(&ring);
    return 1;
  }
  if (ring.enqueue != 1) {
    xhci_ring_destroy(&ring);
    return 1;
  }
  if ((ring.trbs[0].control & XHCI_TRB_CYCLE) != 1) {
    xhci_ring_destroy(&ring);
    return 1;
  }
  if (XHCI_TRB_GET_TYPE(ring.trbs[0]) != XHCI_TRB_NORMAL) {
    xhci_ring_destroy(&ring);
    return 1;
  }

  xhci_ring_destroy(&ring);
  return 0;
}

static int test_enqueue_wrap(void) {
  xhci_ring_t ring;
  if (xhci_ring_init(&ring, 4, 1) != 0) // 4 TRBs: 0,1,2 usables + 3 Link
    return 1;

  xhci_trb_t trb = {0};
  trb.control = XHCI_TRB_TYPE(XHCI_TRB_NORMAL);

  // Llenar slots 0, 1, 2 con cycle=1
  for (int i = 0; i < 3; i++) {
    if (xhci_ring_enqueue(&ring, &trb) != 0) {
      xhci_ring_destroy(&ring);
      return 1;
    }
  }
  if (ring.enqueue != 3) {
    xhci_ring_destroy(&ring);
    return 1;
  }
  if (ring.cycle != 1) {
    xhci_ring_destroy(&ring);
    return 1;
  }

  // Link TRB en slot 3 con cycle=1
  if ((ring.trbs[3].control & XHCI_TRB_CYCLE) != 1) {
    xhci_ring_destroy(&ring);
    return 1;
  }
  if (XHCI_TRB_GET_TYPE(ring.trbs[3]) != XHCI_TRB_LINK) {
    xhci_ring_destroy(&ring);
    return 1;
  }

  // Siguiente enqueue fuerza wrap: enqueue→0, cycle→0, y el Link conserva
  // el cycle de la vuelta que termina (1) para que el HC lo siga.
  if (xhci_ring_enqueue(&ring, &trb) != 0) {
    xhci_ring_destroy(&ring);
    return 1;
  }
  if (ring.enqueue != 1) {
    xhci_ring_destroy(&ring);
    return 1;
  }
  if (ring.cycle != 0) {
    xhci_ring_destroy(&ring);
    return 1;
  }
  if ((ring.trbs[3].control & XHCI_TRB_CYCLE) != 1) {
    xhci_ring_destroy(&ring);
    return 1;
  }
  if ((ring.trbs[0].control & XHCI_TRB_CYCLE) != 0) {
    xhci_ring_destroy(&ring);
    return 1;
  }

  xhci_ring_destroy(&ring);
  return 0;
}

static int test_dequeue(void) {
  xhci_ring_t ring;
  if (xhci_ring_init(&ring, 4, 0) != 0) // event ring (consumer)
    return 1;

  xhci_trb_t out;
  // Ring vacío: todos los slots con cycle=0 inicialmente.
  if (xhci_ring_dequeue(&ring, &out) != -EAGAIN) {
    xhci_ring_destroy(&ring);
    return 1;
  }

  // Simular que el HC escribió un evento: cycle=1 en slot 0.
  xhci_trb_t evt = {0};
  evt.control = XHCI_TRB_TYPE(XHCI_TRB_CMD_COMPLETION) | XHCI_TRB_CYCLE;
  ring.trbs[0] = evt;

  if (xhci_ring_dequeue(&ring, &out) != 0) {
    xhci_ring_destroy(&ring);
    return 1;
  }
  if (XHCI_TRB_GET_TYPE(out) != XHCI_TRB_CMD_COMPLETION) {
    xhci_ring_destroy(&ring);
    return 1;
  }
  if (ring.dequeue != 1) {
    xhci_ring_destroy(&ring);
    return 1;
  }

  // Sin más eventos: EAGAIN.
  if (xhci_ring_dequeue(&ring, &out) != -EAGAIN) {
    xhci_ring_destroy(&ring);
    return 1;
  }

  // Simular wrap: escribir 4 eventos más con cycle=1 hasta slot 3,
  // luego wrap a 0 con cycle=0.
  ring.trbs[1].control =
      XHCI_TRB_TYPE(XHCI_TRB_CMD_COMPLETION) | XHCI_TRB_CYCLE;
  ring.trbs[2].control =
      XHCI_TRB_TYPE(XHCI_TRB_CMD_COMPLETION) | XHCI_TRB_CYCLE;
  ring.trbs[3].control =
      XHCI_TRB_TYPE(XHCI_TRB_CMD_COMPLETION) | XHCI_TRB_CYCLE;
  for (int i = 0; i < 3; i++) {
    if (xhci_ring_dequeue(&ring, &out) != 0) {
      xhci_ring_destroy(&ring);
      return 1;
    }
  }
  // Ahora dequeue=0, cycle=0. El slot 0 con cycle=1 no debe matchear.
  if (xhci_ring_dequeue(&ring, &out) != -EAGAIN) {
    xhci_ring_destroy(&ring);
    return 1;
  }

  xhci_ring_destroy(&ring);
  return 0;
}

int xhci_ring_self_test(void) {
  int fails = 0;
  fails += test_enqueue_no_wrap();
  fails += test_enqueue_wrap();
  fails += test_dequeue();
  if (fails == 0)
    LOG_INFO("[xHCI-ring] self-test OK");
  else
    LOG_ERR("[xHCI-ring] self-test: %d fallos", fails);
  return fails;
}