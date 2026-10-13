// kernel/net/e1000e.c
//
// Driver Intel 82574L (e1000e). Solo modo legacy descriptors.
//
// Referencias:
//   - Intel 82574 GbE Controller Datasheet (rev 2.1)
//   - Linux drivers/net/ethernet/intel/e1000e/
//
// Decisiones de diseño:
//   - RX por polling. El kthread arranca en e1000e_start().
//   - TX copia síncrona al buffer del descriptor.
//   - Sin checksum offload, sin segmentation offload, sin multi-queue.
//   - MAC leída de RAL/RAH (válida tras reset; QEMU la pre-carga).

#include "e1000e.h"
#include "../apic.h" // lapic_get_bsp_id()
#include "../idt.h"  // MSI_VECTOR_BASE, msi_install_handler()
#include "../klog.h"
#include "../paging.h"
#include "../pci.h"
#include "../pmm.h"
#include "../sched.h"
#include "../spinlock.h"
#include "../string.h"
#include "../uaccess.h"
#include "../wait.h"
#include "netif.h"
#include "skb.h"

// ---------------------------------------------------------------------------
// Registros (offset desde BAR0)
// ---------------------------------------------------------------------------
#define E1000_CTRL 0x0000
#define E1000_STATUS 0x0008
#define E1000_CTRL_EXT 0x0018
#define E1000_ICR 0x00C0
#define E1000_IMS 0x00D0
#define E1000_IMC 0x00D8
#define E1000_RCTL 0x0100
#define E1000_TCTL 0x0400
#define E1000_RDBAL 0x2800
#define E1000_RDBAH 0x2804
#define E1000_RDLEN 0x2808
#define E1000_RDH 0x2810
#define E1000_RDT 0x2818
#define E1000_TDBAL 0x3800
#define E1000_TDBAH 0x3804
#define E1000_TDLEN 0x3808
#define E1000_TDH 0x3810
#define E1000_TDT 0x3818
#define E1000_RAL 0x5400
#define E1000_RAH 0x5404

// CTRL bits
#define E1000_CTRL_FD (1u << 0)
#define E1000_CTRL_LRST (1u << 3)
#define E1000_CTRL_SLU (1u << 6)
#define E1000_CTRL_RST (1u << 26)

// RCTL bits
#define E1000_RCTL_EN (1u << 1)
#define E1000_RCTL_SBP (1u << 2)
#define E1000_RCTL_UPE (1u << 3)
#define E1000_RCTL_MPE (1u << 4)
#define E1000_RCTL_BAM (1u << 15)
#define E1000_RCTL_SECRC (1u << 26)
#define E1000_RCTL_BSIZE_2048 0u // campo 16-17 = 0 → 2048 B

// TCTL bits
#define E1000_TCTL_EN (1u << 1)
#define E1000_TCTL_PSP (1u << 3)
// CT=0x10 (16), COLD=0x40 (64) → << 4 y << 12 respectivamente.
#define E1000_TCTL_CT_SHIFT 4
#define E1000_TCTL_COLD_SHIFT 12

#define E1000_VENDOR_INTEL 0x8086u
#define E1000_DEV_82574L 0x10D3u  // QEMU default (e1000e)
#define E1000_DEV_82540EM 0x100Eu // VirtualBox "Intel PRO/1000 MT Desktop"
#define E1000_DEV_82545EM 0x100Fu // VirtualBox "Intel PRO/1000 MT Server"

// ICR / IMS bits (e1000e datasheet 13.4.10-13.4.13).
#define E1000_ICR_TXDW 0x00000001u   // TX descriptor written back
#define E1000_ICR_TXQE 0x00000002u   // TX queue empty
#define E1000_ICR_LSC 0x00000004u    // link status change
#define E1000_ICR_RXSEQ 0x00000008u  // RX sequence error
#define E1000_ICR_RXDMT0 0x00000010u // RX descriptor min threshold
#define E1000_ICR_RXO 0x00000040u    // RX overrun
#define E1000_ICR_RXT0 0x00000080u   // RX timer

// ---------------------------------------------------------------------------
// Descriptores legacy
// ---------------------------------------------------------------------------
#define DESC_COUNT 256
#define BUF_SIZE 2048

