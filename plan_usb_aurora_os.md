# Plan completo para USB en Aurora OS

Voy a ser honesto primero sobre el alcance: "USB completo" son 2-4 semanas de trabajo a tiempo completo para llegar a un estado donde funcione todo lo que un usuario espera (teclado, ratón, almacenamiento, hubs). No es un fin de semana. Pero se puede estructurar para que en 3-4 días tengas teclado y ratón funcionando, y a partir de ahí extender.

Este plan asume que quieres un driver USB propio, no portar uno existente (como el de Linux o SeaBIOS).

---

## Decisiones de arquitectura (antes de tocar código)

### 1) Qué controlador implementar primero
Tu placa ASUS Z790-P tiene:
- **xHCI (USB 3.x)** — integrado en el chipset, es el principal. Todos los puertos pasan por él.
- **EHCI (USB 2.0)** — puede que exista como controlador separado, pero en Z790 ya no.
- **OHCI/UHCI** — no existen en tu placa.

Empieza por **xHCI**. Es el 95% del valor. EHCI/OHCI/UHCI los dejas para el final o los omites si solo quieres soportar hardware moderno.

### 2) Layout de directorios
```text
kernel/usb/
├── usb_core.c          # Modelo de dispositivo, descriptores, parseo
├── usb_core.h
├── usb_hub.c           # Hubs (root y externos)
├── usb_hub.h
├── usb_urb.c           # URB (USB Request Block) — abstracción de transferencia
├── usb_urb.h
├── usb_dma.c           # Buffers DMA contiguos físicamente
├── usb_dma.h
├── xhci/
│   ├── xhci.c          # Controller driver
│   ├── xhci.h
│   ├── xhci_ring.c     # Command, Event, Transfer rings
│   ├── xhci_ring.h
│   ├── xhci_cmd.c      # Comandos (Enable Slot, Address Device, ...)
│   ├── xhci_port.c     # Port reset, routing
│   └── xhci_trb.c      # TRB (Transfer Request Block) helpers
├── class/
│   ├── hid/
│   │   ├── usb_hid.c   # Driver HID genérico
│   │   ├── usb_hid.h
│   │   ├── hid_parser.c # Report descriptor parser
│   │   ├── hid_keyboard.c
│   │   └── hid_mouse.c
│   ├── storage/
│   │   ├── usb_storage.c    # Class driver BOT
│   │   ├── usb_storage.h
│   │   ├── usb_bot.c        # Bulk-Only Transport
│   │   ├── usb_scsi.c       # SCSI command set
│   │   └── usb_scsi.h
│   └── hub/
│       └── usb_hub_class.c  # Driver de clase hub
├── legacy/
│   ├── ehci.c
│   ├── ehci.h
│   ├── ohci.c
│   └── uhci.c
└── usb.h               # API pública (usb_init, etc.)
```

### 3) Modelo de ejecución
- **BSP** inicializa xHCI durante `kmain_task` (donde ya tienes `drivers_init_all`).
- **Polling por interrupción (MSI)**: xHCI genera eventos en su Event Ring. Configura MSI con uno de los vectores libres (`0x60-0x6F` ya tienes).
- **Bulk transfers**: `kthread` dedicado por dispositivo de almacenamiento.
- **HID interrupt transfers**: el controlador pone los datos en el buffer del cliente y dispara un evento. Desde ahí, el HID driver los entrega al input subsystem que ya tienes.

### 4) DMA
Asume no hay IOMMU activo (es lo normal en PC de escritorio). Las direcciones físicas van directamente al controlador. Documenta esto como limitación. Si algún día añades IOMMU, tendrás que añadir una capa de traducción.

Crea un helper `usb_dma_alloc(size, &phys)` que devuelva memoria físicamente contigua (vía PMM con páginas contiguas) alineada a 64 bytes (xHCI TRB alignment). Y `usb_dma_free(ptr, size)`.

### 5) Jerarquía de tests
Cada fase tiene su criterio de éxito. No pases a la siguiente sin cumplirlo.

---

## FASE 0 — Infraestructura USB (1-2 días)
**Objetivo:** tener el modelo de dispositivo, descriptores y URB antes de tocar ningún controlador.

### 0.1 USB core
- **Ficheros:** `usb_core.c/h`, `usb_urb.c/h`, `usb_dma.c/h`.
- **Qué hace:**
  - Define structs: `usb_device_t`, `usb_config_t`, `usb_interface_t`, `usb_endpoint_t`.
  - Parser de descriptores (device, config, interface, endpoint, string).
  - `usb_control_transfer()` — implementa la lógica común a todos los controladores (setup packet + data + status).
  - URB abstraction: cada operación de transferencia es un URB con callback.
