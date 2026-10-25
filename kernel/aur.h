// kernel/aur.h
#ifndef KERNEL_AUR_H
#define KERNEL_AUR_H

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// [FASE 4.1] Manifiesto .aur (user drivers).
//
// Un .aur es un ELF ET_DYN/ET_EXEC normal con una sección PROGBITS+ALLOC
// extra cuyo contenido empieza por struct aur_manifest. El loader la
// detecta por magic y la parsea.
//
// Layout del blob (todos los offsets desde el inicio del struct):
//   [struct aur_manifest]                       <- 64 bytes fijos
//   [name string + padding]                     <- NUL-terminated
//   [aur_bus_match_t array]
//   [devfd strings (cada uno padded a AUR_MAX_NAME)]
//   [ipc_service strings (idem AUR_MAX_SERVICE)]
// ---------------------------------------------------------------------------

#define AUR_MAGIC 0x41555221u // "AUR!" little-endian
#define AUR_ABI_VERSION 1u

#define AUR_MAX_NAME 32
#define AUR_MAX_SERVICE 32

// [FASE 4.2] Límites del loader. Si un manifiesto excede estos, se
// rechaza con -ENOEXEC. aurbuild también los respeta (MAX_*).
#define AUR_MAX_BUS_MATCH 16
#define AUR_MAX_DEVFDS 8
#define AUR_MAX_SERVICES 8

// Capability bits.
#define AUR_CAP_MMIO (1u << 0)
#define AUR_CAP_DMA (1u << 1)
#define AUR_CAP_IRQ (1u << 2)
#define AUR_CAP_IPC_SERVICE (1u << 3)
#define AUR_CAP_BUS_MATCH (1u << 4)
#define AUR_CAP_BLOCK_DEVICE (1u << 5)
#define AUR_CAP_NETIF (1u << 6)

// Tipo de bus para matching.
#define AUR_BUS_NONE 0
#define AUR_BUS_PCI 1
#define AUR_BUS_USB 2

// NOTA: sin __attribute__((packed)). Natural x86_64 alignment =
// 10 bytes (bus + pad + vendor + device + 4 bytes). aurbuild define
// exactamente lo mismo, sin packed, así que el layout coincide.
typedef struct {
  uint8_t bus; // AUR_BUS_*
  uint16_t vendor;
  uint16_t device;
  uint8_t class_id;
  uint8_t subclass_id;
  uint8_t class_mask; // 0 = match por vendor/device; != 0 = por class
  uint8_t _pad;
} aur_bus_match_t;

struct aur_manifest {
  uint32_t magic;       // AUR_MAGIC
  uint16_t abi_version; // AUR_ABI_VERSION
  uint16_t flags;       // reservado, 0
  uint32_t name_len;    // strlen(name) sin NUL
  uint32_t name_off;    // offset del NUL-terminated name[]
  uint32_t caps;        // AUR_CAP_*
  uint32_t bus_match_count;
  uint32_t bus_match_off; // array de aur_bus_match_t
  uint32_t devfd_count;
  uint32_t devfd_off; // array de char[AUR_MAX_NAME]
  uint32_t ipc_service_count;
  uint32_t ipc_service_off; // array de char[AUR_MAX_SERVICE]
  uint32_t ring_per_cpu;    // 0 = 1, N > 0 = N anillos por CPU
  uint32_t reserved[4];
};

// ---------------------------------------------------------------------------
// [FASE 4.5] /dev/irq/<N>
//
// Struct que devuelve read(/dev/irq/<N>) — 8 bytes exactos.
//   irq   = número de IRQ del nodo (0..15 legacy, 0x60..0x6F MSI)
//   count = nº de IRQs ocurridas desde el último read (se resetea a 0)
//
// El .aur compara count con su propio valor anterior para detectar
// coalescing. El hardware normalmente ya coalesce por pending bits, así
// que el patrón recomendado es "read → procesar todo lo pendiente en el
// device → volver a read".
// ---------------------------------------------------------------------------
struct aur_irq_event {
  uint32_t irq;
  uint32_t count;
};

// ioctls de /dev/irq/<N>
// (reservados; solo tienen efecto para IRQs legacy 0..15)
#define AUR_IRQ_MASK 0x300   // enmascarar en IOAPIC
#define AUR_IRQ_UNMASK 0x301 // desenmascarar en IOAPIC

