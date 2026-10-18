// kernel/usb/xhci/xhci.h
#ifndef KERNEL_USB_XHCI_H
#define KERNEL_USB_XHCI_H

#include "../../pci.h"
#include "../../spinlock.h"
#include "../../wait.h"
#include "../usb_core.h"
#include "../usb_dma.h"
#include <stdint.h>

#define XHCI_CAP_CAPLENGTH 0x00
#define XHCI_CAP_HCIVERSION 0x02
#define XHCI_CAP_HCSPARAMS1 0x04
#define XHCI_CAP_HCSPARAMS2 0x08
#define XHCI_CAP_HCSPARAMS3 0x0C
#define XHCI_CAP_HCCPARAMS1 0x10
#define XHCI_CAP_DBOFF 0x14
#define XHCI_CAP_RTSOFF 0x18
#define XHCI_CAP_HCCPARAMS2 0x1C

#define XHCI_HCS1_MAX_SLOTS(v) ((v) & 0xFFu)
#define XHCI_HCS1_MAX_INTRS(v) (((v) >> 8) & 0x7FFu)
#define XHCI_HCS1_MAX_PORTS(v) (((v) >> 24) & 0xFFu)

#define XHCI_HCC1_AC64 (1u << 0)
#define XHCI_HCC1_CSZ (1u << 2)
#define XHCI_HCC1_PPC (1u << 3)

#define XHCI_OP_USBCMD 0x00
#define XHCI_OP_USBSTS 0x04
#define XHCI_OP_CRCR 0x18
#define XHCI_OP_DCBAAP 0x30
#define XHCI_OP_CONFIG 0x38
#define XHCI_OP_PORTSC(n) (0x400 + ((n) - 1) * 0x10)

#define XHCI_USBCMD_RS (1u << 0)
#define XHCI_USBCMD_HCRST (1u << 1)
#define XHCI_USBCMD_INTE (1u << 2)

#define XHCI_USBSTS_HCH (1u << 0)
#define XHCI_USBSTS_EINT (1u << 3)
#define XHCI_USBSTS_PCD (1u << 4)
#define XHCI_USBSTS_CNR (1u << 11)
#define XHCI_USBSTS_HSE (1u << 2)
#define XHCI_USBSTS_HCE (1u << 12)

#define XHCI_CRCR_RCS (1u << 0)

#define XHCI_PORTSC_CCS (1u << 0)
#define XHCI_PORTSC_PED (1u << 1)
#define XHCI_PORTSC_PR (1u << 4)
#define XHCI_PORTSC_PLS_SHIFT 5
#define XHCI_PORTSC_PLS_MASK (0xFu << 5)
#define XHCI_PORTSC_PP (1u << 9)
#define XHCI_PORTSC_CSC (1u << 17)
#define XHCI_PORTSC_PEC (1u << 18)
#define XHCI_PORTSC_WRC (1u << 19)
#define XHCI_PORTSC_OCC (1u << 20)
#define XHCI_PORTSC_PRC (1u << 21)
#define XHCI_PORTSC_PLC (1u << 22)
#define XHCI_PORTSC_CEC (1u << 23)

#define XHCI_PORTSC_CHANGE_BITS                                                \
  (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC | XHCI_PORTSC_OCC |     \
   XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | XHCI_PORTSC_CEC)

#define XHCI_PLS_U0 0

typedef struct {
  uint64_t param;
  uint32_t status;
  uint32_t control;
} __attribute__((packed)) xhci_trb_t;

#define XHCI_TRB_NORMAL 1
#define XHCI_TRB_SETUP_STAGE 2
#define XHCI_TRB_DATA_STAGE 3
#define XHCI_TRB_STATUS_STAGE 4
#define XHCI_TRB_LINK 6
#define XHCI_TRB_ENABLE_SLOT 9
#define XHCI_TRB_DISABLE_SLOT 10
#define XHCI_TRB_ADDRESS_DEVICE 11
#define XHCI_TRB_CONFIGURE_EP 12
#define XHCI_TRB_EVAL_CTX 13
#define XHCI_TRB_NO_OP_CMD 23
#define XHCI_TRB_TRANSFER_EVT 32
#define XHCI_TRB_CMD_COMPLETION 33
#define XHCI_TRB_PORT_STATUS_CHG 34

#define XHCI_TRB_CYCLE (1u << 0)
#define XHCI_TRB_TC (1u << 1)
#define XHCI_TRB_ISP (1u << 2)
#define XHCI_TRB_CH (1u << 4)
#define XHCI_TRB_IOC (1u << 5)
#define XHCI_TRB_IDT (1u << 6)
#define XHCI_TRB_DIR_IN (1u << 16)
#define XHCI_TRB_TYPE_SHIFT 10
#define XHCI_TRB_TYPE(x) (((x) & 0x3Fu) << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_GET_TYPE(t) (((t).control >> XHCI_TRB_TYPE_SHIFT) & 0x3F)

