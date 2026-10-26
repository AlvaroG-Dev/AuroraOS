# LinuxKPI: sesión del 26-oct-2026 (fases L0 y L1)

Registro de lo hecho en la primera sesión de la capa de compatibilidad con drivers de Linux. El plan es `Plan_LinuxKPI.md`; las decisiones con fecha están en su §11 y los datos medidos en su §3.1. Este documento es el resumen para retomar el trabajo.

Estado: **L0 y L1 terminadas**. L2 (sincronización, tiempo, trabajo diferido) se hizo después: ver `docs/LinuxKPI_Sesion_L2.md`.

---

## 1. Decisiones tomadas (con el autor)

| Tema | Decisión | Dónde |
|---|---|---|
| Versión de Linux | **6.18 LTS** (provisional: 6.18 base; el punto `6.18.y` se fija cuando haya acceso a kernel.org) | `kernel/linuxkpi/linux-version.mk` |
| DRM | **Opción A**: se mantiene `kernel/drm/` propio y la capa aporta un *shim* `drm_*` (contra la recomendación B del plan) | `Plan_LinuxKPI.md` §11, D5 |
| Licencia de la capa | **GPL-2.0-only**, en `kernel/linuxkpi/` + `include/linuxkpi/`, con su propio `.kur` | cabecera SPDX en cada fichero |
| Rama | `ccr-85702992-sw957t`, fusionada después en `claude/gracious-lamport-7eumou` | — |
| NVIDIA | **Abierta**: `kernel-open` sobre LinuxKPI o capa de SO propia (ver datos en §4) | `Plan_NVIDIA.md` |

Consecuencia de la opción A para **D1/D2**: multi-CRTC, hot-plug y cambio de modo no vendrán del DRM de Linux; hay que generalizar `kernel/drm/drm_core.c` cuando haya un driver real.

## 2. Qué se ha hecho, por fase

### L0.1 — Versión y licencias
Decisiones anotadas en `Plan_LinuxKPI.md` §11 y en D5 de `docs/Wayland_Estado_y_Decisiones.md`.

### L0.2 — `scripts/fetch-linux-drivers.sh`
- Descarga (sin versionar, a `build/linuxkpi_src/`, ignorado por git) el árbol de Linux fijado, `open-gpu-kernel-modules` y, con `--firmware`, `linux-firmware`.
- `--ubuntu`: si kernel.org/GitHub no son accesibles, toma del archivo de Ubuntu `linux-source-6.8.0` y `nvidia-kernel-source-615-open` en directorios aparte (`ubuntu-linux/`, `ubuntu-nvidia-open/`). **No son las versiones fijadas**; sirven solo para desarrollar herramientas.

### L0.3 — `scripts/linuxkpi-scan.py` y `docs/linuxkpi_api.md`
- Análisis léxico: indexa lo que definen las cabeceras de Linux (macros, funciones, inlines, tipos, constantes, variables), tokeniza cada driver, quita lo que define el propio driver y clasifica cada símbolo por nivel K0–K9 según la cabecera.
- Salida: `docs/linuxkpi_api.md` (resumen, estimaciones y los 40 símbolos más usados por nivel) y `docs/linuxkpi_api.tsv` (todo).
- **Provisional**: medido sobre Linux 6.8 y NVIDIA 615.71.09 de Ubuntu. Hay que repetirlo con la 6.18:
  `scripts/linuxkpi-scan.py --linux build/linuxkpi_src/linux --nvidia build/linuxkpi_src/nvidia-open`

### L0.4 — Cargador `.kur` y `make linuxkpi-drv`
Cambios en `kernel/kur_loader.c`, `kernel/kur.h`, `kernel/kur_loader.h`:

| Antes | Ahora |
|---|---|
| Máximo 64 secciones en todo el ELF | Sin tope (tabla `sec_off[]` por índice; índices extendidos de ELF) |
| Solo `PROGBITS`/`NOBITS`; relocaciones hacia secciones no cargadas se ignoraban en silencio | Toda sección `SHF_ALLOC`; `.preinit_array`/`.init_array` antes de `kur_init`, `.fini_array` tras `kur_exit`; un símbolo de una sección no cargada es error |
| 5 tipos de relocación | + `NONE`, `PC64`, `GOTPCREL`, `GOTPCRELX`, `REX_GOTPCRELX` (GOT propia al final de la imagen) |
| — | `__start_SEC`/`__stop_SEC`, `_GLOBAL_OFFSET_TABLE_`, símbolos débiles sin definir → 0 |
| 256 exports en total, nombres de ≤ 63 caracteres | Tabla hash sin límite; nombres de cualquier longitud; un export no puede tapar un símbolo del kernel |
| `KUR_MAX_SIZE` 16 MiB | 64 MiB (tope de `kmalloc` al leer el fichero) |
| — | `kur_loader_lookup_symbol()` (para pruebas) |

Compilación (`kernel/linuxkpi/flags.mk`, `linuxkpi.mk`, `drv.mk`):
- `make linuxkpi` (lo llama `make drivers`, y por tanto `make iso`): compila `build/linuxkpi/linuxkpi.kur` y las pruebas, y las instala en `/lib/modules/linuxkpi/` (no se cargan solas al arrancar).
- `make linuxkpi-drv DRV=<nombre>`: compila el driver descrito en `kernel/linuxkpi/drivers/<nombre>.mk` (o `tests/linuxkpi/<nombre>/drv.mk`). Variables del descriptor: `LKPI_DRV_SRCDIR`, `LKPI_DRV_SRCS`, `LKPI_DRV_CFLAGS`, `LKPI_DRV_INSTALL`. Se añade el manifiesto `.kur`, se enlaza con `ld -r` y se sella con `kurbuild -H`.
- Flags como un módulo de Linux x86-64: `-nostdinc`, sin SSE/x87, `-mcmodel=kernel`, `-ffunction-sections`, `-include linux/kconfig.h -include linux/compiler_types.h`, `-D__KERNEL__ -DMODULE`.

### L0.5 — Marco de pruebas
- `tests/linuxkpi/lkpi_test.h`: `KT_ASSERT`, `KT_EQ`, `KT_STREQ` y un PRNG. Las aserciones llaman a `test_pass`/`test_fail` del kernel (exportados), así que cuentan en la suite normal.
- `kernel/tests/kernel_tests.c`: 6 pruebas `linuxkpi: …`:
  1. cargador `.kur` para módulos grandes (L0.4);
  2. carga de la capa y sus exports;
  3. K0; 4. K1; 5. K3 (cada una carga su `.kur` de prueba y lo descarga);
  6. descarga de la capa.

### L1 — K0, K3 y memoria de K1 sin DMA
**Cabeceras** (`include/linuxkpi/`, propias, sin copiar de Linux): tipos, compilador, `kconfig` (`lkpi/config.h` con los `CONFIG_*` soportados), `errno`, `printk`, `bug`, bits, `bitmap`, `bitfield`, `atomic`, barreras, `byteorder`, `unaligned`, `math`, `overflow`, `minmax`, `kstrtox`, `sprintf`, `string`, `ctype`, `jiffies`/`ktime`, `export`, `module`, `moduleparam`; `list`/`hlist`/`llist`, `rbtree` (aumentado y cacheado), `interval_tree`, `xarray`, `idr`/`ida`, `hash`/`hashtable`, `jhash`, `crc32`, `sort`, `bsearch`; `slab`, `gfp`, `mm`/`mm_types`, `highmem`, `vmalloc`, `io`, `uaccess`; y un spinlock mínimo.