// Máx. de descriptors RX procesados por pasada y cada cuántos se
// devuelve RDT al HW (evita escribir MMIO por cada paquete).
#define RX_BUDGET 64
#define RX_RDT_BATCH 16
// Timeout de seguridad del kthread RX (ms). Con MSI solo es una red de
// seguridad ante IRQs perdidas; sin MSI es el periodo de polling.
#define RX_POLL_MS_MSI 10
#define RX_POLL_MS_POLLING 1

typedef struct __attribute__((packed)) {
  uint64_t addr;
  uint16_t length;
  uint16_t csum;
  uint8_t status;
  uint8_t errors;
  uint16_t special;
} e1000e_rx_desc_t;

typedef struct __attribute__((packed)) {
  uint64_t addr;
  uint16_t length;
  uint8_t cso;
  uint8_t cmd;
  uint8_t status;
  uint8_t css;
  uint16_t special;
} e1000e_tx_desc_t;

#define RX_DD 0x01u
#define TX_DD 0x01u
#define TX_CMD_EOP 0x01u
#define TX_CMD_IFCS 0x02u
#define TX_CMD_RS 0x08u

// ---------------------------------------------------------------------------
// Estado
// ---------------------------------------------------------------------------
static pci_device_t g_pci;
static volatile uint32_t *g_mmio;
static netif_t g_nic;
static netif_ops_t g_ops;

static uint64_t g_rx_ring_phys;
static uint64_t g_tx_ring_phys;
static uint64_t g_rx_buf_base_phys;
static uint64_t g_tx_buf_base_phys;

static e1000e_rx_desc_t *g_rx_ring;
static e1000e_tx_desc_t *g_tx_ring;

static uint32_t g_rx_tail; // próximo descriptor a procesar
static uint32_t g_tx_head; // próximo descriptor a usar
static spinlock_t g_lock;
static volatile int g_rx_poll_active;
static volatile int g_rx_pending;
static wait_queue_t g_rx_wq;
static int g_initialized;
static int g_started;
static int g_use_msi;
static uint8_t g_msi_vector;

// ---------------------------------------------------------------------------
// MMIO helpers
// ---------------------------------------------------------------------------
static inline uint32_t e1000_read(uint32_t reg) { return g_mmio[reg / 4]; }
static inline void e1000_write(uint32_t reg, uint32_t val) {
  g_mmio[reg / 4] = val;
}
// El e1000e puede reordenar escrituras. Para forzar el flush basta leer
// un registro cualquiera después de escribir; STATUS vale.
static inline void e1000_flush(void) { (void)e1000_read(E1000_STATUS); }

// ---------------------------------------------------------------------------
// Reset y configuración
// ---------------------------------------------------------------------------
static int e1000e_reset(void) {
  // CTRL.RST: self-clearing. Esperamos a que el bit baje.
  e1000_write(E1000_CTRL, E1000_CTRL_RST);
  e1000_flush();

  for (int i = 0; i < 1000000; i++) {
    if (!(e1000_read(E1000_CTRL) & E1000_CTRL_RST))
      break;
    __asm__ volatile("pause");
    if (i == 999999) {
      LOG_ERR("[E1000E] reset timeout");
      return -EIO;
    }
  }

  // Enmascarar todas las interrupciones (no usamos MSI todavía).
  e1000_write(E1000_IMC, 0xFFFFFFFFu);
  e1000_flush();

  // Link up.
  uint32_t ctrl = e1000_read(E1000_CTRL);
  e1000_write(E1000_CTRL, ctrl | E1000_CTRL_SLU | E1000_CTRL_FD);
  e1000_flush();
  return 0;
}

static void e1000e_read_mac(uint8_t mac[6]) {
  uint32_t ral = e1000_read(E1000_RAL);
  uint32_t rah = e1000_read(E1000_RAH);
  mac[0] = (uint8_t)(ral & 0xFF);
  mac[1] = (uint8_t)((ral >> 8) & 0xFF);
  mac[2] = (uint8_t)((ral >> 16) & 0xFF);
  mac[3] = (uint8_t)((ral >> 24) & 0xFF);
  mac[4] = (uint8_t)(rah & 0xFF);
  mac[5] = (uint8_t)((rah >> 8) & 0xFF);

  int all_zero = 1, all_ff = 1;
  for (int i = 0; i < 6; i++) {
    if (mac[i] != 0)
      all_zero = 0;
    if (mac[i] != 0xFF)
      all_ff = 0;
  }
  if (all_zero || all_ff) {
    // Fallback: MAC localmente administrada, unicast.
    static const uint8_t fallback[6] = {0x02, 0xE0, 0x00, 0x00, 0x00, 0x01};
    memcpy(mac, fallback, 6);
    LOG_WARN("[E1000E] RAL/RAH no traen MAC; usando fallback");
  }
}

