// kernel/usb/xhci/xhci.h
#ifndef KERNEL_USB_XHCI_H
#define KERNEL_USB_XHCI_H

#include "../../pci.h"
#include "../../spinlock.h"
#include "../usb_core.h"
#include "../usb_dma.h"
#include <stdint.h>

// ===========================================================================
// Registros del HBA (offset desde MMIO base)
// ===========================================================================
#define XHCI_CAP_CAPLENGTH 0x00
#define XHCI_CAP_HCIVERSION 0x02
#define XHCI_CAP_HCSPARAMS1 0x04
#define XHCI_CAP_HCSPARAMS2 0x08
#define XHCI_CAP_HCSPARAMS3 0x0C
#define XHCI_CAP_HCCPARAMS1 0x10
#define XHCI_CAP_DBOFF 0x14
#define XHCI_CAP_RTSOFF 0x18
#define XHCI_CAP_HCCPARAMS2 0x1C

// HCSPARAMS1 campos
#define XHCI_HCS1_MAX_SLOTS(v) ((v) & 0xFFu)
#define XHCI_HCS1_MAX_INTRS(v) (((v) >> 8) & 0x7FFu)
#define XHCI_HCS1_MAX_PORTS(v) (((v) >> 24) & 0xFFu)

// HCCPARAMS1 campos
#define XHCI_HCC1_AC64 (1u << 0) // 64-bit addressing
#define XHCI_HCC1_BNC (1u << 1)  // BW negotiation
#define XHCI_HCC1_CSZ (1u << 2)  // Context size (1=64B, 0=32B)
#define XHCI_HCC1_PPC (1u << 3)  // Port power control
#define XHCI_HCC1_PIND (1u << 4) // Port indicators
#define XHCI_HCC1_LTC (1u << 5)  // Latency tolerance
#define XHCI_HCC1_NSS (1u << 6)  // No secondary SID
#define XHCI_HCC1_PAE (1u << 7)  // Parse all event data
#define XHCI_HCC1_SPC (1u << 8)  // Short packet cap
#define XHCI_HCC1_SEC (1u << 9)  // Security cap
#define XHCI_HCC1_CFC (1u << 10) // Config cache
#define XHCI_HCC1_MAX_PSA(v) (((v) >> 12) & 0xFu)
#define XHCI_HCC1_XECP(v) (((v) >> 16) & 0xFFFFu)

// Registros operacionales (offset desde op_base)
#define XHCI_OP_USBCMD 0x00
#define XHCI_OP_USBSTS 0x04
#define XHCI_OP_PAGESIZE 0x08
#define XHCI_OP_DNCTRL 0x0C
#define XHCI_OP_CRCR 0x18   // 64-bit: 0x18 (low), 0x1C (high)
#define XHCI_OP_DCBAAP 0x30 // 64-bit
#define XHCI_OP_CONFIG 0x38
// `n` es el número de puerto 1-based (Port ID). El puerto 1 está en 0x400.
#define XHCI_OP_PORTSC(n) (0x400 + ((n) - 1) * 0x10)
#define XHCI_OP_PORTPMSC(n) (0x404 + ((n) - 1) * 0x10)
#define XHCI_OP_PORTLI(n) (0x408 + ((n) - 1) * 0x10)

// USBCMD bits
#define XHCI_USBCMD_RS (1u << 0)    // Run/Stop
#define XHCI_USBCMD_HCRST (1u << 1) // HC Reset
#define XHCI_USBCMD_INTE (1u << 2)  // Interrupter Enable
#define XHCI_USBCMD_HSEE (1u << 3)  // Host System Error Enable
#define XHCI_USBCMD_EWE (1u << 10)  // Event Wrap Enable

// USBSTS bits
#define XHCI_USBSTS_HCH (1u << 0)  // HC Halted
#define XHCI_USBSTS_HSE (1u << 2)  // Host System Error
#define XHCI_USBSTS_EINT (1u << 3) // Event Interrupt
#define XHCI_USBSTS_PCD (1u << 4)  // Port Change Detect
#define XHCI_USBSTS_CNR (1u << 11) // Controller Not Ready
#define XHCI_USBSTS_HCE (1u << 12) // Host Controller Error

