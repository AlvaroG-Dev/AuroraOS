// kernel/uaccess.c
#include "uaccess.h"
#include "cpu.h"
#include "serial.h"
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

// ---------------------------------------------------------------------------
// [FIX SMAP] Parchear `stac`/`clac` a NOP cuando la CPU no soporta SMAP.
//
// `stac` y `clac` son instrucciones SMAP-only. En CPUs sin SMAP
// (Nehalem, Core2, qemu64, cualquier cosa pre-Haswell) disparan #UD
// SIEMPRE, independientemente de si CR4.SMAP está a 0. Esto rompe
// raw_strncpy_from_user, raw_copy_from_user, raw_copy_to_user y los
// helpers put_user_* / get_user_* (que también emiten stac/clac desde
// el compilador si cpu_smap_enabled se resuelve como true en tiempo de
// compilación — no aquí, pero por si acaso).
//
// Linux usa la infraestructura `alternatives` para parchear en boot.
// Nosotros hacemos un recorrido lineal de .text, que es más sencillo
// y suficiente para un kernel pequeño. Los bytes a buscar son:
//
//   stac = 0F 01 CB  →  90 90 90 (3 NOPs)
//   clac = 0F 01 CA  →  90 90 90 (3 NOPs)
//
// El parcheo es seguro porque:
//   - Ocurre antes de ejecutar ningún código de usuario.
//   - x86 tiene coherencia de caché I/D para escrituras a memoria
//     ejecutable mapeada como RW (nuestro .text es RW en la ventana
//     del kernel durante el boot; se puede hacer WB sin flush).
//   - El patrón 0F 01 CB/CA es muy improbable que aparezca en otra
//     instrucción. En x86_64, `0F 01` cubre SGDT/SIDT/LGDT/LIDT/
//     SMSW/LMSW/INVLPG y algunas VMX. Los bytes CB/CA no son
//     válidos para ninguna de ellas.
//
// Llamar desde kmain() después de paging_init() y antes de arrancar
// cualquier proceso de usuario.
// ---------------------------------------------------------------------------
void uaccess_init(void) {
  extern int cpu_smap_enabled;
  extern uint8_t __text_start[];
  extern uint8_t __text_end[];
  extern void serial_puts(const char *s);

  if (cpu_smap_enabled) {
    serial_puts("[UACCESS] CPU con SMAP, no se parchea stac/clac\n");
    return;
  }

  uint8_t *p = __text_start;
  uint8_t *end = __text_end;
  unsigned int patched_stac = 0;
  unsigned int patched_clac = 0;

  while (p + 3 <= end) {
    if (p[0] == 0x0F && p[1] == 0x01) {
      if (p[2] == 0xCB) { // stac
        p[0] = 0x90;
        p[1] = 0x90;
        p[2] = 0x90;
        patched_stac++;
        p += 3;
        continue;
      } else if (p[2] == 0xCA) { // clac
        p[0] = 0x90;
        p[1] = 0x90;
        p[2] = 0x90;
        patched_clac++;
        p += 3;
        continue;
      }
    }
    p++;
  }

  // Mensaje sin printf (serial_puts no formatea).
  serial_puts("[UACCESS] Parcheo SMAP: stac=");
  {
    char buf[12];
    int n = 0;
    unsigned v = patched_stac;
    if (v == 0)
      buf[n++] = '0';
    while (v > 0) {
      buf[n++] = '0' + (v % 10);
      v /= 10;
    }
    while (n > 0)
      serial_putc(buf[--n]);
  }
  serial_puts(" clac=");
  {
    char buf[12];
    int n = 0;
    unsigned v = patched_clac;
    if (v == 0)
      buf[n++] = '0';
    while (v > 0) {
      buf[n++] = '0' + (v % 10);
      v /= 10;
    }
    while (n > 0)
      serial_putc(buf[--n]);
  }
  serial_puts("\n");
}