static void e1000e_setup_rx(void) {
  e1000_write(E1000_RDBAL, (uint32_t)(g_rx_ring_phys & 0xFFFFFFFFu));
  e1000_write(E1000_RDBAH, (uint32_t)(g_rx_ring_phys >> 32));
  e1000_write(E1000_RDLEN, DESC_COUNT * sizeof(e1000e_rx_desc_t));
  e1000_write(E1000_RDH, 0);
  e1000_write(E1000_RDT, DESC_COUNT - 1);

  uint32_t rctl =
      E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC | E1000_RCTL_BSIZE_2048;
  e1000_write(E1000_RCTL, rctl);
  // Si vamos por MSI, habilitar los eventos de RX en IMS.
  // IMC antes para limpiar cualquier máscara previa.
  if (g_use_msi) {
    e1000_write(E1000_IMC, 0xFFFFFFFFu);
    e1000_write(E1000_IMS, E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_RXSEQ |
                               E1000_ICR_RXO | E1000_ICR_LSC);
    e1000_flush();
  }
}

static void e1000e_setup_tx(void) {
  e1000_write(E1000_TDBAL, (uint32_t)(g_tx_ring_phys & 0xFFFFFFFFu));
  e1000_write(E1000_TDBAH, (uint32_t)(g_tx_ring_phys >> 32));
  e1000_write(E1000_TDLEN, DESC_COUNT * sizeof(e1000e_tx_desc_t));
  e1000_write(E1000_TDH, 0);
  e1000_write(E1000_TDT, 0);

  uint32_t tctl = E1000_TCTL_EN | E1000_TCTL_PSP |
                  (0x10u << E1000_TCTL_CT_SHIFT) |
                  (0x40u << E1000_TCTL_COLD_SHIFT);
  e1000_write(E1000_TCTL, tctl);
  e1000_flush();
}