#define XHCI_SETUP_TRT_NONE (0u << 16)
#define XHCI_SETUP_TRT_OUT (2u << 16)
#define XHCI_SETUP_TRT_IN (3u << 16)

#define XHCI_CC_SUCCESS 1
#define XHCI_CC_SHORT_PACKET 13

#define XHCI_EVT_CC(t) (((t).status >> 24) & 0xFFu)
#define XHCI_EVT_SLOT(t) (((t).control >> 24) & 0xFFu)
#define XHCI_EVT_EP(t) (((t).control >> 16) & 0x1Fu)

#define XHCI_ICC_ADD_SLOT (1u << 0)
#define XHCI_ICC_ADD_EP0 (1u << 1)

typedef struct {
  uint64_t base;
  uint32_t size;
  uint32_t reserved;
} __attribute__((packed)) xhci_erst_entry_t;

typedef struct {
  uint32_t drop;
  uint32_t add;
  uint32_t reserved[6];
} __attribute__((packed)) xhci_icc_t;

#define XHCI_RT_IR_OFFSET(i) (0x20u + (i) * 0x20u)
#define XHCI_RT_IR0_IMAN (XHCI_RT_IR_OFFSET(0) + 0x00u)
#define XHCI_RT_IR0_ERSTSZ (XHCI_RT_IR_OFFSET(0) + 0x08u)
#define XHCI_RT_IR0_ERSTBA (XHCI_RT_IR_OFFSET(0) + 0x10u)
#define XHCI_RT_IR0_ERDP (XHCI_RT_IR_OFFSET(0) + 0x18u)

#define XHCI_IMAN_IP (1u << 0)
#define XHCI_IMAN_IE (1u << 1)
#define XHCI_ERDP_EHB (1u << 3)

#define XHCI_SLOT_DW0_ROUTE(s) ((s) & 0xFFFFFu)
#define XHCI_SLOT_DW0_SPEED(s) (((s) & 0xFu) << 20)
#define XHCI_SLOT_DW0_HUB (1u << 26)
#define XHCI_SLOT_DW0_CTX_ENT(n) (((n) & 0x1Fu) << 27)

#define XHCI_SLOT_DW1_RH_PORT(p) (((uint32_t)(p) & 0xFFu) << 16)
#define XHCI_SLOT_DW1_NUM_PORTS(p) (((uint32_t)(p) & 0xFFu) << 24)

#define XHCI_SLOT_DW2_TT_HUB_SLOT(s) ((s) & 0xFFu)
#define XHCI_SLOT_DW2_TT_PORT(p) (((p) & 0xFFu) << 8)
#define XHCI_SLOT_DW2_TTT(t) (((t) & 0x3u) << 16)

#define XHCI_EP_DW0_LSA 0x00008000u
#define XHCI_EP_DW0_MAXPSTR(p) (((p) & 0x1Fu) * 1024u)
#define XHCI_STREAM_CTX_SCT_PRIMARY 0x00000002u
#define XHCI_UAS_STREAM_COUNT 16u
#define XHCI_UAS_MAX_PSTREAMS 3u

#define XHCI_EP_DW1_CERR(c) (((c) & 0x3u) << 1)
#define XHCI_EP_DW1_EPTYPE(t) (((t) & 0x7u) << 3)
#define XHCI_EPTYPE_BULK_OUT 2
#define XHCI_EPTYPE_INT_OUT 3
#define XHCI_EPTYPE_CONTROL 4
#define XHCI_EPTYPE_BULK_IN 6
#define XHCI_EPTYPE_INT_IN 7
#define XHCI_EP_DW1_MAX_PKT(p) (((uint32_t)(p) & 0xFFFFu) << 16)
#define XHCI_EP_DW4_AVG_TRB_LEN(l) ((l) & 0xFFFFu)
#define XHCI_EP_DEQ_DCS (1u << 0)

typedef struct xhci_ring {
  xhci_trb_t *trbs;
  uint64_t phys;
  uint32_t size;
  uint32_t enqueue;
  uint32_t dequeue;
  uint8_t cycle;
  uint8_t is_producer;
  uint8_t pad[2];
} xhci_ring_t;

#define XHCI_MAX_PENDING_CMDS 32
#define XHCI_MAX_PENDING_XFERS 64

typedef struct xhci_controller {
  pci_device_t pci;
  volatile uint8_t *mmio;
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
  uint8_t ctx_size;

  volatile uint8_t *op;
  volatile uint8_t *db;
  volatile uint8_t *rt;

  xhci_ring_t cmd;
  xhci_ring_t evt;
  xhci_erst_entry_t *erst;
  uint64_t erst_phys;

  uint64_t *dcbaa;
  uint64_t dcbaa_phys;

  int using_msi;
  uint8_t msi_vector;
  uint8_t irq;

  wait_queue_t irq_wq;
  wait_queue_t cmd_wq;
  wait_queue_t xfer_wq;
  wait_queue_t hub_wq;

  int initialized;
  spinlock_t lock;
} xhci_controller_t;

