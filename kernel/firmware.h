// kernel/firmware.h
//
// Cargador de firmware al estilo Linux (request_firmware) para drivers de
// kernel (.kur) y del propio kernel. Busca `name` en /lib/firmware/updates y
// /lib/firmware (el contenido de linux-firmware se copia alli). Un driver .aur
// (Ring 3) puede simplemente abrir esos ficheros.

#ifndef KERNEL_FIRMWARE_H
#define KERNEL_FIRMWARE_H

#include <stddef.h>
#include <stdint.h>

#define FIRMWARE_MAX_SIZE (64u << 20) // 64 MiB

struct firmware {
  size_t size;
  const uint8_t *data; // kmalloc'd; liberado por release_firmware()
};

// 0 y *fw relleno, -ENOENT si no existe, -EINVAL (nombre con ".." o absoluto),
// -E2BIG (> FIRMWARE_MAX_SIZE), -ENOMEM.
int request_firmware(const struct firmware **fw, const char *name);
void release_firmware(const struct firmware *fw);

#endif