// CRCR bits (Ring Cycle State, etc.)
#define XHCI_CRCR_RCS (1u << 0) // Ring Cycle State
#define XHCI_CRCR_CS (1u << 1)  // Command Stop
#define XHCI_CRCR_CA (1u << 2)  // Command Abort
#define XHCI_CRCR_CRR (1u << 3) // Command Ring Running

// PORTSC bits
#define XHCI_PORTSC_CCS (1u << 0) // Current Connect Status
#define XHCI_PORTSC_PED (1u << 1) // Port Enabled/Disabled
#define XHCI_PORTSC_OCA (1u << 3) // Over-current Active
#define XHCI_PORTSC_PR (1u << 4)  // Port Reset (RW1S)
#define XHCI_PORTSC_PLS_SHIFT 5
#define XHCI_PORTSC_PLS_MASK (0xFu << 5)
#define XHCI_PORTSC_PP (1u << 9) // Port Power
#define XHCI_PORTSC_SPEED_SHIFT 10
#define XHCI_PORTSC_SPEED_MASK (0xFu << 10)
#define XHCI_PORTSC_LWS (1u << 16) // Link State Write Strobe
#define XHCI_PORTSC_CSC (1u << 17) // Connect Status Change (RW1C)
#define XHCI_PORTSC_PEC (1u << 18) // Port Enabled Change (RW1C)
#define XHCI_PORTSC_WRC (1u << 19) // Warm Reset Change
#define XHCI_PORTSC_OCC (1u << 20) // Over-current Change
#define XHCI_PORTSC_PRC (1u << 21) // Port Reset Change (RW1C)
#define XHCI_PORTSC_PLC (1u << 22) // Port Link State Change
#define XHCI_PORTSC_CEC (1u << 23) // Config Error Change
#define XHCI_PORTSC_CAS (1u << 24) // Cold Attach Status (RO)
#define XHCI_PORTSC_WCE (1u << 25)
#define XHCI_PORTSC_WDE (1u << 26)
#define XHCI_PORTSC_WOE (1u << 27)
#define XHCI_PORTSC_DR (1u << 30)
#define XHCI_PORTSC_WPR (1u << 31)

#define XHCI_PORTSC_CHANGE_BITS                                                \
  (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC | XHCI_PORTSC_OCC |     \
   XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | XHCI_PORTSC_CEC)

// PLS (Port Link State)
#define XHCI_PLS_U0 0 // Link up / enabled
#define XHCI_PLS_U3 3 // Suspended
#define XHCI_PLS_RX_DETECT 5
#define XHCI_PLS_INACTIVE 6
#define XHCI_PLS_POLLING 7
#define XHCI_PLS_RECOVERY 8
#define XHCI_PLS_HOT_RESET 9
#define XHCI_PLS_COMPLIANCE 10

// Speed codes en PORTSC bits 13:10
#define XHCI_SPEED_FULL 1
#define XHCI_SPEED_LOW 2
#define XHCI_SPEED_HIGH 3
#define XHCI_SPEED_SUPER 4
#define XHCI_SPEED_SUPER_PLUS 5

// ===========================================================================
// TRBs
// ===========================================================================
typedef struct {
  uint64_t param;   // TRB-type-specific
  uint32_t status;  // TRB-type-specific
  uint32_t control; // Cycle + type + flags
} __attribute__((packed)) xhci_trb_t;

_Static_assert(sizeof(xhci_trb_t) == 16, "xhci_trb_t debe ser 16 bytes");

// TRB types
#define XHCI_TRB_NORMAL 1
#define XHCI_TRB_SETUP_STAGE 2
#define XHCI_TRB_DATA_STAGE 3
#define XHCI_TRB_STATUS_STAGE 4
#define XHCI_TRB_LINK 6 // OJO: 5 es Isoch. Link = 6 (xHCI 1.2 tabla 6-91)
#define XHCI_TRB_ENABLE_SLOT 9
#define XHCI_TRB_DISABLE_SLOT 10
#define XHCI_TRB_ADDRESS_DEVICE 11
#define XHCI_TRB_CONFIGURE_EP 12
#define XHCI_TRB_EVAL_CTX 13
#define XHCI_TRB_RESET_EP 14
#define XHCI_TRB_STOP_EP 15
#define XHCI_TRB_SET_TR_DEQ 16
#define XHCI_TRB_RESET_DEVICE 17
#define XHCI_TRB_NO_OP_CMD 23
#define XHCI_TRB_TRANSFER_EVT 32
#define XHCI_TRB_CMD_COMPLETION 33
#define XHCI_TRB_PORT_STATUS_CHG 34
#define XHCI_TRB_DOORBELL_EVT 36