extern xhci_controller_t g_xhci;
extern usb_hcd_t g_xhci_hcd;

extern xhci_trb_t g_pending_cmds[XHCI_MAX_PENDING_CMDS];
extern int g_pending_cmd_count;
extern xhci_trb_t g_pending_xfers[XHCI_MAX_PENDING_XFERS];
extern int g_pending_xfer_count;

int xhci_take_pending_cmd(uint64_t cmd_trb_phys, xhci_trb_t *out);
int xhci_take_pending_xfer(uint8_t slot, uint8_t ep_id, uint64_t trb_phys,
                           xhci_trb_t *out);

void xhci_event_thread(void);
void xhci_hotplug_thread(void);

int xhci_probe(void);
void xhci_dump(void);
int xhci_cmd_noop(void);
int xhci_cmd_enable_slot(uint8_t *slot_id);
int xhci_cmd_disable_slot(uint8_t slot_id);
uint32_t xhci_port_status(uint8_t port);
void xhci_port_dump_all(void);
int xhci_port_reset(uint8_t port);
int xhci_attach_all_devices(void);

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

typedef struct {
  xhci_icc_t icc;
  xhci_slot_ctx_t slot;
  xhci_ep_ctx_t ep[31];
} __attribute__((packed)) xhci_input_ctx_t;

static inline int xhci_ep_ctx_index(int ep_num, int dir_in) {
  if (ep_num == 0)
    return 2;
  return 2 * ep_num + 1 + (dir_in ? 1 : 0);
}

static inline xhci_ep_ctx_t *xhci_ictx_ep(xhci_input_ctx_t *p, int ep_num,
                                          int dir_in) {
  return &p->ep[xhci_ep_ctx_index(ep_num, dir_in) - 2];
}

static inline xhci_ep_ctx_t *xhci_ictx_ep0(xhci_input_ctx_t *p) {
  return &p->ep[0];
}

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
    return USB_SPEED_SUPER_PLUS;
  default:
    return USB_SPEED_UNKNOWN;
  }
}

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
    return 512;
  default:
    return 8;
  }
}

typedef struct {
  int configured;
  uint8_t ep_num;
  uint8_t dir_in;
  uint8_t ep_type;
  uint8_t has_streams;
  uint8_t max_streams;
  xhci_ring_t ring;
  usb_dma_buf_t stream_ctx;
} xhci_ep_state_t;

typedef struct xhci_dev_priv {
  uint8_t slot_id;
  uint8_t root_hub_port;
  usb_speed_t speed;
  xhci_ring_t ep0_ring;
  xhci_ep_state_t eps[15];
  usb_dma_buf_t input_ctx;
  usb_dma_buf_t out_ctx;
} xhci_dev_priv_t;

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
    return 5;
  default:
    return 0;
  }
}

int xhci_cmd_address_device(uint8_t slot, uint8_t port, usb_speed_t speed,
                            uint16_t ep0_mps, uint64_t input_ctx_phys);
int xhci_cmd_configure_endpoint(uint8_t slot, uint64_t input_ctx_phys);
int xhci_cmd_evaluate_context(uint8_t slot, uint64_t input_ctx_phys);

int xhci_hcd_control_xfer(usb_device_t *dev, const uint8_t setup[8], void *data,
                          int data_len, int is_in, int timeout_ms);
int xhci_hcd_alloc_device(usb_device_t *dev);
void xhci_hcd_free_device(usb_device_t *dev);
int xhci_hcd_configure_ep(usb_device_t *dev, usb_endpoint_t *ep);
int xhci_hcd_interrupt_xfer(usb_device_t *dev, usb_endpoint_t *ep, void *buf,
                            int len, int timeout_ms, int *out_actual_len);
int xhci_hcd_bulk_xfer(usb_device_t *dev, usb_endpoint_t *ep, void *buf,
                       int len, int is_in, int timeout_ms);

int xhci_hcd_new_device(usb_device_t *parent_hub, uint8_t port,
                        usb_speed_t speed, usb_device_t **out_dev);
int xhci_hcd_set_hub_info(usb_device_t *dev, uint8_t num_ports);

#define XHCI_MAX_ASYNC_URBS 64

typedef struct xhci_pending_urb {
  usb_bulk_urb_t *urb;
  uint64_t trb_phys;
  uint8_t slot_id;
  uint8_t ep_id;
} xhci_pending_urb_t;

extern xhci_pending_urb_t g_async_urbs[XHCI_MAX_ASYNC_URBS];
extern int g_async_urb_count;

int xhci_hcd_bulk_submit(usb_device_t *dev, usb_endpoint_t *ep,
                         usb_bulk_urb_t *urb);

int xhci_hcd_bulk_xfer_phys(usb_device_t *dev, usb_endpoint_t *ep,
                            uint64_t phys, void *virt, int len, int is_in,
                            int timeout_ms);

int xhci_wait_transfer(uint8_t slot, uint8_t ep_id, uint64_t last_trb_phys,
                       uint32_t timeout_ms, uint32_t *out_residual_len);

#endif