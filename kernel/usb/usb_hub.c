// kernel/usb/usb_hub.c
//
// Driver USB Hub (class 0x09). Soporta hubs USB 2.0 y 3.0.
//
// Modelo: un kthread por hub, esperando en el interrupt endpoint.
// Cuando llega un change-bit, hace GET_PORT_STATUS, resetea el puerto
// si hace falta, y llama a hcd->new_device + usb_enumerate.

#include "usb_hub.h"
#include "../heap.h"
#include "../klog.h"
#include "../sched.h"
#include "../string.h"
#include "../uaccess.h"
#include "usb_core.h"

// ===========================================================================
// Class requests y constantes (USB 2.0 §11.24, USB 3.0 §10.16)
// ===========================================================================
#define USB_HUB_CLASS 0x09

#define HUB_REQ_GET_STATUS 0x00
#define HUB_REQ_CLEAR_FEATURE 0x01
#define HUB_REQ_SET_FEATURE 0x03
#define HUB_REQ_GET_DESCRIPTOR 0x06

#define HUB_DESC_TYPE 0x29
#define HUB_DESC_TYPE_SS 0x2A

// Port features
#define PORT_CONNECTION 0
#define PORT_ENABLE 1
#define PORT_SUSPEND 2
#define PORT_OVER_CURRENT 3
#define PORT_RESET 4
#define PORT_POWER 8
#define PORT_LOW_SPEED 9
#define C_PORT_CONNECTION 16
#define C_PORT_ENABLE 17
#define C_PORT_SUSPEND 18
#define C_PORT_OVER_CURRENT 19
#define C_PORT_RESET 20

// Port status bits (USB 2.0 §11.8.3 / USB 3.0 §10.16.2.6)
#define PORT_STAT_CONNECTION 0x0001
#define PORT_STAT_ENABLE 0x0002
#define PORT_STAT_SUSPEND 0x0004
#define PORT_STAT_OVERCURRENT 0x0008
#define PORT_STAT_RESET 0x0010
#define PORT_STAT_POWER 0x0100
#define PORT_STAT_LOW_SPEED 0x0200
#define PORT_STAT_HIGH_SPEED 0x0400

#define PORT_CHANGE_CONNECTION 0x0001
#define PORT_CHANGE_ENABLE 0x0002
#define PORT_CHANGE_SUSPEND 0x0004
#define PORT_CHANGE_OVERCURRENT 0x0008
#define PORT_CHANGE_RESET 0x0010

// ===========================================================================
// Hub descriptor (USB 2.0 §11.23.2.1)
// ===========================================================================
typedef struct __attribute__((packed)) {
  uint8_t bDescLength;
  uint8_t bDescriptorType;
  uint8_t bNbrPorts;
  uint16_t wHubCharacteristics;
  uint8_t bPwrOn2PwrGood;
  uint8_t bHubContrCurrent;
  uint8_t var[256]; // DeviceRemovable + PortPwrCtrlMask
} usb_hub_desc_t;

// ===========================================================================
// Estado por hub
// ===========================================================================
typedef struct usb_hub_dev {
  usb_device_t *dev;
  usb_endpoint_t *int_ep;
  uint8_t n_ports;
  uint16_t characteristics;
  uint8_t pwr_on_2_pwr_good;
  int is_ss;
  char name[32];
} usb_hub_dev_t;

#define MAX_HUBS 8
static usb_hub_dev_t *g_hubs[MAX_HUBS];
static int g_hub_count = 0;
static usb_hub_dev_t *g_hub_pending = NULL;

// ===========================================================================
// Class requests (todas van a dev->hcd->control_xfer)
// ===========================================================================
static int hub_get_port_status(usb_device_t *dev, uint8_t port,
                               uint16_t *status, uint16_t *change) {
  uint8_t buf[4] = {0};
  int rc = usb_control_transfer(dev, 0xA3, HUB_REQ_GET_STATUS, 0, port, buf,
                                sizeof(buf), 1000);
  if (rc < 0)
    return rc;
  if (status)
    *status = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
  if (change)
    *change = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
  return 0;
}

static int hub_clear_port_feature(usb_device_t *dev, uint8_t port,
                                  uint16_t feature) {
  int rc = usb_control_transfer(dev, 0x23, HUB_REQ_CLEAR_FEATURE, feature, port,
                                NULL, 0, 1000);
  return rc < 0 ? rc : 0;
}

static int hub_set_port_feature(usb_device_t *dev, uint8_t port,
                                uint16_t feature) {
  int rc = usb_control_transfer(dev, 0x23, HUB_REQ_SET_FEATURE, feature, port,
                                NULL, 0, 1000);
  return rc < 0 ? rc : 0;
}

static int hub_get_descriptor(usb_device_t *dev, usb_hub_desc_t *desc) {
  uint8_t dtype =
      (dev->speed == USB_SPEED_SUPER || dev->speed == USB_SPEED_SUPER_PLUS)
          ? HUB_DESC_TYPE_SS
          : HUB_DESC_TYPE;
  int rc = usb_control_transfer(dev, 0xA0, HUB_REQ_GET_DESCRIPTOR, dtype << 8,
                                0, desc, sizeof(*desc), 1000);
  if (rc < 0)
    return rc;
  // Sanity: el descriptor tiene que ser del tipo pedido y tener al
  // menos los 7 bytes fijos del hub descriptor.
  if (desc->bDescriptorType != dtype || desc->bDescLength < 7)
    return -EIO;
  return 0;
}

