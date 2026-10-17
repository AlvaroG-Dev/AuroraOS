// kernel/usb/usb_core.c
//
// Modelo de dispositivo USB, parser de descriptores y enumeración
// genérica. No habla con hardware: toda la E/S pasa por dev->hcd.

#include "usb_core.h"
#include "heap.h"
#include "klog.h"
#include "string.h"
#include "uaccess.h" // EINVAL, ENOMEM, EIO, ENODEV
#include "usb_hid.h"

// ===========================================================================
// Parser: device descriptor (18 bytes fijos)
// ===========================================================================
int usb_parse_device_descriptor(const uint8_t *raw, usb_device_t *dev_out) {
  if (!raw || !dev_out)
    return -EINVAL;
  if (raw[0] < 18 || raw[1] != USB_DT_DEVICE)
    return -EINVAL;

  dev_out->bcd_usb = (uint16_t)(raw[2] | (raw[3] << 8));
  dev_out->class = raw[4];
  dev_out->subclass = raw[5];
  dev_out->protocol = raw[6];
  dev_out->max_packet_size_ep0 = raw[7];
  dev_out->vendor_id = (uint16_t)(raw[8] | (raw[9] << 8));
  dev_out->product_id = (uint16_t)(raw[10] | (raw[11] << 8));
  dev_out->bcd_device = (uint16_t)(raw[12] | (raw[13] << 8));
  dev_out->i_manufacturer = raw[14];
  dev_out->i_product = raw[15];
  dev_out->i_serial = raw[16];
  dev_out->num_configurations = raw[17];
  return 0;
}

// ===========================================================================
// Parser: configuration blob
//
// Estructura: [config][interface + endpoints]* [interface + endpoints]*
// ===========================================================================
usb_config_t *usb_parse_config_descriptor(const uint8_t *raw, int raw_len) {
  if (!raw || raw_len < 9)
    return NULL;
  if (raw[0] < 9 || raw[1] != USB_DT_CONFIG)
    return NULL;

  uint16_t total = (uint16_t)(raw[2] | (raw[3] << 8));
  if (total < 9 || total > raw_len)
    return NULL;

  usb_config_t *cfg = (usb_config_t *)kzalloc(sizeof(*cfg));
  if (!cfg)
    return NULL;

  cfg->value = raw[5];
  cfg->num_interfaces = raw[4];
  cfg->attributes = raw[7];
  cfg->max_power = raw[8];

  // Pass 1: contar interfaces.
  int n_iface = 0;
  int off = 0;
  while (off < total) {
    uint8_t dlen = raw[off];
    uint8_t dtype = raw[off + 1];
    if (dlen < 2 || off + dlen > total)
      goto fail;
    if (dtype == USB_DT_INTERFACE)
      n_iface++;
    off += dlen;
  }
  if (n_iface == 0)
    goto fail;

  cfg->interfaces =
      (usb_interface_t *)kzalloc((size_t)n_iface * sizeof(usb_interface_t));
  if (!cfg->interfaces)
    goto fail;
  cfg->num_interface_structs = n_iface;

  // Pass 2: rellenar. Cada interface recibe su propio array de endpoints.
  int cur_iface = -1;
  off = 0;
  while (off < total) {
    uint8_t dlen = raw[off];
    uint8_t dtype = raw[off + 1];

    if (dtype == USB_DT_INTERFACE && dlen >= 9) {
      cur_iface++;
      usb_interface_t *itf = &cfg->interfaces[cur_iface];
      itf->number = raw[off + 2];
      itf->alt_setting = raw[off + 3];
      itf->class = raw[off + 5];
      itf->subclass = raw[off + 6];
      itf->protocol = raw[off + 7];
      itf->endpoints = NULL;
      itf->num_endpoints = 0;
    } else if (dtype == USB_DT_ENDPOINT && dlen >= 7 && cur_iface >= 0) {
      usb_interface_t *itf = &cfg->interfaces[cur_iface];
      int ne = itf->num_endpoints;
      usb_endpoint_t *new_eps = (usb_endpoint_t *)krealloc(
          itf->endpoints, (size_t)(ne + 1) * sizeof(usb_endpoint_t));
      if (!new_eps)
        goto fail;
      itf->endpoints = new_eps;
      usb_endpoint_t *ep = &itf->endpoints[ne];
      ep->addr = raw[off + 2];
      ep->type = raw[off + 3] & 0x03;
      ep->max_packet = (uint16_t)((raw[off + 4] | (raw[off + 5] << 8)) & 0x7FF);
      ep->interval = raw[off + 6];
      ep->hc_ep = NULL;
      itf->num_endpoints++;
    }
    off += dlen;
  }

  return cfg;

fail:
  usb_config_free(cfg);
  return NULL;
}