- **Estructura de `usb_device_t`:**
  ```c
  typedef struct usb_device {
      uint8_t addr;               // 1..127
      uint8_t speed;              // LOW, FULL, HIGH, SUPER
      uint16_t vendor_id;
      uint16_t product_id;
      uint8_t class, subclass, protocol;
      uint8_t max_packet_size_ep0;
      struct usb_device *parent_hub;
      uint8_t port_on_parent;
      char manufacturer[64], product[64], serial[64];
      struct usb_config_descriptor *configs[8];
      uint8_t active_config;
      void *hc_priv;              // datos privados del HCD (xHCI slot, etc.)
  } usb_device_t;
  ```
- **Test:** Parsear descriptores generados a mano en un test unitario. Todos los tests deben pasar.
- **Effort:** 1 día.

### 0.2 DMA buffer
- **Qué hace:**
  - `usb_dma_alloc(size)`: pide páginas contiguas al PMM, devuelve VA + PA.
  - `usb_dma_free(va, size)`.
  - `usb_dma_sync(va, size, direction)` — para futuras arquitecturas con caché no coherente. En `x86_64` no hace nada, pero lo dejas por si acaso.
- **Test:** Reservar, escribir, leer por PA usando la direct map, verificar.
- **Effort:** medio día.

---

## FASE 1 — xHCI bring-up (3-4 días)
**Objetivo:** detectar el controlador, inicializarlo, leer descriptores del primer dispositivo.

### 1.1 Detección PCI + MMIO
- **Dónde:** `xhci.c: xhci_probe()`.
- Recorrer PCI buscando `class=0x0C`, `subclass=0x03`, `prog_if=0x30` (USB xHCI).
- Leer BAR0 (64-bit MMIO).
- Mapear con `mmio_map`.
- Habilitar bus mastering.
- Habilitar MSI (o MSI-X, mejor).
- **Test:** Log con la versión de xHCI (`HCCPARAMS`, `HCSPARAMS1/2`), número de puertos.
- **Effort:** medio día.

### 1.2 Reset y configuración del controlador
Pasos (capítulo 4.2 del xHCI spec):
1. `USBCMD = 0` (halt).
2. Esperar `USBSTS.HCHalted`.
3. Leer `HCSPARAMS1` para saber número de slots y puertos.
4. Escribir `CONFIG.MaxSlotsEn`.
5. Escribir `DCBAAP` (Device Context Base Address Array).
6. Escribir `CRCR` (Command Ring Control Register).
7. `USBCMD.RS = 1` (run).
- **Test:** `USBSTS.HCHalted = 0`. Los puertos empiezan a reportar conexiones.
- **Effort:** 1 día.

### 1.3 Rings (Command, Event, Transfer)
- **Ficheros:** `xhci_ring.c/h`.
- **Command Ring:** array circular de TRBs, controlado por el software, consumido por el hardware.
- **Event Ring:** array circular de TRBs, controlado por el hardware, consumido por el software. Necesita una ERST (Event Ring Segment Table).
- **Transfer Ring:** uno por endpoint. Los maneja el HCD.
- **Helpers:**
  - `xhci_ring_init(ring, size)`
  - `xhci_ring_enqueue(ring, trb)` — añade TRB, actualiza cycle bit, toca doorbell.
  - `xhci_ring_dequeue(ring)` — consume TRBs del event ring.
- **Test:** Enviar un comando no-op y recibir el Completion Event en el event ring.
- **Effort:** 1 día.

### 1.4 Port reset y Enable Slot
- `xhci_port_status(port)` — lee PORTSC.
- Si `CCS=1` (hay dispositivo) y aún no está habilitado: reset.
- `xhci_port_reset(port)` — escribe `PORTSC.PR=1`, espera `PRC=1`, escribe `PRC=1` para limpiar.
- `xhci_cmd_enable_slot()` → devuelve `slot_id`.
- `xhci_cmd_address_device(slot, port, speed, ep0_max_packet)` — asigna dirección USB al dispositivo.
- **Test:** El dispositivo aparece en el log con su dirección asignada.
- **Effort:** 1-2 días.

### 1.5 Get Descriptors
- `xhci_control_transfer(slot, ep0, setup, data, len)` — transferencia de control.
- `usb_get_device_descriptor()` — `GET_DESCRIPTOR(DEVICE)`.
- `usb_get_config_descriptor()` — `GET_DESCRIPTOR(CONFIG)`.
- `usb_get_string_descriptor()` — para fabricante, producto, serial.
- `usb_set_configuration(dev, config_idx)`.
- **Test:** Log con manufacturer, product, serial de un dispositivo conectado a un puerto físico. Aquí ya tienes validación de que todo el stack funciona.
- **Effort:** 1 día.

*Fin de la Fase 1: 3-4 días. Puedes leer descriptores de un dispositivo USB conectado.*

---

## FASE 2 — HID: teclado y ratón (3-4 días)
**Objetivo:** reemplazar/añadir al PS/2. El input subsystem ya existe.

