// kernel/aur_bus.c
#include "aur_bus.h"
#include "aur_loader.h"
#include "elf.h"
#include "klog.h"
#include "pci.h"
#include "process.h"
#include "sched.h"
#include "string.h"
#include "uaccess.h"
#include "vfs.h"

// Wrapper para leer un ELF desde el VFS (necesario para el parser)
static int64_t aur_read_vfs(void *ctx, uint64_t offset, size_t size,
                            void *buf) {
  vfs_node_t *node = (vfs_node_t *)ctx;
  if (!node || !node->ops || !node->ops->read)
    return -1;
  return node->ops->read(node, offset, size, buf);
}

// Comprueba si un dispositivo físico coincide con los IDs de un manifiesto .aur
static int aur_bus_match_manifest(struct device *dev,
                                  const struct aur_module_info *info) {
  if (!dev || !info || info->bus_match_count == 0)
    return 0;

  for (int i = 0; i < info->bus_match_count; i++) {
    const aur_bus_match_t *m = &info->bus_match[i];

    // Match para bus PCI
    if (m->bus == AUR_BUS_PCI && strcmp(dev->bus->name, "pci") == 0) {
      pci_device_t *pdev = (pci_device_t *)dev->priv;
      if (!pdev)
        continue;

      // Si el manifiesto especifica vendor/device y no coinciden, saltar
      if (m->vendor != 0 && m->vendor != pdev->vendor_id)
        continue;
      if (m->device != 0 && m->device != pdev->device_id)
        continue;

      // Si se especifica class_mask, comprobar clase y subclase
      if (m->class_mask != 0) {
        if ((pdev->class_id & m->class_mask) != (m->class_id & m->class_mask))
          continue;
        if ((pdev->subclass_id & m->class_mask) !=
            (m->subclass_id & m->class_mask))
          continue;
      }

      return 1; // ¡Match!
    }
  }
  return 0;
}

// Función probe: se llama cuando el bus decide probar este driver contra un
// dispositivo
static int aur_bus_probe(struct device *dev) {
  LOG_INFO("[AUR-BUS] Buscando driver para %s...", dev->name);

  vfs_node_t *dir = vfs_lookup("/usr/lib/aur");
  if (!dir) {
    LOG_WARN("[AUR-BUS] Directorio /usr/lib/aur no existe");
    return -ENODEV;
  }
  if (!(dir->flags & VFS_DIRECTORY)) {
    vfs_node_free(dir);
    LOG_WARN("[AUR-BUS] /usr/lib/aur no es un directorio");
    return -ENODEV;
  }

  vfs_dirent_t ent;
  int matched = 0;

  for (uint64_t i = 0; i < 1024; i++) {
    if (vfs_readdir_node(dir, i, &ent) != 0 || ent.name[0] == '\0')
      break;

    size_t nlen = strlen(ent.name);
    if (nlen < 5)
      continue; // Mínimo "x.aur"
    if (strcmp(ent.name + nlen - 4, ".aur") != 0)
      continue;

    char full_path[VFS_PATH_MAX] = "/usr/lib/aur/";
    size_t base_len = strlen(full_path);
    if (base_len + nlen + 1 > sizeof(full_path))
      continue;
    memcpy(full_path + base_len, ent.name, nlen + 1);

    LOG_INFO("[AUR-BUS] Analizando %s...", full_path);

    vfs_node_t *node = vfs_lookup(full_path);
    if (!node)
      continue;

    struct aur_module_info info;
    memset(&info, 0, sizeof(info));

    int rc = aur_loader_parse_streaming(aur_read_vfs, node, node->size, &info);
    if (rc == 0) {
      if (aur_bus_match_manifest(dev, &info)) {
        LOG_INFO("[AUR-BUS] Match encontrado! Lanzando %s", full_path);
        process_t *p = process_load(full_path);
        if (p) {
          matched = 1;
          vfs_node_free(node);
          break;
        } else {
          LOG_ERR("[AUR-BUS] Fallo al lanzar %s", full_path);
        }
      } else {
        LOG_INFO("[AUR-BUS] %s no coincide con %s", full_path, dev->name);
      }
    }
    vfs_node_free(node);
  }

  vfs_node_free(dir);

  if (!matched) {
    LOG_WARN("[AUR-BUS] Ningún .aur reclamó %s", dev->name);
  }

  return matched ? 0 : -ENODEV;
}

// Tabla de IDs. Al usar PCI_ANY_ID, este driver acepta probar CUALQUIER
// dispositivo PCI que haya sido rechazado por los drivers del kernel.
static const pci_device_id_t aur_pci_id_table[] = {
    {
        .vendor_id = PCI_ANY_ID,
        .device_id = PCI_ANY_ID,
        .subvendor = PCI_ANY_ID, // <-- FALTABA
        .subdevice = PCI_ANY_ID  // <-- FALTABA
    },
    {0}};

struct driver aur_bus_driver = {
    .name = "aur_bus",
    .bus = &pci_bus,
    .id_table = aur_pci_id_table,
    .probe = aur_bus_probe,
};