static void e1000e_setup_rings(void) {
  g_rx_ring = (e1000e_rx_desc_t *)phys_to_virt(g_rx_ring_phys);
  g_tx_ring = (e1000e_tx_desc_t *)phys_to_virt(g_tx_ring_phys);
  memset(g_rx_ring, 0, DESC_COUNT * sizeof(e1000e_rx_desc_t));
  memset(g_tx_ring, 0, DESC_COUNT * sizeof(e1000e_tx_desc_t));

  for (int i = 0; i < DESC_COUNT; i++) {
    uint64_t rx_buf = g_rx_buf_base_phys + (uint64_t)i * BUF_SIZE;
    uint64_t tx_buf = g_tx_buf_base_phys + (uint64_t)i * BUF_SIZE;
    g_rx_ring[i].addr = rx_buf;
    g_rx_ring[i].status = 0;
    g_tx_ring[i].addr = tx_buf;
    g_tx_ring[i].status = TX_DD; // "libre" desde el punto de vista del SW
  }

  g_rx_tail = 0;
  g_tx_head = 0;
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------
static int e1000e_transmit(netif_t *netif, skb_t *skb) {
  (void)netif;
  if (!skb)
    return -EINVAL;
  if (!g_initialized) {
    skb_free(skb);
    return -ENODEV;
  }

  size_t len = skb_len(skb);
  if (len > BUF_SIZE) {
    LOG_WARN("[E1000E] tx drop: frame %lu > %u", (unsigned long)len, BUF_SIZE);
    skb_free(skb);
    return -EMSGSIZE;
  }

  unsigned long flags = spin_lock_irqsave(&g_lock);

  e1000e_tx_desc_t *d = &g_tx_ring[g_tx_head];
  // Si el descriptor todavía no ha sido consumido por el HW, esperamos.
  // Es un caso raro porque TDLEN == DESC_COUNT y el HW avanza rápido.
  for (int i = 0; i < 1000000 && !(d->status & TX_DD); i++)
    __asm__ volatile("pause");
  if (!(d->status & TX_DD)) {
    spin_unlock_irqrestore(&g_lock, flags);
    LOG_WARN("[E1000E] tx ring lleno (desc %u sin DD)", g_tx_head);
    skb_free(skb);
    return -EAGAIN;
  }

  uint8_t *dst = (uint8_t *)phys_to_virt(d->addr);
  memcpy(dst, skb_data(skb), len);

  d->length = (uint16_t)len;
  d->cso = 0;
  d->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
  d->status = 0; // el HW lo pondrá a TX_DD cuando termine
  d->css = 0;
  d->special = 0;

  g_tx_head = (g_tx_head + 1) % DESC_COUNT;
  // El descriptor debe ser visible antes de avanzar TDT. La escritura MMIO
  // es posted: no hace falta leer un registro para "flushear" (esa lectura
  // costaba un exit/espera por cada paquete enviado).
  __sync_synchronize();
  e1000_write(E1000_TDT, g_tx_head);

  spin_unlock_irqrestore(&g_lock, flags);
  skb_free(skb);
  return 0;
}

static int e1000e_open(netif_t *netif) {
  (void)netif;
  return 0;
}
static int e1000e_close(netif_t *netif) {
  (void)netif;
  return 0;
}

// ---------------------------------------------------------------------------
// RX polling
// ---------------------------------------------------------------------------
// Estadísticas de diagnóstico (solo contadores, sin lock).
static volatile uint32_t
    g_stat_timeout_hits;                   // pasadas con paquetes tras timeout
static volatile uint32_t g_stat_irq_wakes; // despertares por MSI

// Procesa hasta RX_BUDGET descriptors. Devuelve cuántos ha consumido.
static int e1000e_poll_batch(void) {
  if (!g_initialized)
    return 0;

  // El hilo RX y el MSI pueden entrar mientras netif_rx procesa un paquete
  // con g_lock liberado. Solo un consumidor puede avanzar el ring y despachar
  // paquetes, para no duplicar descriptors ni entregar TCP fuera de orden.
  if (__atomic_exchange_n(&g_rx_poll_active, 1, __ATOMIC_ACQUIRE))
    return 0;

  int done = 0;
  unsigned long flags = spin_lock_irqsave(&g_lock);
  while (done < RX_BUDGET) {
    e1000e_rx_desc_t *d = &g_rx_ring[g_rx_tail];
    if (!(d->status & RX_DD))
      break;

    uint16_t len = d->length;
    if (len > 0) {
      // El descriptor puede tener errores; los descartamos.
      if (d->errors == 0) {
        skb_t *sk = skb_alloc();
        if (sk) {
          uint8_t *p = (uint8_t *)skb_put(sk, len);
          if (p) {
            memcpy(p, (const uint8_t *)phys_to_virt(d->addr), len);
            sk->netif = &g_nic;
            // Pasar por netif_rx SIN g_lock: netif_rx no lo toca y el
            // camino TCP puede transmitir (ACK) y necesita g_lock.
            spin_unlock_irqrestore(&g_lock, flags);
            netif_rx(sk);
            flags = spin_lock_irqsave(&g_lock);
          } else {
            skb_free(sk);
          }
        } else {
          g_nic.rx_dropped++;
        }
      } else {
        g_nic.rx_errors++;
      }
    }

    // Devolver el descriptor al HW.
    d->status = 0;
    d->errors = 0;
    d->length = 0;
    g_rx_tail = (g_rx_tail + 1) % DESC_COUNT;
    done++;

    // RDT apunta al ÚLTIMO descriptor que el HW puede escribir
    // ((tail - 1) mod COUNT). Lo actualizamos por lotes, sin lectura de
    // flush: antes eran 2 accesos MMIO por paquete.
    if ((done & (RX_RDT_BATCH - 1)) == 0)
      e1000_write(E1000_RDT, (g_rx_tail + DESC_COUNT - 1) % DESC_COUNT);
  }
  if (done & (RX_RDT_BATCH - 1))
    e1000_write(E1000_RDT, (g_rx_tail + DESC_COUNT - 1) % DESC_COUNT);
  spin_unlock_irqrestore(&g_lock, flags);
  __atomic_store_n(&g_rx_poll_active, 0, __ATOMIC_RELEASE);
  return done;
}

void e1000e_poll(void) { (void)e1000e_poll_batch(); }

// ---------------------------------------------------------------------------
// Kthread de RX
// ---------------------------------------------------------------------------
static bool e1000e_rx_pending(void *arg) {
  (void)arg;
  return __atomic_load_n(&g_rx_pending, __ATOMIC_ACQUIRE) != 0;
}

static void e1000e_rx_thread(void) {
  LOG_INFO("[E1000E] kthread RX arrancado (msi=%d)", g_use_msi);

  int woke_by_timeout = 0;
  while (1) {
    __atomic_store_n(&g_rx_pending, 0, __ATOMIC_RELEASE);
    int n = e1000e_poll_batch();

    if (n > 0 && woke_by_timeout && g_use_msi) {
      // Había paquetes esperando y el MSI no nos despertó: la IRQ no se
      // está entregando (o llega tarde). Diagnóstico para dmesg.
      uint32_t c = ++g_stat_timeout_hits;
      if ((c & 0x3FF) == 1)
        LOG_WARN("[E1000E] %u pasadas RX recogidas por timeout con MSI activo "
                 "(irq_wakes=%u): el MSI no llega o llega tarde",
                 c, g_stat_irq_wakes);
    }
    woke_by_timeout = 0;

    if (n >= RX_BUDGET)
      continue; // posible backlog: otra pasada sin dormir

    if (!e1000e_rx_pending(NULL)) {
      long r = wait_event_interruptible_timeout(
          &g_rx_wq, e1000e_rx_pending, NULL,
          g_use_msi ? RX_POLL_MS_MSI : RX_POLL_MS_POLLING);
      if (r == 0)
        woke_by_timeout = 1;
      else if (r > 0)
        g_stat_irq_wakes++;
    }
  }
}

// Handler MSI del e1000e. Solo lee ICR y despierta el consumidor RX; el
// despacho IP/TCP se mantiene fuera del contexto de interrupción.
static void e1000e_msi_handler(void) {
  if (!g_initialized)
    return;
  // Lectura con auto-clear de los bits marcados.
  uint32_t icr = e1000_read(E1000_ICR);
  if (icr == 0)
    return; // spurious, no hay nada que procesar

  // LSC: por ahora solo logueamos. Más adelante reconfiguramos speed/duplex.
  if (icr & E1000_ICR_LSC) {
    uint32_t status = e1000_read(E1000_STATUS);
    LOG_INFO("[E1000E] link status change: STATUS=0x%08x (LU=%d)", status,
             !!(status & (1u << 1)));
  }

  // RX: procesar el ring. Cubre RXT0, RXDMT0, RXO y RXSEQ.
  if (icr &
      (E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_RXO | E1000_ICR_RXSEQ)) {
    __atomic_store_n(&g_rx_pending, 1, __ATOMIC_RELEASE);
    wake_up_one(&g_rx_wq);
  }
}
// ---------------------------------------------------------------------------
// Init público
// ---------------------------------------------------------------------------
int e1000e_init(void) {
  if (g_initialized)
    return 0;

  spin_init(&g_lock);
  __atomic_store_n(&g_rx_poll_active, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_rx_pending, 0, __ATOMIC_RELAXED);
  wait_queue_init(&g_rx_wq);

  // 1) Localizar la NIC por vendor/device.
  int found = 0;
  uint8_t sb = 0, ss = 0, sf = 0;
  pci_device_t d;
  while (pci_find_next_device(0x02, 0x00, &sb, &ss, &sf, &d) == 0) {
    if (d.vendor_id == E1000_VENDOR_INTEL &&
        (d.device_id == E1000_DEV_82574L || d.device_id == E1000_DEV_82540EM ||
         d.device_id == E1000_DEV_82545EM)) {
      g_pci = d;
      found = 1;
      break;
    }
    // Avanzar el cursor para la próxima iteración.
    if (++sf == 0) {
      sf = 0;
      if (++ss == 0) {
        ss = 0;
        sb++;
      }
    }
  }
  if (!found) {
    LOG_WARN("[E1000E] no se encontró 82574L; driver deshabilitado");
    return -ENODEV;
  }

  LOG_INFO("[E1000E] NIC en %02x:%02x.%u vendor=%04x device=%04x", g_pci.bus,
           g_pci.slot, g_pci.func, g_pci.vendor_id, g_pci.device_id);

  // 2) Habilitar memoria + bus master.
  pci_enable_io_mem(&g_pci);
  pci_enable_bus_mastering(&g_pci);

  // 3) Mapear BAR0 (128 KB).
  uint32_t bar0 = pci_get_bar(&g_pci, 0) & ~0xFu;
  if (bar0 == 0) {
    LOG_ERR("[E1000E] BAR0 inválido");
    return -EIO;
  }
  void *mmio = mmio_map(bar0, 128 * 1024, PTE_WRITABLE | PTE_NOCACHE | PTE_NX);
  if (!mmio) {
    LOG_ERR("[E1000E] mmio_map(BAR0=0x%x) falló", bar0);
    return -EIO;
  }
  g_mmio = (volatile uint32_t *)mmio;
  LOG_INFO("[E1000E] BAR0 mapeado phys=0x%x -> virt=%p", bar0, mmio);

  // 4) Reservar memoria DMA.
  g_rx_ring_phys = pmm_alloc_pages(1);
  g_tx_ring_phys = pmm_alloc_pages(1);
  g_rx_buf_base_phys = pmm_alloc_pages((DESC_COUNT * BUF_SIZE) / PAGE_SIZE);
  g_tx_buf_base_phys = pmm_alloc_pages((DESC_COUNT * BUF_SIZE) / PAGE_SIZE);
  if (!g_rx_ring_phys || !g_tx_ring_phys || !g_rx_buf_base_phys ||
      !g_tx_buf_base_phys) {
    LOG_ERR("[E1000E] no hay memoria DMA");
    return -ENOMEM;
  }
  LOG_INFO("[E1000E] DMA: rx_ring=%p tx_ring=%p rx_bufs=%p tx_bufs=%p",
           (void *)g_rx_ring_phys, (void *)g_tx_ring_phys,
           (void *)g_rx_buf_base_phys, (void *)g_tx_buf_base_phys);

  // 5) Reset + MAC.
  if (e1000e_reset() != 0)
    return -EIO;
  uint8_t mac[6];
  e1000e_read_mac(mac);

  // Intentar habilitar MSI. Si falla, seguimos con polling puro.
  uint8_t msi_cap =
      pci_find_capability(g_pci.bus, g_pci.slot, g_pci.func, 0x05);
  if (msi_cap) {
    // MSI_VECTOR_BASE ya lo usa AHCI. Usamos el siguiente.
    uint8_t vec = (uint8_t)(MSI_VECTOR_BASE + 1);
    if (pci_enable_msi(g_pci.bus, g_pci.slot, g_pci.func, vec,
                       lapic_get_bsp_id()) == 0) {
      msi_install_handler(vec, e1000e_msi_handler);
      g_use_msi = 1;
      g_msi_vector = vec;
      LOG_INFO("[E1000E] MSI habilitado en vector 0x%02x", vec);
    } else {
      LOG_WARN("[E1000E] pci_enable_msi falló, usando polling");
    }
  } else {
    LOG_INFO("[E1000E] sin capability MSI, usando polling");
  }

  // 6) Rings + enable.
  e1000e_setup_rings();
  e1000e_setup_rx();
  e1000e_setup_tx();

  // 7) Registrar la netif.
  memset(&g_nic, 0, sizeof(g_nic));
  g_nic.name[0] = 'e';
  g_nic.name[1] = 't';
  g_nic.name[2] = 'h';
  g_nic.name[3] = '0';
  g_nic.name[4] = '\0';
  memcpy(g_nic.mac, mac, 6);
  // [CONFIG] Sin IP hardcodeada. El userland configura la interfaz vía
  // ifconfig. netif_register() pone is_up = 1 por defecto.
  g_nic.ip = 0;
  g_nic.netmask = 0;
  g_nic.gateway = 0;

  memset(&g_ops, 0, sizeof(g_ops));
  g_ops.transmit = e1000e_transmit;
  g_ops.open = e1000e_open;
  g_ops.close = e1000e_close;
  g_nic.ops = &g_ops;

  netif_register(&g_nic);

  LOG_INFO(
      "[E1000E] MAC %02x:%02x:%02x:%02x:%02x:%02x ip=10.0.2.15/24 gw=10.0.2.2",
      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  g_initialized = 1;
  return 0;
}

int e1000e_start(void) {
  if (!g_initialized)
    return -ENODEV;
  if (g_started)
    return 0;
  task_t *t = sched_create_task(e1000e_rx_thread);
  if (!t) {
    LOG_ERR("[E1000E] no se pudo crear el kthread RX");
    return -ENOMEM;
  }
  t->cpu_affinity = 0;
  g_started = 1;
  return 0;
}

netif_t *e1000e_netif(void) { return g_initialized ? &g_nic : NULL; }