void usb_config_free(usb_config_t *cfg) {
  if (!cfg)
    return;
  if (cfg->interfaces) {
    for (int i = 0; i < cfg->num_interface_structs; i++)
      kfree(cfg->interfaces[i].endpoints);
    kfree(cfg->interfaces);
  }
  kfree(cfg);
}

// ===========================================================================
// Control transfer: construye el setup packet y delega en el HCD.
// ===========================================================================
int usb_control_transfer(usb_device_t *dev, uint8_t request_type,
                         uint8_t request, uint16_t value, uint16_t index,
                         void *data, uint16_t len, int timeout_ms) {
  if (!dev || !dev->hcd || !dev->hcd->control_xfer)
    return -ENODEV;

  uint8_t setup[8];
  setup[0] = request_type;
  setup[1] = request;
  setup[2] = (uint8_t)(value & 0xFF);
  setup[3] = (uint8_t)(value >> 8);
  setup[4] = (uint8_t)(index & 0xFF);
  setup[5] = (uint8_t)(index >> 8);
  setup[6] = (uint8_t)(len & 0xFF);
  setup[7] = (uint8_t)(len >> 8);

  int is_in = (request_type & 0x80) ? 1 : 0;
  return dev->hcd->control_xfer(dev, setup, data, (int)len, is_in, timeout_ms);
}

int usb_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t index,
                       uint16_t langid, void *buf, uint16_t len) {
  uint16_t value = (uint16_t)((type << 8) | index);
  // bmRequestType = 0x80 (device-to-host, standard, device)
  return usb_control_transfer(dev, 0x80, 0x06, value, langid, buf, len, 5000);
}

int usb_set_configuration(usb_device_t *dev, uint8_t config_value) {
  // bmRequestType = 0x00 (host-to-device, standard, device)
  return usb_control_transfer(dev, 0x00, 0x09, config_value, 0, NULL, 0, 5000);
}

// ===========================================================================
// String descriptor: USB usa UTF-16LE. Convertimos a ASCII (no-ASCII -> '?').
// ===========================================================================
int usb_get_string_descriptor(usb_device_t *dev, uint8_t index, uint16_t langid,
                              char *out, size_t outlen) {
  if (!dev || !out || outlen == 0)
    return -EINVAL;
  out[0] = '\0';
  if (index == 0)
    return 0; // sin string

  uint8_t buf[255];
  int rc =
      usb_get_descriptor(dev, USB_DT_STRING, index, langid, buf, sizeof(buf));
  if (rc < 0)
    return rc;

  if (buf[0] < 2 || buf[1] != USB_DT_STRING)
    return -EIO;

  int nchars = (buf[0] - 2) / 2;
  size_t i;
  for (i = 0; i < (size_t)nchars && (i + 1) < outlen; i++) {
    uint16_t c = (uint16_t)(buf[2 + i * 2] | (buf[3 + i * 2] << 8));
    out[i] = (c < 0x80) ? (char)c : '?';
  }
  out[i] = '\0';
  return (int)i;
}