// Control bits (DWord 3)
#define XHCI_TRB_CYCLE (1u << 0)
#define XHCI_TRB_TC (1u << 1)      // Toggle Cycle (Link TRB)
#define XHCI_TRB_ENT (1u << 1)     // Evaluate Next TRB (Data/Normal)
#define XHCI_TRB_ISP (1u << 2)     // Interrupt on Short Packet
#define XHCI_TRB_NS (1u << 3)      // No Snoop
#define XHCI_TRB_CH (1u << 4)      // Chain
#define XHCI_TRB_IOC (1u << 5)     // Interrupt on Completion
#define XHCI_TRB_IDT (1u << 6)     // Immediate Data
#define XHCI_TRB_DIR_IN (1u << 16) // Data/Setup/Status Direction
#define XHCI_TRB_TYPE_SHIFT 10
#define XHCI_TRB_TYPE_MASK (0x3Fu << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_TYPE(x) (((x) & 0x3Fu) << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_GET_TYPE(t) (((t).control >> XHCI_TRB_TYPE_SHIFT) & 0x3F)

// Setup stage TRT (bits 31:24)
// Setup Stage TRB — Status (DWord 2)
#define XHCI_SETUP_TRT_NONE (0u << 16)
#define XHCI_SETUP_TRT_OUT (2u << 16)
#define XHCI_SETUP_TRT_IN (3u << 16)

// Completion codes
#define XHCI_CC_SUCCESS 1
#define XHCI_CC_SHORT_PACKET 13
#define XHCI_CC_TRB_ERROR 5
#define XHCI_CC_STALL_ERROR 6
#define XHCI_CC_SLOT_NOT_ENABLED 11

// Extracción de campos de eventos
#define XHCI_EVT_CC(t) (((t).status >> 24) & 0xFFu)
#define XHCI_EVT_SLOT(t) (((t).control >> 24) & 0xFFu)
#define XHCI_EVT_EP(t) (((t).control >> 16) & 0x1Fu)

// ICC add/drop bits (bit 0 = slot, bit 1 = ep0, ...)
#define XHCI_ICC_ADD_SLOT (1u << 0)
#define XHCI_ICC_ADD_EP0 (1u << 1)
#define XHCI_ICC_DROP_EP0 (1u << 1)

// ===========================================================================
// Macros que faltaban (referenciadas por xhci_hcd_configure_ep).
// Semánticamente spec-correct para xHCI 1.0/1.1/1.2.
// ===========================================================================

// Slot Context, DWord 2, bits 15:8 — USB Device Address.
#define XHCI_SLOT_DW2_USB_ADDR(a) (((a) & 0xFFu) << 8)

// Endpoint Context, DWord 0, bits 31:24 — Error Count (CErr).
// (xHCI 1.0/1.1/1.2 lo ponen aquí; el `XHCI_EP_DW1_CERR` que ya tienes
// es un remanente, no lo usa nadie en el código activo.)
#define XHCI_EP_DW0_CERR(c) (((c) & 0xFFu) << 24)

// ===========================================================================
// ERST (Event Ring Segment Table)
// ===========================================================================
typedef struct {
  uint64_t base; // Ring Segment Base Address (64-bit phys)
  uint32_t size; // Ring Segment Size (TRBs)
  uint32_t reserved;
} __attribute__((packed)) xhci_erst_entry_t;

_Static_assert(sizeof(xhci_erst_entry_t) == 16, "ERST entry != 16 bytes");

// ===========================================================================
// Contextos (32 bytes cada uno)
// ===========================================================================
typedef struct {
  uint32_t dw0;
  uint32_t dw1;
  uint32_t dw2;
  uint32_t dw3;
} __attribute__((packed)) xhci_ctx32_t;

_Static_assert(sizeof(xhci_ctx32_t) == 16, "xhci_ctx32_t != 16 bytes");

// Input Control Context (ICC)
typedef struct {
  uint32_t drop;
  uint32_t add;
  uint32_t reserved[6];
} __attribute__((packed)) xhci_icc_t;

_Static_assert(sizeof(xhci_icc_t) == 32, "ICC != 32 bytes");

