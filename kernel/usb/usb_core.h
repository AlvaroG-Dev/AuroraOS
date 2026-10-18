// kernel/usb/usb_core.h
#ifndef KERNEL_USB_CORE_H
#define KERNEL_USB_CORE_H

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
  USB_SPEED_SUPER_PLUS = 5, // <-- NUEVO (USB 3.1/3.2, tratado como SUPER)
} usb_speed_t;

// ===========================================================================
// Modelo de dispositivo
// ===========================================================================
typedef struct usb_device usb_device_t;
typedef struct usb_hcd usb_hcd_t;

typedef struct usb_endpoint {
  uint8_t addr;        // bit 7: dir (0=OUT, 1=IN), bits 3:0 número
  uint8_t type;        // USB_EP_*
  uint16_t max_packet; // wMaxPacketSize & 0x7FF
  uint8_t interval;    // bInterval
  void *hc_ep;         // HCD-privado (contexto de endpoint xHCI)
} usb_endpoint_t;

typedef struct usb_interface {
  uint8_t number;      // bInterfaceNumber
  uint8_t alt_setting; // bAlternateSetting
  uint8_t class;       // bInterfaceClass
  uint8_t subclass;    // bInterfaceSubClass
  uint8_t protocol;    // bInterfaceProtocol
  usb_endpoint_t *endpoints;
  int num_endpoints;
} usb_interface_t;

typedef struct usb_config {
  uint8_t value;          // bConfigurationValue
  uint8_t num_interfaces; // bNumInterfaces
  uint8_t attributes;     // bmAttributes
  uint8_t max_power;      // bMaxPower (unidades de 2 mA)
  usb_interface_t *interfaces;
  int num_interface_structs; // incluye alt settings
} usb_config_t;

struct usb_device {
  uint8_t addr; // 1..127
  usb_speed_t speed;
  uint16_t vendor_id;
  uint16_t product_id;
  uint16_t bcd_usb;
  uint16_t bcd_device;
  uint8_t class, subclass, protocol;
  uint8_t max_packet_size_ep0;
  uint8_t num_configurations;

  // Índices de string (0 = sin string).
  uint8_t i_manufacturer, i_product, i_serial;

  // Strings ya decodificadas.
  char manufacturer[64];
  char product[64];
  char serial[64];

  usb_device_t *parent_hub;
  uint8_t port_on_parent;

  usb_config_t *configs[8];
  uint8_t active_config; // bConfigurationValue activa, 0 = sin configurar

  usb_hcd_t *hcd; // back-pointer al controlador que lo gestiona
  void *hc_priv;  // datos privados del HCD (slot_id de xHCI, etc.)
};

// ===========================================================================
// Interfaz HCD (Host Controller Driver).
//
// La implementa xHCI (y en el futuro EHCI/OHCI/UHCI).
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

  // Crea un nuevo usb_device_t detrás de un puerto (root o hub externo).
  int (*new_device)(usb_device_t *parent_hub, uint8_t port, usb_speed_t speed,
                    usb_device_t **out_dev);

  // Marca un dispositivo ya direccionado como Hub y actualiza su
  // Number of Ports. Requerido para que el xHCI asocie el Transaction
  // Translator (TT) de sus hijos.
  int (*set_hub_info)(usb_device_t *dev, uint8_t num_ports);

  int (*alloc_device)(usb_device_t *dev);
  void (*free_device)(usb_device_t *dev);
};

// ===========================================================================
// Parsers puros (sin I/O). El llamante pasa el buffer crudo leído por
// control transfer.
// ===========================================================================

// Rellena `dev_out` con los campos del device descriptor.
// Devuelve 0 si OK, -EINVAL si el descriptor está malformado.
int usb_parse_device_descriptor(const uint8_t *raw, usb_device_t *dev_out);

// Parsea un blob de configuration (config + interface(s) + endpoint(s)).
// Devuelve un usb_config_t* heap-allocated (liberar con usb_config_free),
// o NULL si el blob está malformado / sin memoria.
usb_config_t *usb_parse_config_descriptor(const uint8_t *raw, int raw_len);

void usb_config_free(usb_config_t *cfg);

// ===========================================================================
// Helpers de control transfer (usan dev->hcd->control_xfer).
// ===========================================================================
int usb_control_transfer(usb_device_t *dev, uint8_t request_type,
                         uint8_t request, uint16_t value, uint16_t index,
                         void *data, uint16_t len, int timeout_ms);

int usb_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t index,
                       uint16_t langid, void *buf, uint16_t len);

int usb_set_configuration(usb_device_t *dev, uint8_t config_value);

// Lee un string descriptor y lo decodifica a ASCII (reemplaza no-ASCII
// por '?'). `langid` típico es 0x0409 (inglés US). Devuelve la longitud
// copiada (sin NUL) o negativo.
int usb_get_string_descriptor(usb_device_t *dev, uint8_t index, uint16_t langid,
                              char *out, size_t outlen);

// ===========================================================================
// Ciclo de vida + enumeración genérica.
// ===========================================================================
usb_device_t *usb_device_alloc(void);
void usb_device_free(usb_device_t *dev);

// Lee device + config descriptor, setea configuración y strings.
// El HCD debe haber rellenado dev->speed, dev->hcd, dev->hc_priv y
// dev->max_packet_size_ep0 antes de llamar.
int usb_enumerate(usb_device_t *dev);

// Self-test de los parsers. Devuelve 0 si todo pasa, >0 = fallos.
// Pensado para engancharse al run_all_tests().
int usb_core_self_test(void);

// Calcula el route string xHCI de un device (walk de parent_hub).
uint32_t usb_device_route(const usb_device_t *dev);

#endif