# LinuxKPI — sesión L6 (K9: ttm, planificador de GPU y memoria de dispositivo)

Fecha: 28-oct-2026. Rama `claude/project-thread-v3dezm` (desde `claude/beautiful-newton-1neyw4`, que trae L0–L5). Sin PR ni merge a main.

## 1. Qué se hizo

- **`lkpi_ttm.kur`** (`kernel/linuxkpi/drivers/lkpi_ttm.mk`): compila desde Linux 6.18.59 sin parches: `drm_buddy`/`gpu_buddy`, `ttm/*` (device, bo, bo_util, bo_vm, resource, range_manager, tt, pool, backup, sys_manager) y `scheduler/*` (`drm_gpu_scheduler`, entidades, `s_fence`). Pegamento propio en `drivers/lkpi_ttm/glue.c`.
- **Capa**: `shrinker` real (lista global, `lkpi_shrink_slab` invocado desde `lkpi_alloc_pages` cuando el PMM falla), `shmem`/«swap» en RAM (`get_nr_swap_pages` = mitad de las páginas libres; `shmem_writeout` devuelve `-EBUSY`), `split_page` con liberación por página, `kmap_local_page_prot` con WC vía `lkpi_vmap`, `io_mapping` sobre `ioremap_wc`, `si_meminfo`, cabeceras nuevas (`shrinker`, `swap`, `page-flags`, `pgtable`, `mmzone`, `fault-inject`, `io-mapping`, `kmemleak`, `gpu_buddy`).
- **`mmu_notifier`** funcional pero sin invalidaciones reales; **`hmm`** desactivado (`hmm_range_fault` → `-EOPNOTSUPP`).
- **`dma_resv`** en uso real (fences de BO y de movimiento).

## 2. Decisiones (ver Plan_LinuxKPI.md §11, 28-oct-2026)

VRAM simulada = RAM gestionada por `gpu_buddy` (4 MiB, el máximo de `alloc_pages` es orden 10); GTT = `ttm_range_man`; swap de ttm = shmem en RAM sin disco; shrinker sin LRU ni cgroups; `ttm_pool` sin dma32/NUMA/THP; el alias WB no se retira de la ventana directa (L7/L9).

## 3. Hallazgos

- Con evicción *pipelined* (`ttm_bo_move_pipeline_evict`) el recurso origen se libera al instante y solo queda el fence `man->move`; un job no puede leer ese recurso después (UAF encontrado y corregido capturando los campos al crear el job).
- Tras `drm_sched_stop`, el fence `finished` del culpable acaba con `-ECANCELED`.
- `ttm_bo_vm` funciona sobre el mmap de la capa, incluida la evicción con un mapeo de usuario vivo.

## 4. Pruebas

- Kernel: `lkpi_test_k9` (shrinker, split_page, swap, mmu_notifier), `lkpi_test_ttm` (crear/mapear/evictar, sync/async/null move, gpu-sched con job simulado y fences reales, timeout/hang) y `lkpi_test_ttm_smp` (estrés 4 hilos × 400 rondas).
- Espacio de usuario: `/dev/lkpi_ttm` (`lkpi_test_ttm_dev`) y `lkpi_test` (141 comprobaciones): BO de 256 KiB mapeado, VRAM→GTT→VRAM, presión con el mapeo vivo, descarga.

## 5. Commits

`743f9bba` (módulo y capa), `81dfbd66` (pruebas), `e8d42905` (`/dev/lkpi_ttm` y userland), más el de documentación de esta sesión.

## 6. Regresión (28-oct-2026, QEMU `-m 6G`, uno a la vez)

Todo OK: suite del kernel 207/207 con `-smp 2` y `-smp 4`; `lkpi_test` 141 con `-smp 4`; `drm_test` nativo, bochs, simpledrm y virtio-gpu (2 salidas); sig_test, rseq_test, timer_test, thread_sig_test; clipboard-x11.sh y primary-icons.sh (valores revisados a mano). En una pasada previa salió un fallo de tiempo de K2 (`20 x msleep(1)` en 155 jiffies); no se ha reproducido.

## 7. Pendiente

- De L5: kmscube (Mesa), plano de cursor de virtio-gpu, `IMPORT_SYNC_FILE` de syncobj, hot-plug y compositor multi-monitor (D1/D2).
- De L6: alias WB en la ventana directa (L7/L9); VRAM limitada a 4 MiB; `ttm_pool` sin dma32/NUMA/THP; shrinker sin LRU/cgroups; HMM y mmu_notifier no funcionales.
- D1/D2/D5/D6/D10 no cambian; `docs/Wayland_Estado_y_Decisiones.md` no se toca.
