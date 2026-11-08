# fork con copy-on-write

Estado: implementado en la rama `claude/project-thread-cxw685` (sin fusionar).
Requisito del plan GCC (`docs/Plan_GCC_Autocompilacion.md`, bloqueo "fork sin COW").

## Antes

`sys_fork` (kernel/process.c, `clone_user_space`) recorría las tablas del padre y
copiaba cada página presente a un frame nuevo. Fork de 192 MB en QEMU/TCG: ~5,3 s
y 192 MB de RAM consumidos al instante. `vfork` y `clone` sin `CLONE_THREAD` caían
en el mismo camino.

## Ahora

* **Refcount por frame en el PMM** (`kernel/pmm.c`): un `uint16_t` por página con
  las referencias *extra* (0 = un solo dueño; 2 B/página, p. ej. 8 MB para 16 GB).
  `pmm_page_share(phys)` suma un dueño; `pmm_free_page` suelta una referencia y solo
  libera el frame con la última. Por eso munmap, exit, brk, madvise, execve, etc.
  funcionan sin tocarlos. Operaciones atómicas, sin el lock del PMM en el camino
  caliente. Si no hay tabla o el contador se satura, fork copia esa página como antes.
* **`PTE_COW` (0x800)**: bit propio del PTE. fork deja las páginas privadas de RAM
  (anónimas, brk, pila, ELF, MAP_PRIVATE) compartidas, **sin `PTE_WRITABLE`** y con
  `PTE_COW` en padre e hijo. Un único TLB shootdown global al final del recorrido
  (`paging_invalidate_tlb_global(0)`), no uno por página.
* **Fallo de escritura** (`pf_handle_cow` en kernel/pf.c): bajo `mm_lock`, si el
  frame sigue compartido copia a uno nuevo; si el otro dueño ya salió, solo vuelve a
  hacer el PTE escribible. Si el proceso tiene varios hilos, invalida la página en
  todas las CPUs antes de soltar el frame viejo. Un PTE ya escribible (TLB
  desfasado de otra CPU) se trata como fallo espurio.
* **El kernel también cae en COW**: `CR0.WP=1` (BSP tras `uaccess_init`, APs en
  `smp_boot.c`). Un `copy_to_user`/`read()` sobre una página compartida provoca
  #PF de supervisor y se resuelve igual antes de buscar el fixup de uaccess.
* **mprotect** conserva `PTE_COW` y no hace escribible una página aún compartida.
* **swap**: kswapd solo elige páginas `PTE_WRITABLE` (las COW no lo son) y aborta si
  el frame se comparte durante la E/S. Las páginas ya en swap se siguen copiando
  (leyéndolas) al hacer fork.
* Páginas `PTE_SPECIAL` (vDSO, buffers DMA de usuario) pasan a compartirse tal cual
  (antes se copiaban, y la copia se filtraba al salir).
* `vfork` y `clone` sin `CLONE_THREAD` siguen siendo fork, ahora barato; no se
  implementa el bloqueo del padre de vfork.

## Medidas (QEMU TCG, -smp 4, 1 GB)

| | antes | ahora |
|---|---|---|
| fork con 192 MB tocados | 5262 ms, 197 MB consumidos | ~130-190 ms, 428 KB |
| 1500 fork+exit | fuga de ~14 MB en el kernel | 0 (los ~1,9 MB vistos antes eran la cache del overlay, ver abajo) |
| 300 fork/vfork+execve | fuga 2,4 MB | 0 |

Tras cada fase la suma de referencias extra vuelve a 0 (comprobado con un contador
temporal), es decir, no hay fugas de refcount.

## Pruebas

`tests/cow/run.sh [segundos]` arranca en QEMU (`-smp 4`) un initrd mínimo cuyo
`/usr/bin/init` es `tests/cow/cow_test.c` y espera `COW-TEST: PASS todo`. Cubre:
aislamiento padre/hijo (mmap, .data, pila), escritura del kernel en página COW
(`read()` a un buffer compartido, en hijo y en padre), mprotect R→RW sobre páginas
COW, fork de 192 MB (tiempo y memoria), 1500 fork+exit, 300 fork/vfork+execve, y
8 hijos × 3 nietos escribiendo en paralelo. Prueba del kernel: `pmm: refcount por
frame (COW)` en `kernel/tests/kernel_tests.c`.

Requiere `kernel/kernel.elf` (`make -C kernel`) y `bootloader/efi/BOOTX64.EFI`
(`make bootloader-efi`). Con el initrd mínimo fallan las pruebas del kernel que
necesitan ficheros que no están (linuxkpi .kur, firmware, bibliotecas en tarfs):
es esperado.

## Falso positivo de "fuga" de ~1,9 MB (2026-11-02)

`tests/cow/run.sh` fallaba de forma intermitente con ~1904 KB "perdidos". No es
una fuga: es una única `kmalloc` de 1.943.552 B (475 páginas de heap) en
`overlay_rebuild_cache` (kernel/boot_console.c), la capa cacheada del overlay de
log del kernel. Se crea en el primer fotograma del compositor con el overlay
activo (sin disco el arranque lo activa en `main.c`), y ese instante depende de la
carga de TCG, así que cae en cualquier fase del test (forks, exec o SMP) y solo
en una por pasada. Con la prueba antigua el umbral de 512 KB (exec) fallaba y el de
3 MB (forks) lo escondía. Medido con 18 pasadas: sin esa reserva los forks dan 0 KB
y los exec 0-4 KB. La prueba ahora juzga cada bucle por mitades y toma la menor
(`no_leak`): una fuga real crece en las dos mitades, la reserva única en una. El
test SMP repite la ronda dos veces (la primera deja 130-200 KB de caches calientes;
la segunda 0).

## Límites conocidos

* El shootdown es a todas las CPUs (no hay máscara de CPUs por proceso).
* `munmap`/`mprotect`/`mmap` no toman `mm_lock` (solo fork, brk, swap-in y COW);
  una carrera de esas con un fallo COW en otro hilo existía ya con el demand
  paging.
* Las páginas en swap se leen y copian al hacer fork (no se comparten slots).
* MAP_SHARED de ficheros/anónimo que no sea memfd sigue sin compartirse entre
  padre e hijo (igual que antes).
