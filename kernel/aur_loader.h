// kernel/aur_loader.h
#ifndef KERNEL_AUR_LOADER_H
#define KERNEL_AUR_LOADER_H

#include "aur.h"
#include "elf.h"     // elf_read_fn (misma firma que necesitamos)

#include <stddef.h>
#include <stdint.h>

// Representación en memoria del manifiesto .aur de un binario.
// Todo en host order. Si `name[0] == '\0'`, el binario no es un .aur.
//
// Vive inline en process_t: 724 bytes por proceso. Como process_t ya
// pesa varios KB (sigactions, fds, argv, envp), añadir 724 B es marginal.
struct aur_module_info {
  char     name[AUR_MAX_NAME];
  uint32_t caps;
  uint32_t ring_per_cpu;
  int32_t  devfd_count;
  char     devfds[AUR_MAX_DEVFDS][AUR_MAX_NAME];
  int32_t  ipc_count;
  char     ipc_services[AUR_MAX_SERVICES][AUR_MAX_SERVICE];
  int32_t  bus_match_count;
  aur_bus_match_t bus_match[AUR_MAX_BUS_MATCH];
};

// Parsea la sección .aur de un ELF via streaming (sin cargar el ELF
// entero; 3-4 reads pequeños).
//
// Devuelve:
//   0         .aur válido; `*out` queda relleno
//   -ENOENT   sin sección .aur; `*out` queda zeroed (no es .aur)
//   -ENOEXEC  sección .aur presente pero malformada (magic OK pero
//             offsets, versión, tamaños, etc. inválidos)
//   -EIO      error de lectura del callback
//   -EINVAL   argumentos inválidos (read o out NULL)
//
// `read` es exactamente elf_read_fn (misma firma y ABI), así que los
// llamantes pueden pasar su propia función sin cast.
int aur_loader_parse_streaming(elf_read_fn read, void *ctx,
                               uint64_t file_size,
                               struct aur_module_info *out);

#endif  // KERNEL_AUR_LOADER_H