// ===========================================================================
// Interrupter 0 (Runtime registers). El offset base es RTSOFF. Cada
// interrupter ocupa 0x20 bytes; los registros "en crudo" del primer
// interrupter empiezan en RTSOFF + 0x20.
//
// Offsets relativos al inicio del interrupter (i.e. a
// g_xhci.rt + 0x20 + i * 0x20). Para i=0 se simplifica a g_xhci.rt + 0x20.
// ===========================================================================
#define XHCI_RT_IR_OFFSET(i) (0x20u + (i) * 0x20u)

// Registros dentro de un interrupter (offsets relativos al interrupter).
#define XHCI_IR_IMAN 0x00u   // Interrupter Management
#define XHCI_IR_IMOD 0x04u   // Interrupter Moderation
#define XHCI_IR_ERSTSZ 0x08u // Event Ring Segment Table Size
#define XHCI_IR_RSVD 0x0Cu
#define XHCI_IR_ERSTBA 0x10u // ERST Base Address (64-bit)
#define XHCI_IR_ERDP 0x18u   // Event Ring Dequeue Pointer (64-bit)

// Shortcuts para el interrupter 0 (offsets relativos a g_xhci.rt).
#define XHCI_RT_IR0_IMAN (XHCI_RT_IR_OFFSET(0) + XHCI_IR_IMAN)
#define XHCI_RT_IR0_IMOD (XHCI_RT_IR_OFFSET(0) + XHCI_IR_IMOD)
#define XHCI_RT_IR0_ERSTSZ (XHCI_RT_IR_OFFSET(0) + XHCI_IR_ERSTSZ)
#define XHCI_RT_IR0_ERSTBA (XHCI_RT_IR_OFFSET(0) + XHCI_IR_ERSTBA)
#define XHCI_RT_IR0_ERDP (XHCI_RT_IR_OFFSET(0) + XHCI_IR_ERDP)

// IMAN bits
#define XHCI_IMAN_IP (1u << 0) // Interrupt Pending (RW1C)
#define XHCI_IMAN_IE (1u << 1) // Interrupt Enable

// ERDP bits
#define XHCI_ERDP_EHB (1u << 3) // Event Handler Busy (RW1C)

// ---------------------------------------------------------------------------
// Slot Context (xHCI 1.2 §6.2.2)
//   DW0: Route String [19:0] | Speed [23:20] | MTT [25] | Hub [26] |
//        Context Entries [31:27]
//   DW1: Max Exit Latency [15:0] | Root Hub Port Number [23:16] |
//        Number of Ports [31:24]
//   DW2: TT Hub Slot ID [7:0] | TT Port Number [15:8] | TTT [17:16] |
//        Interrupter Target [31:22]
//   DW3: USB Device Address [7:0] | Slot State [31:27]  (los escribe el HC)
// ---------------------------------------------------------------------------
#define XHCI_SLOT_DW0_ROUTE(s) ((s) & 0xFFFFFu)
#define XHCI_SLOT_DW0_SPEED(s) (((s) & 0xFu) << 20)
#define XHCI_SLOT_DW0_MTT (1u << 25)
#define XHCI_SLOT_DW0_HUB (1u << 26)
#define XHCI_SLOT_DW0_CTX_ENT(n) (((n) & 0x1Fu) << 27)

#define XHCI_SLOT_DW1_MAX_EXIT_LAT(l) ((l) & 0xFFFFu)
#define XHCI_SLOT_DW1_RH_PORT(p) (((uint32_t)(p) & 0xFFu) << 16)
#define XHCI_SLOT_DW1_NUM_PORTS(p) (((uint32_t)(p) & 0xFFu) << 24)

#define XHCI_SLOT_DW2_TT_HUB_SLOT(s) ((s) & 0xFFu)
#define XHCI_SLOT_DW2_TT_PORT(p) (((p) & 0xFFu) << 8)
#define XHCI_SLOT_DW2_TTT(t) (((t) & 0x3u) << 16)
#define XHCI_SLOT_DW2_INTR_TGT(t) (((t) & 0x3FFu) << 22)

