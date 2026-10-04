// kernel/sysfs.h
#ifndef KERNEL_SYSFS_H
#define KERNEL_SYSFS_H

#include <stdint.h>

struct vfs_fs_ops;

// Devuelve las vfs_fs_ops de sysfs para vfs_mount("/sys", ...).
struct vfs_fs_ops *sysfs_get_vfs_ops(void);

// Pre-enumera los dispositivos PCI y los cachea. Llamar UNA VEZ
// durante el boot, DESPUÉS de pci_init() (o pci_enumerate, como se
// llame en tu árbol). Idempotente.
void sysfs_init(void);

// ---------------------------------------------------------------------------
// [FIX] API pública para que procfs pueda enumerar los mismos dispositivos
// que sysfs. Busybox lspci lee /proc/bus/pci/devices, que debe contener
// la misma lista.
// ---------------------------------------------------------------------------
struct sysfs_pci_info {
  uint8_t bus, slot, func;
  uint16_t vendor, device;
  uint8_t class, subclass, prog_if, revision;
  uint16_t subsys_vendor, subsys_device;
};

int sysfs_pci_count(void);
const struct sysfs_pci_info *sysfs_pci_get(int i);

// ---------------------------------------------------------------------------
// [FIX] Lee config space (256 bytes) de un dispositivo PCI ya enumerado.
// La usa procfs para servir /proc/bus/pci/<bus>/<devfn>.
// Devuelve 0 si OK, -ENOENT si el dispositivo no está en la tabla,
// -EINVAL si out==NULL.
// ---------------------------------------------------------------------------
int sysfs_pci_read_config(uint8_t bus, uint8_t slot, uint8_t func,
                          uint8_t out[256]);
                          
#endif