// ===========================================================================
// Ciclo de vida
// ===========================================================================
usb_device_t *usb_device_alloc(void) {
  usb_device_t *dev = (usb_device_t *)kzalloc(sizeof(*dev));
  return dev;
}

void usb_device_free(usb_device_t *dev) {
  if (!dev)
    return;
  for (int i = 0; i < 8; i++) {
    if (dev->configs[i]) {
      usb_config_free(dev->configs[i]);
      dev->configs[i] = NULL;
    }
  }
  if (dev->hcd && dev->hcd->free_device)
    dev->hcd->free_device(dev);
  kfree(dev);
}

// ===========================================================================
// Enumeración genérica.
// ===========================================================================
int usb_enumerate(usb_device_t *dev) {
  if (!dev || !dev->hcd || !dev->hcd->control_xfer)
    return -ENODEV;

  // 1. Device descriptor completo (18 bytes).
  //    El HCD ya ha hecho SET_ADDRESS y conoce el speed, así que EP0
  //    tiene max_packet_size correcto. Podemos leer los 18 de una.
  uint8_t dev_desc[18];
  int rc =
      usb_get_descriptor(dev, USB_DT_DEVICE, 0, 0, dev_desc, sizeof(dev_desc));
  if (rc < 0) {
    LOG_ERR("[USB] GET_DESCRIPTOR(DEVICE) falló: %d", rc);
    return rc;
  }
  rc = usb_parse_device_descriptor(dev_desc, dev);
  if (rc != 0) {
    LOG_ERR("[USB] device descriptor malformado");
    return rc;
  }

  LOG_INFO("[USB] device %04x:%04x (bcdUSB=%04x, EP0=%u, %u configs)",
           dev->vendor_id, dev->product_id, dev->bcd_usb,
           dev->max_packet_size_ep0, dev->num_configurations);

  // 2. Leer cada configuration descriptor.
  uint8_t cfg_hdr[9];
  int n_cfg = dev->num_configurations;
  if (n_cfg > 8)
    n_cfg = 8;

  for (int i = 0; i < n_cfg; i++) {
    rc = usb_get_descriptor(dev, USB_DT_CONFIG, (uint8_t)i, 0, cfg_hdr,
                            sizeof(cfg_hdr));
    if (rc < 0)
      return rc;

    uint16_t total = (uint16_t)(cfg_hdr[2] | (cfg_hdr[3] << 8));
    if (total < 9 || total > 4096)
      return -EIO;

    uint8_t *blob = (uint8_t *)kmalloc(total);
    if (!blob)
      return -ENOMEM;

    rc = usb_get_descriptor(dev, USB_DT_CONFIG, (uint8_t)i, 0, blob, total);
    if (rc < 0) {
      kfree(blob);
      return rc;
    }

    usb_config_t *cfg = usb_parse_config_descriptor(blob, total);
    kfree(blob);
    if (!cfg)
      return -EIO;
    dev->configs[i] = cfg;
  }

  // 3. Set configuration (la primera).
  if (dev->configs[0]) {
    rc = usb_set_configuration(dev, dev->configs[0]->value);
    if (rc < 0) {
      LOG_WARN("[USB] SET_CONFIGURATION(%u) falló: %d", dev->configs[0]->value,
               rc);
      // Algunos dispositivos no lo necesitan; seguimos.
    }
    dev->active_config = dev->configs[0]->value;
  }

  // 4. Strings. langid estándar inglés US.
  usb_get_string_descriptor(dev, dev->i_manufacturer, 0x0409, dev->manufacturer,
                            sizeof(dev->manufacturer));
  usb_get_string_descriptor(dev, dev->i_product, 0x0409, dev->product,
                            sizeof(dev->product));
  usb_get_string_descriptor(dev, dev->i_serial, 0x0409, dev->serial,
                            sizeof(dev->serial));

  LOG_INFO("[USB]   manuf='%s' prod='%s' serial='%s'",
           dev->manufacturer[0] ? dev->manufacturer : "?",
           dev->product[0] ? dev->product : "?",
           dev->serial[0] ? dev->serial : "?");

  // Probe HID (boot protocol kbd/mouse). No es fatal si falla.
  extern int usb_hid_probe(usb_device_t * dev);
  int hrc = usb_hid_probe(dev);
  if (hrc > 0)
    LOG_INFO("[USB] HID: %d interfaz(es) configurada(s)", hrc);

  return 0;
}

