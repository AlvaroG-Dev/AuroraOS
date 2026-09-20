// kernel/uaccess.h
#ifndef KERNEL_UACCESS_H
#define KERNEL_UACCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Límite del espacio de usuario. Por debajo de esta dirección se asume
// que un puntero puede ser de userland.
#define USER_LIMIT 0x0000800000000000ULL

// errnos básicos (negativos).
#define EPERM 1
#define ENOENT 2
#define EIO 5
#define EBADF 9
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENOSPC 28
#define EROFS 30
#define ERANGE 34
#define ENOSYS 38
#define ETIMEDOUT 110

// Comprueba que [p, p+n) está dentro del espacio de usuario.
static inline bool access_ok(const void *p, size_t n) {
  uint64_t a = (uint64_t)p;
  // Un puntero de userland nunca es 0 si n > 0, y el rango debe caber
  // por debajo de USER_LIMIT.
  return a < USER_LIMIT && n <= (USER_LIMIT - a);
}

// Copia de/a userland. Devuelven 0 si OK, -EFAULT si falla.
long copy_from_user(void *dst, const void *src, size_t n);
long copy_to_user(void *dst, const void *src, size_t n);

// Copia una string de userland a un buffer del kernel, hasta max-1 bytes,
// terminando con '\0'. Devuelve la longitud (sin contar '\0') o -EFAULT.
long strncpy_from_user(char *dst, const char *src, size_t max);

// put/get de tamaños fijos. Devuelven 0 si OK, -EFAULT si el puntero
// es inválido o hay fallo de página no recuperable.
long put_user_u8(uint8_t *dst, uint8_t val);
long put_user_u16(uint16_t *dst, uint16_t val);
long put_user_u32(uint32_t *dst, uint32_t val);
long put_user_u64(uint64_t *dst, uint64_t val);

long get_user_u8(uint8_t *dst, const uint8_t *src);
long get_user_u16(uint16_t *dst, const uint16_t *src);
long get_user_u32(uint32_t *dst, const uint32_t *src);
long get_user_u64(uint64_t *dst, const uint64_t *src);

// Símbolos definidos por uaccess_faults.asm y el linker script.
extern const uint64_t __uaccess_table_start[];
extern const uint64_t __uaccess_table_end[];

// Busca (fault_rip, fixup_rip) en la tabla. Devuelve el fixup o 0.
uint64_t uaccess_lookup_fixup(uint64_t fault_rip);

#endif