// kernel/usb/usb_core.c
//
// Modelo de dispositivo USB, parser de descriptores y enumeración
// genérica. No habla con hardware: toda la E/S pasa por dev->hcd.

#include "usb_core.h"
#include "../bus.h"
#include "../heap.h"
#include "../klog.h"
#include "../string.h"
#include "../uaccess.h"
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
      ep->stream_id = 0;
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
// Control transfer
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
  int rc =
      dev->hcd->control_xfer(dev, setup, data, (int)len, is_in, timeout_ms);
  return rc;
}

int usb_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t index,
                       uint16_t langid, void *buf, uint16_t len) {
  uint16_t value = (uint16_t)((type << 8) | index);
  return usb_control_transfer(dev, 0x80, 0x06, value, langid, buf, len, 5000);
}

int usb_set_configuration(usb_device_t *dev, uint8_t config_value) {
  return usb_control_transfer(dev, 0x00, 0x09, config_value, 0, NULL, 0, 5000);
}

// ===========================================================================
// String descriptor
// ===========================================================================
int usb_get_string_descriptor(usb_device_t *dev, uint8_t index, uint16_t langid,
                              char *out, size_t outlen) {
  if (!dev || !out || outlen == 0)
    return -EINVAL;
  out[0] = '\0';
  if (index == 0)
    return 0;

  uint8_t buf[255] = {0};
  int rc =
      usb_get_descriptor(dev, USB_DT_STRING, index, langid, buf, sizeof(buf));
  if (rc < 0)
    return rc;

  if (rc < 2 || buf[0] < 2 || buf[1] != USB_DT_STRING)
    return -EIO;

  int blen = buf[0] < rc ? buf[0] : rc;
  int nchars = (blen - 2) / 2;
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
  // [FASE 2.1c] Desregistrar del bus USB antes de liberar nada. Si el
  // device nunca llegó a usb_device_register (fallo en enumerate), es
  // no-op.
  usb_device_unregister(dev);
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

uint32_t usb_device_route(const usb_device_t *dev) {
  if (!dev || !dev->parent_hub)
    return 0;

  uint8_t ports[5];
  int n = 0;
  const usb_device_t *d = dev;
  while (d && d->parent_hub && n < 5) {
    ports[n++] = (uint8_t)(d->port_on_parent & 0xF);
    d = d->parent_hub;
  }
  uint32_t route = 0;
  for (int i = 0; i < n; i++)
    route |= ((uint32_t)ports[n - 1 - i]) << (i * 4);
  return route;
}

// ===========================================================================
// [FASE 2.1c] Bus USB genérico.
//
// Pool estático de struct device. Como máximo un puñado de devices USB
// por controlador (root hub + hubs anidados + devices finales). El slot
// se marca libre con dev->bus = NULL; usb_device_unregister lo limpia.
// ===========================================================================
#define USB_MAX_BUS_DEVICES 32
static struct device g_usb_dev_nodes[USB_MAX_BUS_DEVICES];

static int usb_match_one(const usb_device_t *dev, const usb_device_id_t *id) {
  if (id->match_flags & USB_DEVICE_ID_MATCH_VENDOR)
    if (dev->vendor_id != id->id_vendor)
      return 0;
  if (id->match_flags & USB_DEVICE_ID_MATCH_PRODUCT)
    if (dev->product_id != id->id_product)
      return 0;
  if (id->match_flags & USB_DEVICE_ID_MATCH_DEV_CLASS)
    if (dev->class != id->b_device_class)
      return 0;
  if (id->match_flags & USB_DEVICE_ID_MATCH_DEV_SUBCLASS)
    if (dev->subclass != id->b_device_subclass)
      return 0;
  if (id->match_flags & USB_DEVICE_ID_MATCH_DEV_PROTOCOL)
    if (dev->protocol != id->b_device_protocol)
      return 0;

  if (id->match_flags &
      (USB_DEVICE_ID_MATCH_INT_CLASS | USB_DEVICE_ID_MATCH_INT_SUBCLASS |
       USB_DEVICE_ID_MATCH_INT_PROTOCOL)) {
    int found = 0;
    for (int c = 0; c < 8 && !found; c++) {
      if (!dev->configs[c])
        continue;
      for (int i = 0; i < dev->configs[c]->num_interface_structs; i++) {
        const usb_interface_t *itf = &dev->configs[c]->interfaces[i];
        int ok = 1;
        if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_CLASS) &&
            itf->class != id->b_interface_class)
          ok = 0;
        if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_SUBCLASS) &&
            itf->subclass != id->b_interface_subclass)
          ok = 0;
        if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_PROTOCOL) &&
            itf->protocol != id->b_interface_protocol)
          ok = 0;
        if (ok) {
          found = 1;
          break;
        }
      }
    }
    if (!found)
      return 0;
  }
  return 1;
}