// ===========================================================================
// Self-test (para enganchar a run_all_tests()).
// ===========================================================================
static int usb_self_test_device(void) {
  const uint8_t raw[18] = {
      18,   USB_DT_DEVICE,
      0x00, 0x02, // bcdUSB = 2.00
      0x00, 0x00,
      0x00,       // class/sub/proto = 0 (per-interface)
      64,         // bMaxPacketSize0 = 64
      0x6B, 0x1D, // idVendor = 0x1D6B (Linux Foundation)
      0x02, 0x00, // idProduct = 0x0002
      0x00, 0x01, // bcdDevice = 1.00
      0,    0,
      0, // iMan/iProd/iSerial = 0
      1, // bNumConfigurations = 1
  };
  usb_device_t dev;
  memset(&dev, 0, sizeof(dev));
  if (usb_parse_device_descriptor(raw, &dev) != 0)
    return 1;
  if (dev.vendor_id != 0x1D6B)
    return 1;
  if (dev.product_id != 0x0002)
    return 1;
  if (dev.max_packet_size_ep0 != 64)
    return 1;
  if (dev.num_configurations != 1)
    return 1;
  return 0;
}

static int usb_self_test_config(void) {
  // Config + 1 interface (Mass Storage/BOT) + 2 bulk endpoints.
  const uint8_t raw[] = {
      9,    USB_DT_CONFIG,
      32,   0, // wTotalLength = 32
      1,       // bNumInterfaces = 1
      1,       // bConfigurationValue = 1
      0,       // iConfiguration = 0
      0x80,    // bmAttributes = bus-powered
      50,      // bMaxPower = 100 mA

      9,    USB_DT_INTERFACE,
      0,    0,
      2, // iface 0, alt 0, 2 endpoints
      0x08, 0x06,
      0x50, // Mass Storage / SCSI / BOT
      0,    // iInterface

      7,    USB_DT_ENDPOINT,
      0x81,       // IN, EP1
      0x02,       // Bulk
      0x00, 0x02, // wMaxPacketSize = 512
      0,

      7,    USB_DT_ENDPOINT,
      0x02,       // OUT, EP2
      0x02,       // Bulk
      0x00, 0x02, // wMaxPacketSize = 512
      0,
  };

  usb_config_t *cfg = usb_parse_config_descriptor(raw, (int)sizeof(raw));
  if (!cfg)
    return 1;

  int fail = 0;
  if (cfg->value != 1)
    fail |= 1;
  if (cfg->num_interface_structs != 1)
    fail |= 2;
  if (cfg->num_interfaces != 1)
    fail |= 4;
  usb_interface_t *itf = &cfg->interfaces[0];
  if (itf->class != 0x08 || itf->subclass != 0x06 || itf->protocol != 0x50)
    fail |= 8;
  if (itf->num_endpoints != 2)
    fail |= 16;
  if (itf->endpoints[0].addr != 0x81 || itf->endpoints[0].type != USB_EP_BULK)
    fail |= 32;
  if (itf->endpoints[1].addr != 0x02 || itf->endpoints[1].max_packet != 512)
    fail |= 64;

  usb_config_free(cfg);
  return fail ? 1 : 0;
}

int usb_core_self_test(void) {
  int fails = 0;
  fails += usb_self_test_device();
  fails += usb_self_test_config();
  if (fails == 0)
    LOG_INFO("[USB] self-test OK");
  else
    LOG_ERR("[USB] self-test: %d fallos", fails);
  return fails;
}