// ---------------------------------------------------------------------------
// Endpoint Context (xHCI 1.2 §6.2.3)
//   DW0: EP State [2:0] | Mult [9:8] | MaxPStreams [14:10] | LSA [15] |
//        Interval [23:16] | Max ESIT Payload Hi [31:24]
//   DW1: CErr [2:1] | EP Type [5:3] | HID [7] | Max Burst [15:8] |
//        Max Packet Size [31:16]
//   DW2/DW3: TR Dequeue Pointer (bit 0 = DCS)
//   DW4: Average TRB Length [15:0] | Max ESIT Payload Lo [31:16]
// ---------------------------------------------------------------------------
#define XHCI_EP_DW0_STATE(s) ((s) & 0x7u)
#define XHCI_EP_STATE_DISABLED 0
#define XHCI_EP_STATE_RUNNING 1
#define XHCI_EP_STATE_HALTED 2
#define XHCI_EP_STATE_STOPPED 3
#define XHCI_EP_STATE_ERROR 4

#define XHCI_EP_DW0_MULT(m) (((m) & 0x3u) << 8)
#define XHCI_EP_DW0_MAXPSTR(p) (((p) & 0x1Fu) << 10)
#define XHCI_EP_DW0_LSA (1u << 15)
#define XHCI_EP_DW0_INTERVAL(i) (((i) & 0xFFu) << 16)

#define XHCI_EP_DW1_CERR(c) (((c) & 0x3u) << 1)
#define XHCI_EP_DW1_EPTYPE(t) (((t) & 0x7u) << 3)
#define XHCI_EP_DW1_HID (1u << 7)
#define XHCI_EPTYPE_ISOC_OUT 1
#define XHCI_EPTYPE_BULK_OUT 2
#define XHCI_EPTYPE_INT_OUT 3
#define XHCI_EPTYPE_CONTROL 4
#define XHCI_EPTYPE_ISOC_IN 5
#define XHCI_EPTYPE_BULK_IN 6
#define XHCI_EPTYPE_INT_IN 7

#define XHCI_EP_DW1_MAX_BURST(b) (((b) & 0xFFu) << 8)
#define XHCI_EP_DW1_MAX_PKT(p) (((uint32_t)(p) & 0xFFFFu) << 16)

#define XHCI_EP_DW4_AVG_TRB_LEN(l) ((l) & 0xFFFFu)

#define XHCI_EP_DEQ_DCS (1u << 0)

// ===========================================================================
// Rings
// ===========================================================================
typedef struct xhci_ring {
  xhci_trb_t *trbs;    // VA del anillo (via phys_to_virt)
  uint64_t phys;       // PA del primer TRB
  uint32_t size;       // número de TRBs (potencia de 2)
  uint32_t enqueue;    // índice producer
  uint32_t dequeue;    // índice consumer (solo event ring)
  uint8_t cycle;       // cycle bit actual del producer/consumer
  uint8_t is_producer; // 1 = cmd/xfer ring; 0 = event ring
  uint8_t pad[2];
} xhci_ring_t;

// ===========================================================================
// Estado del controlador
// ===========================================================================
typedef struct xhci_controller {
  pci_device_t pci;
  volatile uint8_t *mmio; // MMIO base
  uint64_t mmio_phys;
  uint32_t mmio_size;

  uint8_t caplen;
  uint16_t hciversion;
  uint32_t hcsparams1;
  uint32_t hcsparams2;
  uint32_t hcsparams3;
  uint32_t hccparams1;
  uint32_t dboff;
  uint32_t rtsoff;

  uint8_t max_slots;
  uint8_t max_ports;
  uint8_t ctx_size; // 32 o 64 bytes por contexto

  volatile uint8_t *op; // operational registers
  volatile uint8_t *db; // doorbell array
  volatile uint8_t *rt; // runtime (interrupters)

  // Command ring
  xhci_ring_t cmd;
  // Event ring
  xhci_ring_t evt;
  // ERST (una sola entrada para un solo segmento)
  xhci_erst_entry_t *erst;
  uint64_t erst_phys;

  // DCBAA (array de punteros a device contexts)
  uint64_t *dcbaa;
  uint64_t dcbaa_phys;

  // Interrupción
  int using_msi;
  uint8_t irq;

  int initialized;
  spinlock_t lock;
} xhci_controller_t;

extern xhci_controller_t g_xhci;
extern usb_hcd_t g_xhci_hcd;

// ===========================================================================
// API (implementación en xhci.c, xhci_cmd.c, xhci_port.c, xhci_control.c)
// ===========================================================================
int xhci_probe(void);
void xhci_dump(void);

// ===========================================================================
// Comandos (xhci_cmd.c)
// ===========================================================================
int xhci_cmd_noop(void);
int xhci_cmd_enable_slot(uint8_t *slot_id);
int xhci_cmd_disable_slot(uint8_t slot_id);