**Implementación** (`kernel/linuxkpi/`):
- `core.c`: manifiesto, `kur_init`/`kur_exit`, `printk` (niveles de Linux → `klog`), `WARN`/`BUG`/`panic`, `dump_stack`, `jiffies`/`ktime`.
- `lib/`: `vsprintf` (con `%pe`, `%pI4`, `%pM`, `%ph`, `%pa`, `%pV`, `%p4cc`…), `sscanf`, cadenas, `kstrto*`, `bitmap`/`find_bit`, `rbtree`, `interval_tree`, `xarray`, `idr`/`ida`, `sort`/`list_sort`, `crc32`, nombres de error.
- `mm/`: `struct page` y registro pfn, `alloc_pages`, `kmalloc`/`kmem_cache`, `vmalloc`, `ioremap`/`memremap`, `copy_{to,from}_user`.
- `aurora.h`: único puente con el kernel de Aurora (ver §3).

Tamaño: `linuxkpi.kur` ≈ 140 KiB, 457 secciones, 167 exports.

## 3. Decisiones de diseño (técnicas)

- **Prefijo `lkpi_`** en todo lo que exporta la capa; las cabeceras ofrecen el nombre de Linux como `static inline` o macro. Evita que un driver enlace con un homónimo de Aurora con otra semántica (`kmalloc`, `device_register`, `request_firmware`…). Excepción: `memcpy/memset/memmove/memcmp`, que vienen del kernel porque el compilador puede emitirlos.
- **Puente con Aurora** (`kernel/linuxkpi/aurora.h`): la capa no incluye cabeceras de Aurora. Cada servicio se declara como `aurora_*` con la etiqueta asm del símbolo real; `kernel/ksymtab_exports.c` los exporta. **Si cambia una firma de Aurora, hay que cambiarla también allí.**
- **`kmalloc` propio**: memoria físicamente contigua, alineación natural en potencias de dos y `virt_to_page` válido, como en Linux. El `kmalloc` de Aurora no lo garantiza (cabecera de 64 bytes por página en su slab y heap solo virtualmente contiguo).
- **`struct page` propio**: `alloc_pages()` crea un array contiguo (`page + i` vale dentro de la asignación) y un registro hash pfn → página. Para marcos ajenos, descriptor suelto. No hay `mem_map` global.
- **`vmalloc`** sobre el heap de Aurora (≤ 64 MiB por bloque).
- **`ioremap`**: la ventana MMIO de Aurora asigna la virtual por la física; una página ya mapeada por Aurora (LAPIC, framebuffer…) se reutiliza y nunca se desmapea; las de la capa se desmapean cuando ningún `ioremap` vivo las cubre.
- **`xarray`** propio (radix de 64 ranuras con bitmaps de presencia, lleno y marcas). Lecturas sin bloqueo como en Linux; liberar nodos con lectores concurrentes necesita RCU (L2).
- **Tiempo**: `HZ = 1000` (tick de Aurora); `ktime_get()` con resolución de 1 ms por ahora.

## 4. Datos medidos (provisionales, Linux 6.8)

| Driver | Símbolos de la API de Linux | Acumulado en el orden de trabajo |
|---|---:|---:|
| `bochs` | 136 | 135 |
| `simpledrm` | 192 | 277 |
| `virtio-gpu` | 549 | 634 |
| `i915` | 3600 | 3877 |
| `xe` | 1139 | 4198 |
| `amdgpu` | 2709 | 5031 |
| `nvidia` (`kernel-open`, sin `uvm`) | 1679 | 5717 |

- DRM (`include/drm/*`) es el bloque mayor: **2292 símbolos** (1669 de ellos los usa `i915`). Con la opción A es el *shim* propio: **≥ 15–30 kLOC**.
- Resto de la capa: **≈ 22–45 kLOC** (detalle por nivel en `Plan_LinuxKPI.md` §3.1).
- NVIDIA: de 1672 símbolos, **953 (57 %) los necesitan también Intel/AMD**. La alternativa (capa de SO propia) supone reescribir el equivalente a las ~90 k líneas de `kernel-open/`.

## 5. Pruebas

| Prueba | Al empezar | Al terminar |
|---|---|---|
| Suite del kernel | 182/183 (`firmware: request_firmware` fallaba) | **189/189** (183 + 6 de LinuxKPI) |
| `drm_test`, `sig_test`, `rseq_test` | ALL PASS | ALL PASS |
| `clipboard-x11.sh`, `primary-icons.sh` | pasan | pasan |