// ---------------------------------------------------------------------------
// [FASE 5.1] /dev/blkproxy — puente block layer <-> driver de bloque .aur.
//
// Requiere AUR_CAP_BLOCK_DEVICE. Un único proceso puede tenerlo abierto.
// Todo el tráfico va por ioctl (los chardev de read/write del VFS trocean
// a 4 KiB, inservible para transferir bios):
//
//   BLKP_REGISTER (struct aur_blk_register*)
//       Registra un disco en el block layer (nombre sdX asignado por el
//       kernel). Devuelve disk_id y el nombre.
//   BLKP_READY (0)
//       "Ya he registrado todos mis discos". Desbloquea el arranque, que
//       espera a los discos de usuario antes de escanear particiones.
//   BLKP_FETCH (struct aur_blk_fetch*)
//       Bloquea (salvo AUR_BLK_NOWAIT → -EAGAIN) hasta que haya una
//       petición. Rellena req; si es BIO_WRITE copia los datos a buf.
//       Los bios mayores que max_sectors se trocean en el kernel.
//   BLKP_COMPLETE (struct aur_blk_done*)
//       Completa la petición `tag`. Si era lectura y error == 0 copia
//       los datos desde buf. error = 0 o -errno.
//
// Si el proceso cierra el fd (o muere), todos los bios pendientes fallan
// con -EIO y los discos quedan dados de baja.
// ---------------------------------------------------------------------------
#define AUR_BLK_OP_READ 0
#define AUR_BLK_OP_WRITE 1
#define AUR_BLK_OP_FLUSH 2

#define AUR_BLK_RO (1u << 0)     // flags de aur_blk_register
#define AUR_BLK_NOWAIT (1u << 0) // flags de aur_blk_fetch

#define BLKP_REGISTER 0x400
#define BLKP_READY 0x401
#define BLKP_FETCH 0x402
#define BLKP_COMPLETE 0x403

struct aur_blk_register {
  char name[16];        // out: nombre asignado ("sda", ...)
  uint64_t num_sectors; // in
  uint32_t sector_size; // in: 512 o 4096 (potencia de 2, 512..4096)
  uint32_t max_sectors; // in: máx. sectores por petición (0 = 128)
  uint32_t flags;       // in: AUR_BLK_*
  uint32_t disk_id;     // out
};

struct aur_blk_req {
  uint64_t tag;
  uint64_t lba; // relativo al disco (ya sumado el offset si fuera partición)
  uint32_t disk_id;
  uint32_t op; // AUR_BLK_OP_*
  uint32_t count; // sectores (0 para flush)
  uint32_t _pad;
};

struct aur_blk_fetch {
  struct aur_blk_req req; // out
  uint64_t buf;           // in: puntero de usuario (datos de escritura)
  uint64_t buf_size;      // in: bytes disponibles en buf
  uint32_t flags;         // in: AUR_BLK_NOWAIT
  uint32_t _pad;
};

struct aur_blk_done {
  uint64_t tag;
  int32_t error;
  uint32_t _pad;
  uint64_t buf;      // in: puntero de usuario (datos de lectura)
  uint64_t buf_size; // in: bytes válidos en buf
};

_Static_assert(sizeof(struct aur_blk_register) == 40,
               "aur_blk_register debe ser 40 bytes");
_Static_assert(sizeof(struct aur_blk_req) == 32,
               "aur_blk_req debe ser 32 bytes");
_Static_assert(sizeof(struct aur_blk_fetch) == 56,
               "aur_blk_fetch debe ser 56 bytes");
_Static_assert(sizeof(struct aur_blk_done) == 32,
               "aur_blk_done debe ser 32 bytes");

