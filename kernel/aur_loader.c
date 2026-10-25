// kernel/aur_loader.c
#include "aur_loader.h"
#include "elf.h"
#include "heap.h"
#include "klog.h"
#include "string.h"
#include "uaccess.h"

#include <stddef.h>
#include <stdint.h>

// elf.h define Elf64_Ehdr/Elf64_Shdr y ELFMAG, pero no las constantes
// SHT_*. kur_loader.c las define localmente; hacemos lo mismo para no
// tocar elf.h (que es estable).
#define SHT_PROGBITS 1

// Tamaño máximo de sección .aur que aceptamos. 4 KB cubre todos los
// manifiestos razonables (name + hasta 16 bus_match + 8 devfd + 8 ipc).
#define AUR_MAX_BLOB_SIZE 4096

static int aur_parse_blob(const void *blob, size_t blob_size,
                          struct aur_module_info *out) {
  if (blob_size < sizeof(struct aur_manifest))
    return -ENOEXEC;

  const struct aur_manifest *mf = (const struct aur_manifest *)blob;

  if (mf->magic != AUR_MAGIC)
    return -ENOEXEC;
  if (mf->abi_version != AUR_ABI_VERSION) {
    LOG_WARN("[AUR] abi v%u, kernel v%u", mf->abi_version, AUR_ABI_VERSION);
    return -ENOEXEC;
  }
  if (mf->name_len == 0 || mf->name_len >= AUR_MAX_NAME)
    return -ENOEXEC;
  if (mf->name_off + (uint64_t)mf->name_len + 1 > blob_size)
    return -ENOEXEC;

  const char *nm = (const char *)blob + mf->name_off;
  if (nm[mf->name_len] != '\0')
    return -ENOEXEC;

  memset(out, 0, sizeof(*out));
  memcpy(out->name, nm, mf->name_len);
  out->name[mf->name_len] = '\0';
  out->caps = mf->caps;
  out->ring_per_cpu = mf->ring_per_cpu;

  if (mf->bus_match_count > 0) {
    if (mf->bus_match_count > AUR_MAX_BUS_MATCH)
      return -ENOEXEC;
    uint64_t need = (uint64_t)mf->bus_match_count * sizeof(aur_bus_match_t);
    if (mf->bus_match_off + need > blob_size)
      return -ENOEXEC;
    memcpy(out->bus_match, (const char *)blob + mf->bus_match_off, need);
    out->bus_match_count = (int32_t)mf->bus_match_count;
  }

  if (mf->devfd_count > 0) {
    if (mf->devfd_count > AUR_MAX_DEVFDS)
      return -ENOEXEC;
    uint64_t need = (uint64_t)mf->devfd_count * AUR_MAX_NAME;
    if (mf->devfd_off + need > blob_size)
      return -ENOEXEC;
    memcpy(out->devfds, (const char *)blob + mf->devfd_off, need);
    for (uint32_t i = 0; i < mf->devfd_count; i++) {
      int ok = 0;
      for (int j = 0; j < AUR_MAX_NAME; j++)
        if (out->devfds[i][j] == '\0') { ok = 1; break; }
      if (!ok)
        return -ENOEXEC;
    }
    out->devfd_count = (int32_t)mf->devfd_count;
  }

  if (mf->ipc_service_count > 0) {
    if (mf->ipc_service_count > AUR_MAX_SERVICES)
      return -ENOEXEC;
    uint64_t need = (uint64_t)mf->ipc_service_count * AUR_MAX_SERVICE;
    if (mf->ipc_service_off + need > blob_size)
      return -ENOEXEC;
    memcpy(out->ipc_services, (const char *)blob + mf->ipc_service_off, need);
    for (uint32_t i = 0; i < mf->ipc_service_count; i++) {
      int ok = 0;
      for (int j = 0; j < AUR_MAX_SERVICE; j++)
        if (out->ipc_services[i][j] == '\0') { ok = 1; break; }
      if (!ok)
        return -ENOEXEC;
    }
    out->ipc_count = (int32_t)mf->ipc_service_count;
  }

  return 0;
}