static int usb_bus_match(struct device *dev, struct driver *drv,
                         const void **id_out) {
  if (!drv->id_table)
    return 0;
  const usb_device_t *udev = (const usb_device_t *)dev->priv;
  if (!udev)
    return 0;
  const usb_device_id_t *ids = (const usb_device_id_t *)drv->id_table;
  for (int i = 0; ids[i].match_flags != 0; i++) {
    if (usb_match_one(udev, &ids[i])) {
      if (id_out)
        *id_out = &ids[i];
      return 1;
    }
  }
  return 0;
}

static int usb_bus_probe(struct device *dev) {
  if (!dev->driver || !dev->driver->probe)
    return -EINVAL;
  return dev->driver->probe(dev);
}

struct bus_type usb_bus = {
    .name = "usb",
    .match = usb_bus_match,
    .probe = usb_bus_probe,
};

int usb_device_register(usb_device_t *udev) {
  if (!udev)
    return -EINVAL;
  if (udev->dev_node)
    return 0; // ya registrado

  int idx = -1;
  for (int i = 0; i < USB_MAX_BUS_DEVICES; i++) {
    if (g_usb_dev_nodes[i].bus == NULL) {
      idx = i;
      break;
    }
  }
  if (idx < 0) {
    LOG_WARN("[USB] bus device pool lleno (%d)", USB_MAX_BUS_DEVICES);
    return -ENOMEM;
  }

  struct device *dev = &g_usb_dev_nodes[idx];
  memset(dev, 0, sizeof(*dev));
  // snprintf está en kernel/string.h. El nombre "usb-<addr>" es único
  // dentro del bus (addr lo asigna el HCD; root hub usa 0).
  snprintf(dev->name, sizeof(dev->name), "usb-%u", (unsigned)udev->addr);
  dev->bus = &usb_bus;
  dev->priv = udev;
  dev->id = udev;

  udev->dev_node = dev;
  return device_register(dev);
}

void usb_device_unregister(usb_device_t *udev) {
  if (!udev || !udev->dev_node)
    return;
  struct device *dev = udev->dev_node;
  device_unregister(dev);
  dev->bus = NULL; // marcar libre para reutilización
  dev->driver = NULL;
  dev->priv = NULL;
  dev->id = NULL;
  dev->matched_id = NULL;
  dev->next = NULL;
  udev->dev_node = NULL;
}

int usb_bus_init(void) {
  extern struct driver usb_hid_driver;
  extern struct driver usb_storage_driver;
  extern struct driver usb_hub_driver;

  int rc = bus_register(&usb_bus);
  if (rc != 0) {
    LOG_ERR("[USB] bus_register falló: %d", rc);
    return rc;
  }

  driver_register(&usb_hid_driver);
  driver_register(&usb_storage_driver);
  driver_register(&usb_hub_driver);

  LOG_INFO("[USB] bus USB registrado (hid, storage, hub)");
  return 0;
}

