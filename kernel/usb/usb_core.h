// kernel/usb/usb_core.h
#ifndef KERNEL_USB_CORE_H
#define KERNEL_USB_CORE_H

#include "../bus.h"      // [FASE 2.1c] struct bus_type, struct device
#include <stddef.h>
#include <stdint.h>

// ===========================================================================
// Tipos de descriptor (USB 2.0 spec, tabla 9-2)
// ===========================================================================
#define USB_DT_DEVICE 0x01
#define USB_DT_CONFIG 0x02
#define USB_DT_STRING 0x03
#define USB_DT_INTERFACE 0x04
#define USB_DT_ENDPOINT 0x05
#define USB_DT_DEVICE_QUALIFIER 0x06
#define USB_DT_OTHER_SPEED_CFG 0x07
#define USB_DT_INTERFACE_POWER 0x08
#define USB_DT_HID 0x21
#define USB_DT_REPORT 0x22
#define USB_DT_HUB 0x29

// ===========================================================================
// Tipos de endpoint (bmAttributes & 0x03)
// ===========================================================================
#define USB_EP_CONTROL 0
#define USB_EP_ISOCHRONOUS 1
#define USB_EP_BULK 2
#define USB_EP_INTERRUPT 3

// ===========================================================================
// Velocidades
// ===========================================================================
typedef enum {
  USB_SPEED_UNKNOWN = 0,
  USB_SPEED_LOW = 1,
  USB_SPEED_FULL = 2,
  USB_SPEED_HIGH = 3,
  USB_SPEED_SUPER = 4,
  USB_SPEED_SUPER_PLUS = 5,
} usb_speed_t;

// ===========================================================================
// Modelo de dispositivo
// ===========================================================================
typedef struct usb_device usb_device_t;
typedef struct usb_hcd usb_hcd_t;
typedef struct usb_bulk_urb usb_bulk_urb_t;

typedef struct usb_endpoint {
  uint8_t addr;
  uint8_t type;
  uint16_t max_packet;
  uint8_t interval;
  uint16_t stream_id;
  void *hc_ep;
} usb_endpoint_t;

typedef struct usb_interface {
  uint8_t number;
  uint8_t alt_setting;
  uint8_t class;
  uint8_t subclass;
  uint8_t protocol;
  usb_endpoint_t *endpoints;
  int num_endpoints;
} usb_interface_t;

typedef struct usb_config {
  uint8_t value;
  uint8_t num_interfaces;
  uint8_t attributes;
  uint8_t max_power;
  usb_interface_t *interfaces;
  int num_interface_structs;
} usb_config_t;

struct usb_device {
  uint8_t addr;
  usb_speed_t speed;
  uint16_t vendor_id;
  uint16_t product_id;
  uint16_t bcd_usb;
  uint16_t bcd_device;
  uint8_t class, subclass, protocol;
  uint8_t max_packet_size_ep0;
  uint8_t num_configurations;

  uint8_t i_manufacturer, i_product, i_serial;

  char manufacturer[64];
  char product[64];
  char serial[64];

  usb_device_t *parent_hub;
  uint8_t port_on_parent;

  usb_config_t *configs[8];
  uint8_t active_config;

  usb_hcd_t *hcd;
  void *hc_priv;

  // [FASE 2.1c] Nodo del bus USB asociado (NULL si no se registró en bus).
  // Lo pone usb_device_register() y lo limpia usb_device_unregister().
  struct device *dev_node;
};

// ===========================================================================
// Interfaz HCD (Host Controller Driver).
// ===========================================================================
struct usb_hcd {
  const char *name;
  void *priv;

  int (*control_xfer)(usb_device_t *dev, const uint8_t setup[8], void *data,
                      int data_len, int is_in, int timeout_ms);
  int (*bulk_xfer)(usb_device_t *dev, usb_endpoint_t *ep, void *buf, int len,
                   int is_in, int timeout_ms);
  int (*interrupt_xfer)(usb_device_t *dev, usb_endpoint_t *ep, void *buf,
                        int len, int timeout_ms, int *out_actual_len);
  int (*configure_ep)(usb_device_t *dev, usb_endpoint_t *ep);
  int (*new_device)(usb_device_t *parent_hub, uint8_t port, usb_speed_t speed,
                    usb_device_t **out_dev);
  int (*set_hub_info)(usb_device_t *dev, uint8_t num_ports);

  int (*bulk_submit)(usb_device_t *dev, usb_endpoint_t *ep,
                     struct usb_bulk_urb *urb);

  int (*alloc_device)(usb_device_t *dev);
  void (*free_device)(usb_device_t *dev);