// ===========================================================================
// Puertos (xhci_port.c)
// ===========================================================================
uint32_t xhci_port_status(uint8_t port); // port es 1..max_ports
void xhci_port_dump_all(void);

// ===========================================================================
// Input Context / Device Context helpers (Fase 3)
// ===========================================================================

typedef struct {
  uint32_t dw0;
  uint32_t dw1;
  uint32_t dw2;
  uint32_t dw3;
  uint32_t reserved[4];
} __attribute__((packed)) xhci_slot_ctx_t;

typedef struct {
  uint32_t dw0;
  uint32_t dw1;
  uint32_t dw2;
  uint32_t dw3;
  uint32_t reserved[4];
} __attribute__((packed)) xhci_ep_ctx_t;

// ===========================================================================
// Input/Output Device Context (spec xHCI 1.2 §6.2.1).
//
// 33 contextos de 32 B (CSZ=0):
//   [0]  ICC
//   [1]  Slot
//   [2]  EP0
//   [3]  EP1 OUT    [4]  EP1 IN
//   [5]  EP2 OUT    [6]  EP2 IN
//   ...
//   [31] EP15 OUT   [32] EP15 IN
// ===========================================================================
typedef struct {
  xhci_icc_t icc;       // 32 B
  xhci_slot_ctx_t slot; // 32 B
  xhci_ep_ctx_t ep[31]; // 31 * 32 B = 992 B
} __attribute__((packed)) xhci_input_ctx_t;

_Static_assert(sizeof(xhci_input_ctx_t) == 33 * 32,
               "input_ctx debe ser 1056B (33 contextos de 32B)");

// Índice del contexto dentro del array (0..32).
static inline int xhci_ep_ctx_index(int ep_num, int dir_in) {
  if (ep_num == 0)
    return 2;                               // EP0
  return 2 * ep_num + 1 + (dir_in ? 1 : 0); // 2N+1=OUT, 2N+2=IN
}

// Accesor: devuelve el ep_ctx para EPN, dir.
// ep[0] = EP0 (contexto 2), ep[1] = EP1 OUT (contexto 3), ...
static inline xhci_ep_ctx_t *xhci_ictx_ep(xhci_input_ctx_t *p, int ep_num,
                                          int dir_in) {
  return &p->ep[xhci_ep_ctx_index(ep_num, dir_in) - 2];
}
static inline xhci_ep_ctx_t *xhci_ictx_ep0(xhci_input_ctx_t *p) {
  return &p->ep[0];
}

// Bits de Add/Drop Context (spec xHCI 1.2 §6.2.3.1).
//   bit 0: Slot Context
//   bit 1: EP0
//   bit 2N: EPN OUT   bit 2N+1: EPN IN    (N >= 1)
static inline uint32_t xhci_icc_bit_ep(int ep_num, int dir_in) {
  if (ep_num == 0)
    return XHCI_ICC_ADD_EP0;
  return 1u << (2 * ep_num + (dir_in ? 1 : 0));
}

// SPEED de xHCI → usb_speed_t
static inline usb_speed_t xhci_speed_to_usb(uint32_t portsc) {
  switch ((portsc >> 10) & 0xF) {
  case 1:
    return USB_SPEED_FULL;
  case 2:
    return USB_SPEED_LOW;
  case 3:
    return USB_SPEED_HIGH;
  case 4:
    return USB_SPEED_SUPER;
  case 5:
    return USB_SPEED_SUPER_PLUS; // <-- NUEVO
  default:
    return USB_SPEED_UNKNOWN;
  }
}

// Devuelve el bMaxPacketSize0 inicial para un device de una speed dada.
static inline uint16_t xhci_default_ep0_mps(usb_speed_t s) {
  switch (s) {
  case USB_SPEED_LOW:
    return 8;
  case USB_SPEED_FULL:
    return 8;
  case USB_SPEED_HIGH:
    return 64;
  case USB_SPEED_SUPER:
    return 512;
  case USB_SPEED_SUPER_PLUS:
    return 512; // <-- NUEVO
  default:
    return 8;
  }
}

