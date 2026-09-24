// apps/win_blit_fault/main.c
// Regresión: WIN_BLIT no debe provocar un #PF fatal del kernel cuando el
// buffer de píxeles de userland deja de estar mapeado.
#include "../../syscall.h"

#define MMAP_PROT_READ  0x1
#define MMAP_PROT_WRITE 0x2

int main(void) {
  sys_print("[win_blit_fault] creando ventana...\n");

  int win = sys_win_create(0, 0, 64, 64, "WIN_BLIT fault test");
  if (win < 0) {
    sys_print("[win_blit_fault] ERROR: no se pudo crear la ventana\n");
    return 1;
  }

  uint32_t *pixels =
      (uint32_t *)sys_mmap(0, 4096, MMAP_PROT_READ | MMAP_PROT_WRITE, 0, -1, 0);
  if ((long)pixels < 0) {
    sys_print("[win_blit_fault] ERROR: mmap fallo\n");
    sys_win_destroy(win);
    return 1;
  }

  pixels[0] = 0x00123456u;

  int rc = sys_win_blit(win, 0, 0, 1, 1, 0, 0, 1, pixels);
  if (rc != 0) {
    sys_print("[win_blit_fault] ERROR: blit valido fallo\n");
    sys_munmap((uint64_t)pixels, 4096);
    sys_win_destroy(win);
    return 1;
  }

  if (sys_munmap((uint64_t)pixels, 4096) != 0) {
    sys_print("[win_blit_fault] ERROR: munmap fallo\n");
    sys_win_destroy(win);
    return 1;
  }

  /*
   * La direccion sigue dentro de USER_LIMIT, por lo que access_ok() debe
   * aceptarla. La página, sin embargo, ya no existe. WIN_BLIT debe convertir
   * el #PF en -EFAULT mediante copy_from_user(), sin matar al kernel.
   */
  rc = sys_win_blit(win, 0, 0, 1, 1, 0, 0, 1, pixels);
  if (rc != -14) {
    sys_print("[win_blit_fault] ERROR: blit de pagina no mapeada no devolvio -EFAULT\n");
    sys_win_destroy(win);
    return 1;
  }

  sys_print("[win_blit_fault] PASS: pagina no mapeada -> -EFAULT sin panic\n");
  sys_win_destroy(win);
  return 0;
}