// ===========================================================================
// Enumeración genérica.
// ===========================================================================
int usb_enumerate(usb_device_t *dev) {
  if (!dev || !dev->hcd || !dev->hcd->control_xfer)
    return -ENODEV;

  uint8_t dev_desc[18] = {0};
  int rc =
      usb_get_descriptor(dev, USB_DT_DEVICE, 0, 0, dev_desc, sizeof(dev_desc));
  if (rc < 0) {
    LOG_ERR("[USB] GET_DESCRIPTOR(DEVICE) falló: %d", rc);
    return rc;
  }
  if (rc != (int)sizeof(dev_desc)) {
    LOG_ERR("[USB] GET_DESCRIPTOR(DEVICE) incompleto: %d/%u bytes", rc,
            (unsigned)sizeof(dev_desc));
    return -EIO;
  }
  rc = usb_parse_device_descriptor(dev_desc, dev);
  if (rc != 0) {
    LOG_ERR("[USB] device descriptor malformado");
    return rc;
  }

  LOG_INFO("[USB] device %04x:%04x (bcdUSB=%04x, EP0=%u, %u configs)",
           dev->vendor_id, dev->product_id, dev->bcd_usb,
           dev->max_packet_size_ep0, dev->num_configurations);

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

  if (dev->configs[0]) {
    rc = usb_set_configuration(dev, dev->configs[0]->value);
    if (rc < 0) {
      LOG_WARN("[USB] SET_CONFIGURATION(%u) falló: %d", dev->configs[0]->value,
               rc);
    }
    dev->active_config = dev->configs[0]->value;
  }

  if (dev->i_manufacturer || dev->i_product || dev->i_serial) {
    uint8_t lbuf[8] = {0};
    int lrc = usb_get_descriptor(dev, USB_DT_STRING, 0, 0, lbuf, sizeof(lbuf));
    if (lrc >= 4 && lbuf[1] == USB_DT_STRING && lbuf[0] >= 4) {
      uint16_t langid = (uint16_t)(lbuf[2] | (lbuf[3] << 8));
      usb_get_string_descriptor(dev, dev->i_manufacturer, langid,
                                dev->manufacturer, sizeof(dev->manufacturer));
      usb_get_string_descriptor(dev, dev->i_product, langid, dev->product,
                                sizeof(dev->product));
      usb_get_string_descriptor(dev, dev->i_serial, langid, dev->serial,
                                sizeof(dev->serial));
    }
  }

  LOG_INFO("[USB]   manuf='%s' prod='%s' serial='%s'",
           dev->manufacturer[0] ? dev->manufacturer : "?",
           dev->product[0] ? dev->product : "?",
           dev->serial[0] ? dev->serial : "?");

  // [FASE 2.1c] El dispatch a hid/storage/hub lo hace el bus USB. Antes
  // se llamaba aquí directamente a usb_hid_probe / usb_storage_probe /
  // usb_hub_probe; ahora esos son drivers del bus y el matching lo hace
  // bus_try_probe() cuando device_register dispara.
  //
  // Si usb_device_register falla (pool lleno), el device queda enumerado
  // pero sin driver. No abortamos el enumerate: el device es funcional
  // (tiene config activa) y un unload posterior lo libera limpiamente.
  int brc = usb_device_register(dev);
  if (brc != 0) {
    LOG_WARN("[USB] device_register falló: %d (device queda sin driver)", brc);
  }

  return 0;
}

// ===========================================================================
// Self-test
// ===========================================================================
static int usb_self_test_device(void) {
  const uint8_t raw[18] = {
      18,  USB_DT_DEVICE, 0x00, 0x02, 0x00, 0x00, 0x00, 64, 0x6B,
      0x1D, 0x02,          0x00, 0x00, 0x01, 0,    0,    0,  1,
  };
  usb_device_t dev;
  memset(&dev, 0, sizeof(dev));
  if (usb_parse_device_descriptor(raw, &dev) != 0)
    return 1;
  if (dev.vendor_id != 0x1D6B || dev.product_id != 0x0002)
    return 1;
  if (dev.max_packet_size_ep0 != 64 || dev.num_configurations != 1)
    return 1;
  return 0;
}

static int usb_self_test_config(void) {
  const uint8_t raw[] = {
      9,  USB_DT_CONFIG, 32, 0, 1, 1, 0, 0x80, 50,
      9,  USB_DT_INTERFACE, 0, 0, 2, 0x08, 0x06, 0x50, 0,
      7,  USB_DT_ENDPOINT, 0x81, 0x02, 0x00, 0x02, 0,
      7,  USB_DT_ENDPOINT, 0x02, 0x02, 0x00, 0x02, 0,
  };
  usb_config_t *cfg = usb_parse_config_descriptor(raw, (int)sizeof(raw));
  if (!cfg)
    return 1;
  int fail = 0;
  if (cfg->value != 1 || cfg->num_interface_structs != 1)
    fail |= 1;
  usb_interface_t *itf = &cfg->interfaces[0];
  if (itf->class != 0x08 || itf->num_endpoints != 2)
    fail |= 2;
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