// ===========================================================================
// Comandos extendidos (Fase 3)
// ===========================================================================
// `slot` es el slot_id. `port` es el puerto del root hub (1..max_ports).
// `speed` es usb_speed_t del device. `ep0_mps` es bMaxPacketSize0.
// `input_ctx_phys` es la dirección física del input context ya relleno.
int xhci_cmd_address_device(uint8_t slot, uint8_t port, usb_speed_t speed,
                            uint16_t ep0_mps, uint64_t input_ctx_phys);

// ===========================================================================
// Port reset + attach (Fase 3)
// ===========================================================================
int xhci_port_reset(uint8_t port);
int xhci_attach_all_devices(void);

// ===========================================================================
// HCD ops (Fase 3). Expuestas para que `xhci.c` rellene usb_hcd_t.
// ===========================================================================
int xhci_hcd_control_xfer(usb_device_t *dev, const uint8_t setup[8], void *data,
                          int data_len, int is_in, int timeout_ms);
int xhci_hcd_alloc_device(usb_device_t *dev);
void xhci_hcd_free_device(usb_device_t *dev);

// ===========================================================================
// Estado privado por device (asignado por alloc_device).
// ===========================================================================
// Estado de un endpoint no-EP0 configurado en este device.
typedef struct {
  int configured;
  uint8_t ep_num;   // 1..15
  uint8_t dir_in;   // 1 = IN, 0 = OUT
  uint8_t ep_type;  // XHCI_EPTYPE_*
  xhci_ring_t ring; // transfer ring del endpoint
} xhci_ep_state_t;

typedef struct xhci_dev_priv {
  uint8_t slot_id;
  uint8_t root_hub_port;
  usb_speed_t speed;

  xhci_ring_t ep0_ring;
  xhci_ep_state_t eps[15]; // índice ep_num-1

  usb_dma_buf_t input_ctx; // 33 * ctx_size bytes
  usb_dma_buf_t out_ctx;   // 33 * ctx_size bytes
  uint32_t transfer_pending;
} xhci_dev_priv_t;

// ===========================================================================
// Conversión usb_speed_t → código de Speed del Slot Context (xHCI §6.2.2).
//
// OJO: los valores del enum usb_speed_t NO coinciden con los códigos
// del slot context para FS/LS. xHCI spec Tabla 6-5:
//   1 = Full Speed
//   2 = Low Speed
//   3 = High Speed
//   4 = SuperSpeed
//   5 = SuperSpeedPlus
//
// Mientras que usb_speed_t es:
//   LOW = 1, FULL = 2, HIGH = 3, SUPER = 4
//
// Coinciden HIGH (3) y SUPER (4), pero NO LOW ni FULL.
// ===========================================================================
static inline uint32_t usb_speed_to_xhci_slot_code(usb_speed_t s) {
  switch (s) {
  case USB_SPEED_FULL:
    return 1;
  case USB_SPEED_LOW:
    return 2;
  case USB_SPEED_HIGH:
    return 3;
  case USB_SPEED_SUPER:
    return 4;
  case USB_SPEED_SUPER_PLUS:
    return 5; // <-- NUEVO (xHCI spec 1.2: 5 = SS+)
  default:
    return 0;
  }
}

// ===========================================================================
// Configure Endpoint (Fase 2)
// ===========================================================================
int xhci_cmd_configure_endpoint(uint8_t slot, uint64_t input_ctx_phys);
int xhci_cmd_evaluate_context(uint8_t slot, uint64_t input_ctx_phys);

// Configura un endpoint no-EP0 (interrupt/bulk) en `dev`. Tras esta
// llamada, `ep->hc_ep` apunta a un xhci_ep_state_t con el ring listo.
// El endpoint debe venir de un usb_interface_t ya parseado.
int xhci_hcd_configure_ep(usb_device_t *dev, usb_endpoint_t *ep);

// Interrupt/bulk transfer síncrono (bloqueante).
// `timeout_ms == 0` significa "infinito" (útil para HID: dormimos
// hasta que llegue un report).
// `out_actual_len` recibe los bytes transferidos.
int xhci_hcd_interrupt_xfer(usb_device_t *dev, usb_endpoint_t *ep, void *buf,
                            int len, int timeout_ms, int *out_actual_len);

int xhci_hcd_bulk_xfer(usb_device_t *dev, usb_endpoint_t *ep, void *buf,
                       int len, int is_in, int timeout_ms);

int xhci_hcd_new_device(usb_device_t *parent_hub, uint8_t port,
                        usb_speed_t speed, usb_device_t **out_dev);

#endif