  int (*bulk_xfer_phys)(usb_device_t *dev, usb_endpoint_t *ep, uint64_t phys,
                        void *virt, int len, int is_in, int timeout_ms);
};

// Devuelve el número de bytes transferidos en éxito, o negativo en error.
int usb_control_transfer(usb_device_t *dev, uint8_t request_type,
                         uint8_t request, uint16_t value, uint16_t index,
                         void *data, uint16_t len, int timeout_ms);

// ===========================================================================
// Parsers puros (sin I/O).
// ===========================================================================
int usb_parse_device_descriptor(const uint8_t *raw, usb_device_t *dev_out);
usb_config_t *usb_parse_config_descriptor(const uint8_t *raw, int raw_len);
void usb_config_free(usb_config_t *cfg);

// ===========================================================================
// Helpers de control transfer.
// ===========================================================================
int usb_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t index,
                       uint16_t langid, void *buf, uint16_t len);
int usb_set_configuration(usb_device_t *dev, uint8_t config_value);
int usb_get_string_descriptor(usb_device_t *dev, uint8_t index, uint16_t langid,
                              char *out, size_t outlen);

// ===========================================================================
// Ciclo de vida + enumeración genérica.
// ===========================================================================
usb_device_t *usb_device_alloc(void);
void usb_device_free(usb_device_t *dev);

int usb_enumerate(usb_device_t *dev);
int usb_core_self_test(void);
uint32_t usb_device_route(const usb_device_t *dev);

struct usb_bulk_urb {
  usb_device_t *dev;
  usb_endpoint_t *ep;
  void *buf;
  int len;
  int is_in;
  int status;
  int actual_len;
  uint16_t stream_id;
  void (*complete)(struct usb_bulk_urb *urb);
  void *context;
  void *hc_priv;
  struct usb_dma_buf *dma_buf;
};

// ===========================================================================
// [FASE 2.1c] Bus USB genérico.
//
// usb_bus ata los drivers USB al framework genérico de bus.c, igual que
// pci_bus. La única diferencia con PCI: los IDs se comparan contra el
// device descriptor (vendor/product/clase) Y, si match_flags incluye
// los bits INT_*, contra alguna interface del device (class/subclass/
// protocol por interface).
//
// Uso:
//   static const usb_device_id_t usb_hid_id_table[] = {
//     { .match_flags = USB_DEVICE_ID_MATCH_INT_CLASS,
//       .b_interface_class = 0x03 },
//     {0}
//   };
//   struct driver usb_hid_driver = {
//     .name     = "usbhid",
//     .bus      = &usb_bus,
//     .id_table = usb_hid_id_table,
//     .probe    = hid_probe_generic,
//   };
//   driver_register(&usb_hid_driver);
// ===========================================================================

#define USB_DEVICE_ID_MATCH_VENDOR       0x0001
#define USB_DEVICE_ID_MATCH_PRODUCT      0x0002
#define USB_DEVICE_ID_MATCH_DEV_CLASS    0x0004
#define USB_DEVICE_ID_MATCH_DEV_SUBCLASS 0x0008
#define USB_DEVICE_ID_MATCH_DEV_PROTOCOL 0x0010
#define USB_DEVICE_ID_MATCH_INT_CLASS    0x0020
#define USB_DEVICE_ID_MATCH_INT_SUBCLASS 0x0040
#define USB_DEVICE_ID_MATCH_INT_PROTOCOL 0x0080

typedef struct {
  uint16_t match_flags;  // combinación de USB_DEVICE_ID_MATCH_*
  uint16_t id_vendor;
  uint16_t id_product;
  uint8_t  b_device_class;
  uint8_t  b_device_subclass;
  uint8_t  b_device_protocol;
  uint8_t  b_interface_class;
  uint8_t  b_interface_subclass;
  uint8_t  b_interface_protocol;
} usb_device_id_t;

extern struct bus_type usb_bus;

// Registra el device en el bus USB. Si algún driver ya registrado matchea,
// llama a su probe. Devuelve 0 si OK, -errno si no hay slot libre.
int usb_device_register(usb_device_t *udev);
void usb_device_unregister(usb_device_t *udev);

// [FASE 2.1c] Inicializa el bus USB (bus_register + driver_register de
// usb_hid / usb_storage / usb_hub). Idempotente.
//
// NO confundir con usb_init() de usb.h, que inicializa el subsistema
// USB core (DMA pool, URB self-tests). Aquí solo registramos el bus y
// sus drivers en bus.c.
int usb_bus_init(void);
#endif