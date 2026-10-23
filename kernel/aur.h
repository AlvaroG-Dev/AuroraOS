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

#define AUR_MAGIC       0x41555221u   // "AUR!" little-endian
#define AUR_ABI_VERSION 1u

#define AUR_MAX_NAME    32
#define AUR_MAX_SERVICE 32

// [FASE 4.2] Límites del loader. Si un manifiesto excede estos, se
// rechaza con -ENOEXEC. aurbuild también los respeta (MAX_*).
#define AUR_MAX_BUS_MATCH 16
#define AUR_MAX_DEVFDS    8
#define AUR_MAX_SERVICES  8

// Capability bits.
#define AUR_CAP_MMIO         (1u << 0)
#define AUR_CAP_DMA          (1u << 1)
#define AUR_CAP_IRQ          (1u << 2)
#define AUR_CAP_IPC_SERVICE  (1u << 3)
#define AUR_CAP_BUS_MATCH    (1u << 4)
#define AUR_CAP_BLOCK_DEVICE (1u << 5)
#define AUR_CAP_NETIF        (1u << 6)

// Tipo de bus para matching.
#define AUR_BUS_NONE 0
#define AUR_BUS_PCI  1
#define AUR_BUS_USB  2

// NOTA: sin __attribute__((packed)). Natural x86_64 alignment =
// 10 bytes (bus + pad + vendor + device + 4 bytes). aurbuild define
// exactamente lo mismo, sin packed, así que el layout coincide.
typedef struct {
  uint8_t  bus;              // AUR_BUS_*
  uint16_t vendor;
  uint16_t device;
  uint8_t  class_id;
  uint8_t  subclass_id;
  uint8_t  class_mask;       // 0 = match por vendor/device; != 0 = por class
  uint8_t  _pad;
} aur_bus_match_t;

struct aur_manifest {
  uint32_t magic;               // AUR_MAGIC
  uint16_t abi_version;         // AUR_ABI_VERSION
  uint16_t flags;               // reservado, 0
  uint32_t name_len;            // strlen(name) sin NUL
  uint32_t name_off;            // offset del NUL-terminated name[]
  uint32_t caps;                // AUR_CAP_*
  uint32_t bus_match_count;
  uint32_t bus_match_off;       // array de aur_bus_match_t
  uint32_t devfd_count;
  uint32_t devfd_off;           // array de char[AUR_MAX_NAME]
  uint32_t ipc_service_count;
  uint32_t ipc_service_off;     // array de char[AUR_MAX_SERVICE]
  uint32_t ring_per_cpu;        // 0 = 1, N > 0 = N anillos por CPU
  uint32_t reserved[4];
};

_Static_assert(sizeof(struct aur_manifest) == 64,
               "aur_manifest debe ser 64 bytes");
_Static_assert(sizeof(aur_bus_match_t) == 10,
               "aur_bus_match_t debe ser 10 bytes en x86_64");

#endif  // KERNEL_AUR_H