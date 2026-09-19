// kernel/uaccess.c
#include "uaccess.h"
#include "cpu.h"
#include "string.h"
#include <stddef.h>

// Primitivas definidas en uaccess_asm.asm.
extern long raw_copy_from_user(void *dst, const void *src, size_t n);
extern long raw_copy_to_user(void *dst, const void *src, size_t n);
extern long raw_strncpy_from_user(char *dst, const char *src, size_t max);

// ---------------------------------------------------------------------------
// Búsqueda en la tabla de fixups.
// ---------------------------------------------------------------------------
uint64_t uaccess_lookup_fixup(uint64_t fault_rip) {
  const uint64_t *p = __uaccess_table_start;
  const uint64_t *end = __uaccess_table_end;
  while (p + 1 < end) {
    uint64_t from = p[0];
    uint64_t to = p[1];
    if (from == 0 && to == 0)
      break;
    // Margen de 16 bytes: por si la instrucción que falla tiene prefijos
    // o el rip apunta justo después de la instrucción.
    if (fault_rip >= from && fault_rip < from + 16)
      return to;
    p += 2;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------
long copy_from_user(void *dst, const void *src, size_t n) {
  if (n == 0)
    return 0;
  if (!access_ok(src, n))
    return -EFAULT;
  // El dst es del kernel: no hace falta access_ok.
  long rem = raw_copy_from_user(dst, src, n);
  return rem == 0 ? 0 : -EFAULT;
}

long copy_to_user(void *dst, const void *src, size_t n) {
  if (n == 0)
    return 0;
  if (!access_ok(dst, n))
    return -EFAULT;
  long rem = raw_copy_to_user(dst, src, n);
  return rem == 0 ? 0 : -EFAULT;
}

long strncpy_from_user(char *dst, const char *src, size_t max) {
  if (max == 0)
    return -EFAULT;
  if (!access_ok(src, 1))
    return -EFAULT;
  // No validamos max bytes porque la string puede terminar antes; el
  // asm hace las lecturas byte a byte y cada una tiene su fixup.
  long n = raw_strncpy_from_user(dst, src, max);
  return n; // >=0 OK, -EFAULT si falló
}

// ---------------------------------------------------------------------------
// put/get de tamaños fijos. Sin tabla de fixups: usan stac/clac y una
// comprobación previa con access_ok. Un puntero válido por access_ok
// puede aún fallar si la página no está mapeada, pero en ese caso el
// usuario merece un -EFAULT y aquí no lo detectamos: preferimos que el
// handler de #PF lo resuelva como demand paging (ver pf.c).
//
// En la práctica, para puntos que sabemos que el usuario va a tocar
// (return values de syscalls, etc.), es mejor usar copy_to_user.
// ---------------------------------------------------------------------------
long put_user_u8(uint8_t *dst, uint8_t val) {
  if (!access_ok(dst, 1))
    return -EFAULT;
  stac();
  *dst = val;
  clac();
  return 0;
}
long put_user_u16(uint16_t *dst, uint16_t val) {
  if (!access_ok(dst, 2))
    return -EFAULT;
  stac();
  *dst = val;
  clac();
  return 0;
}
long put_user_u32(uint32_t *dst, uint32_t val) {
  if (!access_ok(dst, 4))
    return -EFAULT;
  stac();
  *dst = val;
  clac();
  return 0;
}
long put_user_u64(uint64_t *dst, uint64_t val) {
  if (!access_ok(dst, 8))
    return -EFAULT;
  stac();
  *dst = val;
  clac();
  return 0;
}
long get_user_u8(uint8_t *dst, const uint8_t *src) {
  if (!access_ok(src, 1))
    return -EFAULT;
  stac();
  *dst = *src;
  clac();
  return 0;
}
long get_user_u16(uint16_t *dst, const uint16_t *src) {
  if (!access_ok(src, 2))
    return -EFAULT;
  stac();
  *dst = *src;
  clac();
  return 0;
}
long get_user_u32(uint32_t *dst, const uint32_t *src) {
  if (!access_ok(src, 4))
    return -EFAULT;
  stac();
  *dst = *src;
  clac();
  return 0;
}
long get_user_u64(uint64_t *dst, const uint64_t *src) {
  if (!access_ok(src, 8))
    return -EFAULT;
  stac();
  *dst = *src;
  clac();
  return 0;
}