### 2.1 USB HID class driver
- **Qué hace:**
  - Enumerar interfaces con `bInterfaceClass=0x03` (HID).
  - Obtener HID descriptor (`GET_DESCRIPTOR(HID)`).
  - Determinar si es keyboard/mouse por `bInterfaceProtocol` (1=keyboard, 2=mouse) o por el report descriptor.
  - Configurar un interrupt IN endpoint con `SET_IDLE` (opcional).
  - Arrancar el endpoint con un buffer de 8 bytes.

### 2.2 Boot protocol (rápido, para empezar)
Con `SET_PROTOCOL(0)` el dispositivo entra en boot protocol. Los reportes son:
- **Keyboard (8 bytes):**
  ```text
  [0] modifiers (Ctrl, Shift, Alt, GUI)
  [1] reserved
  [2..7] keycodes HID
  ```
- **Mouse (3-4 bytes):**
  ```text
  [0] buttons
  [1] X delta
  [2] Y delta
  [3] wheel (opcional)
  ```
Conversión a eventos `input_event_t` que ya tienes en `input.c`. Esto te da teclado y ratón funcionando en 1 día.

### 2.3 Report protocol (full)
Una vez funcione el boot protocol, implementa el parser de report descriptors:
- Recorrer el descriptor con Items (1 byte + parámetros).
- Extraer Usage, Usage Minimum/Maximum, Report Size, Report Count, Logical Min/Max, Input/Output/Feature.
- Construir una tabla de campos: `{offset_bits, size_bits, usage, min, max}`.
Esto es lo que permite soportar teclados con teclas multimedia, ratones con botones extra, tablets y gamepads. No es urgente para el primer bring-up. Hazlo después.

### 2.4 Integración con input
En `input.c` ya tienes `input_push(&ev)`. Añade una función:
```c
void usb_hid_keyboard_report(const uint8_t *report);
void usb_hid_mouse_report(const uint8_t *report);
```
Que conviertan el report en `input_event_t` y llamen a `input_push`.

- **Test:** Conectar un teclado USB. Escribir en el terminal. Ver los caracteres aparecer.
- **Effort:** 3-4 días para boot protocol + integración. +3 días para full report parser.

---

## FASE 3 — Hubs (2-3 días)
**Objetivo:** soportar dispositivos conectados a través de hubs (incluido el root hub virtual).

### 3.1 Root hub virtual
Aunque xHCI no tiene un hub físico, el software trata los puertos como un root hub virtual. Necesitas:
- Reportar los puertos como "downstream ports".
- Recibir cambios de estado (connect, disconnect, overcurrent) via PORTSC.

### 3.2 Hubs externos
- Detectar por `bDeviceClass=0x09` (hub) o interface class 0x09.
- Enumerar los puertos del hub.
- Configurar un interrupt endpoint del hub para recibir cambios de estado.
- Arrancar enumeración recursiva en cada puerto.

- **Test:** Conectar un hub con teclado y ratón. Ambos deben funcionar.
- **Effort:** 2-3 días.

---

## FASE 4 — Almacenamiento USB (5-7 días)
**Objetivo:** pendrive USB aparece como `/dev/sdX`, montable con FAT32.

### 4.1 Bulk-Only Transport (BOT)
Protocolo simple, es lo que usan el 99% de los pendrives.
- Enumerar interface class `0x08` (Mass Storage), subclass `0x06` (SCSI), protocol `0x50` (BOT).
- Obtener 2 bulk endpoints (IN, OUT).
- Cada comando: `CBW` (31 bytes) → data → `CSW` (13 bytes).

### 4.2 SCSI command set
Comandos mínimos:
- `INQUIRY` (0x12)
- `TEST UNIT READY` (0x00)
- `READ CAPACITY(10)` (0x25)
- `READ(10)` (0x28)
- `WRITE(10)` (0x2A)
- `REQUEST SENSE` (0x03)
- `MODE SENSE(6)` (0x1A)

Traducción a `block_device_t` que ya tienes en `block.h`.

### 4.3 Integración con block layer
- `blk_register()` con el nuevo dispositivo.
- `part_scan_all()` lo detectará en el próximo boot (o manualmente).
- FAT32 ya lo tienes, así que montar funciona directamente.

- **Test:** Insertar pendrive. Ver `/dev/sdX` en el log. Montar y leer un fichero.
- **Effort:** 5-7 días. El SCSI tiene muchos detalles (sense data, unit attention, etc.).

---

## FASE 5 — EHCI (3-4 días, opcional)
**Objetivo:** soportar placas antiguas o puertos USB 2.0 que no pasan por xHCI.
En tu Z790 probablemente no hay EHCI. Lo dejas para el final o lo omites.
- **Effort:** 3-4 días si lo haces.

## FASE 6 — OHCI/UHCI (muy opcional, 5-7 días)
Solo para hardware pre-2005. No merece la pena salvo que quieras correr Aurora en un Pentium 4.

## FASE 7 — Power management (opcional)
- Suspender/resumir puertos.
- Runtime PM (autosuspend).
- U3 (suspend) en xHCI.
*No urgente.*