// ===========================================================================
// Reset + attach de un puerto del hub
// ===========================================================================
static int hub_port_attach(usb_hub_dev_t *hub, uint8_t port) {
  usb_device_t *hdev = hub->dev;

  // SET_PORT_FEATURE(PORT_RESET).
  int rc = hub_set_port_feature(hdev, port, PORT_RESET);
  if (rc != 0) {
    LOG_ERR("[HUB] %s port %u: PORT_RESET falló: %d", hub->name, port, rc);
    return rc;
  }

  // Esperar C_PORT_RESET (o timeout).
  uint16_t status = 0, change = 0;
  int ok = 0;
  for (int i = 0; i < 500; i++) {
    rc = hub_get_port_status(hdev, port, &status, &change);
    if (rc == 0 && (change & PORT_CHANGE_RESET)) {
      ok = 1;
      break;
    }
    for (int k = 0; k < 10; k++)
      sched_yield();
  }
  if (!ok) {
    LOG_ERR("[HUB] %s port %u: timeout esperando C_PORT_RESET", hub->name,
            port);
    return -ETIMEDOUT;
  }
  hub_clear_port_feature(hdev, port, C_PORT_RESET);

  // Determinar speed.
  usb_speed_t speed = USB_SPEED_FULL;
  if (hub->is_ss) {
    uint32_t sp = (status >> 10) & 0x0F;
    if (sp == 3)
      speed = USB_SPEED_SUPER;
    else if (sp == 4)
      speed = USB_SPEED_SUPER; // SS+ → tratamos como SS
    else
      speed = USB_SPEED_UNKNOWN;
  } else {
    if (status & PORT_STAT_HIGH_SPEED)
      speed = USB_SPEED_HIGH;
    else if (status & PORT_STAT_LOW_SPEED)
      speed = USB_SPEED_LOW;
    else
      speed = USB_SPEED_FULL;
  }

  LOG_INFO("[HUB] %s port %u: connected speed=%d status=0x%04x", hub->name,
           port, (int)speed, status);

  // Crear el device.
  usb_device_t *dev = NULL;
  rc = hdev->hcd->new_device(hdev, port, speed, &dev);
  if (rc != 0) {
    LOG_ERR("[HUB] %s port %u: new_device falló: %d", hub->name, port, rc);
    return rc;
  }

  rc = usb_enumerate(dev);
  if (rc != 0) {
    LOG_ERR("[HUB] %s port %u: enumerate falló: %d", hub->name, port, rc);
    return rc;
  }
  return 0;
}

// ===========================================================================
// Handle a port change
// ===========================================================================
static void hub_port_change(usb_hub_dev_t *hub, uint8_t port) {
  uint16_t status = 0, change = 0;
  int rc = hub_get_port_status(hub->dev, port, &status, &change);
  if (rc != 0)
    return;

  LOG_DEBUG("[HUB] %s port %u: status=0x%04x change=0x%04x", hub->name, port,
            status, change);

  if (change & PORT_CHANGE_CONNECTION) {
    hub_clear_port_feature(hub->dev, port, C_PORT_CONNECTION);
    if (!(status & PORT_STAT_CONNECTION)) {
      LOG_INFO("[HUB] %s port %u: disconnected", hub->name, port);
      return;
    }
  }

  if (change & PORT_CHANGE_ENABLE)
    hub_clear_port_feature(hub->dev, port, C_PORT_ENABLE);
  if (change & PORT_CHANGE_SUSPEND)
    hub_clear_port_feature(hub->dev, port, C_PORT_SUSPEND);
  if (change & PORT_CHANGE_OVERCURRENT)
    hub_clear_port_feature(hub->dev, port, C_PORT_OVER_CURRENT);

  // Si hay device conectado y el puerto no está ya habilitado,
  // resetear + attach.
  if ((status & PORT_STAT_CONNECTION) && !(status & PORT_STAT_ENABLE)) {
    hub_port_attach(hub, port);
  }
}

// ===========================================================================
// Kthread por hub
// ===========================================================================
static void usb_hub_kthread(void) {
  usb_hub_dev_t *hub = g_hub_pending;
  g_hub_pending = NULL;
  if (!hub) {
    LOG_ERR("[HUB] kthread sin pending hub");
    return;
  }
  LOG_INFO("[HUB] %s: kthread iniciado (%u puertos)", hub->name, hub->n_ports);

  uint8_t buf[32];
  int report_len = (hub->n_ports + 7) / 8;
  if (report_len <= 0 || report_len > (int)sizeof(buf))
    report_len = 1;

  int err = 0;
  while (1) {
    int actual = 0;
    int rc = hub->dev->hcd->interrupt_xfer(hub->dev, hub->int_ep, buf,
                                           report_len, 0, &actual);
    if (rc == 0 && actual > 0) {
      err = 0;
      for (int p = 0; p < hub->n_ports; p++) {
        if (buf[p / 8] & (1 << (p % 8)))
          hub_port_change(hub, (uint8_t)(p + 1));
      }
    } else if (rc < 0) {
      if (++err == 1 || (err & 0x3F) == 0)
        LOG_WARN("[HUB] %s: interrupt xfer rc=%d", hub->name, rc);
      for (int i = 0; i < 50; i++)
        sched_yield();
    }
  }
}