- El fallo inicial venía de `.gitignore` (`*.bin` dejaba fuera `precompiled/lib/firmware/aurora/selftest.bin`); corregido en commit aparte.
- En una de las dos regresiones completas, el `bash` de `clipboard-x11.sh` se quedó sin terminar después de `head` (las comprobaciones de portapapeles ya habían pasado). Repetida con el mismo código, pasó todo. La capa se descarga antes de que arranque el userland, así que no parece relacionado, pero no se aisló la causa.
- Cómo reproducir: `make iso MUSL_CC=/usr/bin/musl-gcc KUR_CC=gcc` y arrancar con `scripts/qemu-drive.py -m 6G`; la suite del kernel imprime `[TEST] N tests: …` y cada prueba `linuxkpi: …`.

## 6. Qué no se ha podido comprobar

- Nada contra **Linux 6.18** ni contra el código de NVIDIA de GitHub: el entorno bloquea `kernel.org`, `cdn.kernel.org`, `git.kernel.org`, `github.com` y `codeload.github.com`.
- `copy_to_user`/`copy_from_user` con un proceso de usuario real.
- Concurrencia en varias CPU (estrés SMP): es L2.
- Ningún driver de GPU real (ni se ha intentado, como se pidió).

## 7. Contradicciones encontradas en los planes

- **Límites del cargador**: el plan suponía que bastaba revisarlo; en la práctica rechazaba cualquier driver de Linux (64 secciones en total, 256 exports, 16 MiB). Arreglado en L0.4.
- **Rama**: `docs/Wayland_Estado_y_Decisiones.md` citaba `claude/gracious-lamport-7eumou`; la sesión trabajó en `ccr-85702992-sw957t` (fusionada después en la primera).
- **NVIDIA**: D10 y `Plan_NVIDIA.md` §3 dan como enfoque la capa de SO propia; `Plan_LinuxKPI.md` L8, `kernel-open` sobre LinuxKPI. Sigue abierto (§1).
- **Numeración**: `Plan_Wayland_Unificado.md` 8.3 llama a IOMMU «7.2» y a MSI-X «1.4»; `drv_impl_plan.md` usa «8.0» para IOMMU.
- **Comentario desfasado** en `kernel/paging.h`: dice que la ventana física de RAM es `PTE_NOCACHE`; `paging.c` la mapea WB.

## 8. Pendiente y decisiones abiertas

1. **Acceso a red** a kernel.org y GitHub (configuración de *Network access* del entorno). Después: fijar `6.18.y` y la etiqueta de `linux-firmware`, y repetir L0.3 sobre la 6.18.
2. **NVIDIA**: `kernel-open` sobre LinuxKPI o capa propia (los datos apuntan a lo primero; decide el autor).
3. **Opción A de DRM**: si se quiere revisar, antes de L5.
4. **RCU** (L2) antes de usar `xarray`/`idr` con lectores concurrentes.
5. **Módulos > 64 MiB** (NVIDIA): leer el `.kur` por trozos (L8).
6. **L2**: `mutex`, `rwsem`, `ww_mutex`, `completion`, `wait_queue`, RCU, `workqueue`/`delayed_work`, `timer_list`, `hrtimer`, con estrés en 4 CPU. **Pendiente de confirmación.**

## 9. Commits de la sesión

| Commit | Contenido |
|---|---|
| `0234bfb8` | Versionar `selftest.bin` (arregla la prueba de firmware) |
| `41de9513` | L0.2: versiones fijadas y `fetch-linux-drivers.sh` |
| `b62db415` | L0.1: decisiones |
| `bafdbbef` | L0.3: escáner y `docs/linuxkpi_api.md` |
| `745762a7` | L0.4: cargador `.kur` |
| `7a122537` | L1: cabeceras e implementación |
| `c7a1b5cc` | L0.4/L0.5: `make linuxkpi-drv` y pruebas |
| `0f4dbcaa` | Decisiones de diseño en el plan |