int aur_loader_parse_streaming(elf_read_fn read, void *ctx,
                               uint64_t file_size,
                               struct aur_module_info *out) {
  if (!read || !out)
    return -EINVAL;
  memset(out, 0, sizeof(*out));

  if (file_size < sizeof(Elf64_Ehdr))
    return -ENOENT;

  Elf64_Ehdr ehdr;
  if (read(ctx, 0, sizeof(ehdr), &ehdr) != (int64_t)sizeof(ehdr))
    return -EIO;
  if (memcmp(ehdr.e_ident, ELFMAG, 4) != 0)
    return -ENOENT;
  if (ehdr.e_ident[EI_CLASS] != ELFCLASS64 ||
      ehdr.e_ident[EI_DATA] != ELFDATA2LSB ||
      ehdr.e_machine != EM_X86_64)
    return -ENOENT;

  if (ehdr.e_shoff == 0 || ehdr.e_shnum == 0 ||
      ehdr.e_shentsize != sizeof(Elf64_Shdr))
    return -ENOENT;
  if (ehdr.e_shnum > 256)
    return -ENOEXEC;
  uint64_t sh_bytes = (uint64_t)ehdr.e_shnum * sizeof(Elf64_Shdr);
  if (ehdr.e_shoff + sh_bytes > file_size)
    return -ENOEXEC;

  Elf64_Shdr *shdrs = (Elf64_Shdr *)kmalloc((size_t)sh_bytes);
  if (!shdrs)
    return -ENOMEM;
  if (read(ctx, ehdr.e_shoff, (size_t)sh_bytes, shdrs) != (int64_t)sh_bytes) {
    kfree(shdrs);
    return -EIO;
  }

  for (int i = 0; i < ehdr.e_shnum; i++) {
    if (shdrs[i].sh_type != SHT_PROGBITS)
      continue;
    if (shdrs[i].sh_size < sizeof(struct aur_manifest))
      continue;
    if (shdrs[i].sh_size > AUR_MAX_BLOB_SIZE)
      continue;
    if (shdrs[i].sh_offset + shdrs[i].sh_size > file_size)
      continue;

    // Peek the magic to filter out non-.aur sections cheaply.
    struct aur_manifest mf;
    if (read(ctx, shdrs[i].sh_offset, sizeof(mf), &mf) != (int64_t)sizeof(mf))
      continue;
    if (mf.magic != AUR_MAGIC)
      continue;

    // It's a .aur section. Read the whole blob and parse it.
    uint8_t *blob = (uint8_t *)kmalloc((size_t)shdrs[i].sh_size);
    if (!blob) {
      kfree(shdrs);
      return -ENOMEM;
    }
    if (read(ctx, shdrs[i].sh_offset, shdrs[i].sh_size, blob) !=
        (int64_t)shdrs[i].sh_size) {
      kfree(blob);
      kfree(shdrs);
      return -EIO;
    }
    int rc = aur_parse_blob(blob, shdrs[i].sh_size, out);
    kfree(blob);
    kfree(shdrs);
    return rc;
  }

  kfree(shdrs);
  return -ENOENT;
}

// [FASE 4.7] ¿Coincide un dispositivo PCI con una entrada del manifiesto?
// Misma regla que usa aur_bus para decidir a quién lanzar.
int aur_match_pci(const aur_bus_match_t *m, uint16_t vendor, uint16_t device,
                  uint8_t class_id, uint8_t subclass_id) {
  if (m->bus != AUR_BUS_PCI)
    return 0;
  if (m->vendor != 0 && m->vendor != vendor)
    return 0;
  if (m->device != 0 && m->device != device)
    return 0;
  if (m->class_mask != 0) {
    if ((class_id & m->class_mask) != (m->class_id & m->class_mask))
      return 0;
    if ((subclass_id & m->class_mask) != (m->subclass_id & m->class_mask))
      return 0;
  }
  return 1;
}

// ¿Puede este .aur mapear los BARs de ese dispositivo? Con manifiesto sin
// entradas de bus (p. ej. aur_selftest, lanzado a mano) no hay restricción;
// si declara dispositivos, solo esos.
int aur_info_allows_pci(const struct aur_module_info *info, uint16_t vendor,
                        uint16_t device, uint8_t class_id,
                        uint8_t subclass_id) {
  if (info->bus_match_count <= 0)
    return 1;
  for (int i = 0; i < info->bus_match_count; i++)
    if (aur_match_pci(&info->bus_match[i], vendor, device, class_id,
                      subclass_id))
      return 1;
  return 0;
}