static int usb_hub_spawn_kthread(usb_hub_dev_t *hub) {
  while (g_hub_pending != NULL)
    sched_yield();
  g_hub_pending = hub;
  if (!sched_create_task(usb_hub_kthread)) {
    g_hub_pending = NULL;
    return -ENOMEM;
  }
  while (g_hub_pending != NULL)
    sched_yield();
  return 0;
}

// ===========================================================================
// Probe
// ===========================================================================
int usb_hub_probe(usb_device_t *dev) {
  if (!dev || !dev->hcd || !dev->hcd->configure_ep || !dev->hcd->new_device)
    return -EINVAL;

  usb_config_t *cfg = dev->configs[0];
  if (!cfg)
    return -EINVAL;

  int ports_with_device = 0;

  for (int i = 0; i < cfg->num_interface_structs; i++) {
    usb_interface_t *itf = &cfg->interfaces[i];
    if (itf->class != USB_HUB_CLASS)
      continue;

    // Buscar el interrupt IN endpoint.
    usb_endpoint_t *int_ep = NULL;
    for (int j = 0; j < itf->num_endpoints; j++) {
      usb_endpoint_t *ep = &itf->endpoints[j];
      if ((ep->addr & 0x80) && ep->type == USB_EP_INTERRUPT) {
        int_ep = ep;
        break;
      }
    }
    if (!int_ep) {
      LOG_WARN("[HUB] iface %u: sin interrupt IN", itf->number);
      continue;
    }

    // Leer descriptor del hub.
    usb_hub_desc_t hdesc;
    memset(&hdesc, 0, sizeof(hdesc));
    int rc = hub_get_descriptor(dev, &hdesc);
    if (rc != 0) {
      LOG_ERR("[HUB] GET_HUB_DESCRIPTOR falló: %d", rc);
      continue;
    }
    if (hdesc.bNbrPorts == 0) {
      LOG_ERR("[HUB] bNbrPorts=0");
      continue;
    }

    usb_hub_dev_t *hub = kzalloc(sizeof(*hub));
    if (!hub)
      return -ENOMEM;
    hub->dev = dev;
    hub->int_ep = int_ep;
    hub->n_ports = hdesc.bNbrPorts;
    hub->characteristics = hdesc.wHubCharacteristics;
    hub->pwr_on_2_pwr_good = hdesc.bPwrOn2PwrGood;
    hub->is_ss =
        (dev->speed == USB_SPEED_SUPER || dev->speed == USB_SPEED_SUPER_PLUS);
    snprintf(hub->name, sizeof(hub->name), "hub-%u", itf->number);

    LOG_INFO("[HUB] %s: %u puertos, chars=0x%04x pwr_good=%u is_ss=%d",
             hub->name, hub->n_ports, hub->characteristics,
             hub->pwr_on_2_pwr_good, hub->is_ss);

    // Power-on de puertos (si bPwrOn2PwrGood > 0).
    if (hub->pwr_on_2_pwr_good > 0) {
      for (uint8_t p = 1; p <= hub->n_ports; p++) {
        hub_set_port_feature(dev, p, PORT_POWER);
      }
      extern uint64_t klog_get_tsc_freq(void);
      // Delay 2 * pwr_good ms (aprox, via busy-wait corto).
      volatile uint64_t n = (uint64_t)hub->pwr_on_2_pwr_good * 2000;
      for (volatile uint64_t k = 0; k < n * 200; k++)
        __asm__ volatile("pause");
    }

    // Configure Endpoint del interrupt.
    rc = dev->hcd->configure_ep(dev, int_ep);
    if (rc != 0) {
      LOG_ERR("[HUB] %s: configure_ep falló: %d", hub->name, rc);
      kfree(hub);
      continue;
    }

    if (g_hub_count >= MAX_HUBS) {
      LOG_ERR("[HUB] tabla llena");
      kfree(hub);
      continue;
    }
    g_hubs[g_hub_count++] = hub;

    if (usb_hub_spawn_kthread(hub) != 0) {
      LOG_ERR("[HUB] %s: no se pudo lanzar kthread", hub->name);
      g_hub_count--;
      kfree(hub);
      continue;
    }

    // Probar cada puerto una vez (por si hay devices ya conectados).
    for (uint8_t p = 1; p <= hub->n_ports; p++) {
      uint16_t status = 0, change = 0;
      rc = hub_get_port_status(dev, p, &status, &change);
      if (rc != 0)
        continue;
      if (status & PORT_STAT_CONNECTION) {
        hub_port_change(hub, p);
        ports_with_device++;
      }
    }
  }

  return ports_with_device;
}