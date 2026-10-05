// user/musl/memhog.c
//
// Consume RAM y mantiene las páginas residentes para forzar swap.
// Útil para validar el subsistema de swap end-to-end:
//   1) Reserva N MB anónimos con mmap.
//   2) Escribe un valor único en cada página (identifica cada página).
//   3) Duerme para dar tiempo a kswapd a reclamar.
//   4) Verifica que cada página conserva su valor (swap-in correcto).
//
// Uso:
//   memhog [MB]        (default 400)
//
// Salida:
//   "verificacion completa, 0 errores"  → OK
//   "verificacion completa, N errores"  → swap corrompió datos
//
// Compilar con musl, enlazado dinámico o estático, da igual.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE_SZ 4096

int main(int argc, char **argv) {
  long mb = (argc > 1) ? atol(argv[1]) : 400;
  if (mb <= 0 || mb > 8192) {
    fprintf(stderr, "memhog: MB fuera de rango (1..8192)\n");
    return 2;
  }

  size_t total = (size_t)mb * 1024 * 1024;
  size_t npages = total / PAGE_SZ;

  printf("memhog: reservando %ld MB (%zu paginas) anonimos\n", mb, npages);
  fflush(stdout);

  unsigned char *p = mmap(NULL, total, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    fprintf(stderr, "memhog: mmap(%zu) fallo: %s\n", total, strerror(errno));
    return 1;
  }

  // -------------------------------------------------------------------------
  // Escritura: un valor distinto por página.
  //
  // El byte bajo del índice de página (i / PAGE_SZ) & 0xFF es único para
  // 256 páginas consecutivas. Con 256 páginas = 1 MB. Como el buffer es
  // mucho más grande, el patrón se repite cada 1 MB, pero cada página
  // contiene un byte identificable. Si swap devuelve una página
  // equivocada, la verificación lo detecta (excepto si el slot es
  // múltiplo de 256 páginas — aceptable para test).
  // -------------------------------------------------------------------------
  fprintf(stderr, "memhog: tocando paginas");
  for (size_t i = 0; i < npages; i++) {
    unsigned char v = (unsigned char)(i & 0xFF);
    p[i * PAGE_SZ] = v;
    // Llenar también los otros bytes de la página con un patrón
    // distinto para detectar corrupción parcial.
    p[i * PAGE_SZ + 1] = (unsigned char)((i >> 8) & 0xFF);
    p[i * PAGE_SZ + 2] = (unsigned char)((i >> 16) & 0xFF);
    p[i * PAGE_SZ + 3] = 0xAA;

    if ((i & 0xFFFF) == 0) {
      // Progreso cada 256 MB
      fprintf(stderr, ".");
      fflush(stderr);
    }
  }
  fprintf(stderr, " hechas\n");
  fflush(stderr);

  fprintf(stderr,
          "memhog: %ld MB residentes, durmiendo 30 s "
          "para dar tiempo a kswapd\n",
          mb);
  fflush(stderr);

  // Dormir. Durante este tiempo, kswapd (que corre cada 5 s) debería
  // detectar la presión de memoria y empezar a swapear páginas de
  // este proceso. Cualquier página swapeada se traerá de vuelta en
  // la fase de verificación vía #PF → swap_read_page.
  sleep(30);

  // -------------------------------------------------------------------------
  // Verificación. Lee cada página. Las que estén en swap disparan #PF
  // y el kernel las trae de vuelta. Si todo funciona, los valores son
  // los mismos que escribimos.
  // -------------------------------------------------------------------------
  fprintf(stderr, "memhog: verificando");
  fflush(stderr);

  size_t errors = 0;
  size_t first_bad = (size_t)-1;

  for (size_t i = 0; i < npages; i++) {
    unsigned char v0 = (unsigned char)(i & 0xFF);
    unsigned char v1 = (unsigned char)((i >> 8) & 0xFF);
    unsigned char v2 = (unsigned char)((i >> 16) & 0xFF);
    unsigned char *q = p + i * PAGE_SZ;

    if (q[0] != v0 || q[1] != v1 || q[2] != v2 || q[3] != 0xAA) {
      if (first_bad == (size_t)-1)
        first_bad = i;
      errors++;
    }

    if ((i & 0xFFFF) == 0) {
      fprintf(stderr, ".");
      fflush(stderr);
    }
  }
  fprintf(stderr, " hecha\n");

  if (errors > 0) {
    fprintf(stderr,
            "memhog: %zu errores de %zu paginas "
            "(primera mala: pagina %zu, offset %zu)\n",
            errors, npages, first_bad, first_bad * PAGE_SZ);
  }

  printf("memhog: verificacion completa, %zu errores\n", errors);
  fflush(stdout);

  munmap(p, total);
  return errors ? 1 : 0;
}