// ---------------------------------------------------------------------------
// [FASE 5.4] /dev/usbproxy — puente usb_hcd <-> controlador xHCI en Ring 3.
//
// Requiere AUR_CAP_USB_HCD; un único propietario. El kernel conserva usb_core,
// hubs y drivers de clase; el .aur implementa el controlador (anillos,
// contextos, comandos, eventos, reset de puertos). Cada operación del
// usb_hcd_t del kernel se convierte en una petición que el .aur atiende de
// forma ASÍNCRONA (varias a la vez; las de interrupt IN de HID pueden tardar
// minutos). Como en /dev/blkproxy todo va por ioctl:
//
//   UPROXY_HELLO (struct aur_usb_hello*)  controlador listo; registra el HCD.
//   UPROXY_FETCH (struct aur_usb_fetch*)  siguiente petición (+ datos OUT).
//   UPROXY_COMPLETE (struct aur_usb_done*) resultado (+ datos IN).
//   UPROXY_EVENT (AUR_USB_EVT_*)          aviso asíncrono del controlador.
//
// Los datos viajan por copia con un buffer de usuario (que el .aur reserva en
// DMA); el kernel trocea las transferencias a AUR_USB_MAX_XFER.
// ---------------------------------------------------------------------------
#define AUR_CAP_USB_HCD (1u << 7)

#define AUR_USB_OP_NEW_DEVICE 1   // out0=slot, out1=mps de EP0
#define AUR_USB_OP_FREE_DEVICE 2  // Disable Slot
#define AUR_USB_OP_CONFIGURE_EP 3 // ep_addr/ep_type/max_packet/interval
#define AUR_USB_OP_SET_HUB 4      // num_ports
#define AUR_USB_OP_CONTROL 5      // setup[], len, is_in
#define AUR_USB_OP_XFER 6         // bulk/interrupt: ep_addr, len, is_in
#define AUR_USB_OP_PORT_STATUS 7  // out0=conectado, out1=velocidad
#define AUR_USB_OP_PORT_RESET 8   // out1=velocidad tras el reset

#define AUR_USB_EVT_PORT_CHANGE 1

#define AUR_USB_NOWAIT (1u << 0)
#define AUR_USB_MAX_XFER 65536u

#define UPROXY_HELLO 0x500
#define UPROXY_FETCH 0x501
#define UPROXY_COMPLETE 0x502
#define UPROXY_EVENT 0x503

struct aur_usb_hello {
  uint32_t max_ports;
  uint32_t max_slots;
};

struct aur_usb_req {
  uint64_t tag;
  uint32_t op; // AUR_USB_OP_*
  uint32_t slot;
  uint32_t timeout_ms; // 0 = sin límite
  uint32_t len;        // bytes de datos
  uint32_t route;      // route string (NEW_DEVICE)
  uint16_t max_packet;
  uint8_t is_in;
  uint8_t ep_addr;
  uint8_t ep_type; // USB_EP_* de usb_core.h (2 bulk, 3 interrupt)
  uint8_t interval;
  uint8_t speed; // usb_speed_t de usb_core.h
  uint8_t port;  // puerto del padre (NEW_DEVICE) / puerto raíz (PORT_*)
  uint8_t root_port;
  uint8_t parent_slot; // 0 = el padre es el root hub
  uint8_t parent_speed;
  uint8_t num_ports; // SET_HUB
  uint8_t setup[8];
  uint8_t _pad[16];
};

struct aur_usb_fetch {
  struct aur_usb_req req; // out
  uint64_t buf;           // in: dónde copiar los datos OUT
  uint64_t buf_size;
  uint32_t flags; // AUR_USB_NOWAIT
  uint32_t _pad;
};

struct aur_usb_done {
  uint64_t tag;
  int32_t status; // 0 o -errno
  uint32_t actual; // bytes transferidos
  uint32_t out0;
  uint32_t out1;
  uint64_t buf; // datos IN
  uint64_t buf_size;
};

_Static_assert(sizeof(struct aur_usb_req) == 64,
               "aur_usb_req debe ser 64 bytes");
_Static_assert(sizeof(struct aur_usb_fetch) == 88,
               "aur_usb_fetch debe ser 88 bytes");
_Static_assert(sizeof(struct aur_usb_done) == 40,
               "aur_usb_done debe ser 40 bytes");

_Static_assert(sizeof(struct aur_manifest) == 64,
               "aur_manifest debe ser 64 bytes");
_Static_assert(sizeof(aur_bus_match_t) == 10,
               "aur_bus_match_t debe ser 10 bytes en x86_64");

#endif // KERNEL_AUR_H