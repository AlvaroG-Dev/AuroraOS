# Plan de implementación: capa de compatibilidad con drivers de Linux (LinuxKPI) para Aurora

Estado: **L0, L1, L2, L3, L4, L5 y L6 hechas** (L6 el 28-oct-2026: K9 con `ttm`, `drm_buddy`, planificador de GPU, shrinker y memoria de dispositivo simulada; ver `docs/LinuxKPI_Sesion_L6.md`; **L7: `i915` compila, enlaza y carga en QEMU; hito 1 cumplido en la Portégé el 28-oct-2026 (dibuja con i915), `xe` diferido, ver `docs/LinuxKPI_Sesion_L7.md`**) (L5 el 27/28-oct-2026: K7 y DRM/KMS de Linux con `bochs`, `simpledrm` y `virtio-gpu` en QEMU, uevents, `DRM_BACKEND`; ver `docs/LinuxKPI_Sesion_L5.md`) (L4 el 27-oct-2026: DMA con bounce, `sg_table`, `dma_buf` ↔ PRIME, firmware real, I²C con `i2c-algo-bit`, debugfs, backlight y E/S de usuario con `task_struct`/señales reales; hito `/dev/lkpi_test` desde el espacio de usuario, ver `docs/LinuxKPI_Sesion_L4.md`). L0–L3: 26-oct-2026 (L0.3 repetida sobre Linux 6.18.59 y NVIDIA open 615.78.08, las versiones fijadas, ver §3.1 y §11). Tras L3 (27-oct-2026): `aur.skip=`, polaridad INTx, medida de `virtio-gpu` y correcciones de Aurora (cuelgue de `process_exit`, temporizadores, señales por hilo, salida del grupo, BIOS/LFN, `ata_dma`, montaje tardío), ver `docs/LinuxKPI_Sesion_L3.md` §9–§11. Resúmenes de las sesiones: `docs/LinuxKPI_Sesion_L0_L1.md`, `docs/LinuxKPI_Sesion_L2.md`, `docs/LinuxKPI_Sesion_L3.md` `docs/LinuxKPI_Sesion_L4.md` y `docs/LinuxKPI_Sesion_L5.md`. Decisión D5 (opción a): una sola capa para **Intel (`i915`/`xe`), AMD (`amdgpu`) y NVIDIA (`kernel-open/`)**. Contexto: `docs/Wayland_Estado_y_Decisiones.md` (D5, D6, D10), `Plan_NVIDIA.md`, Bloque 8 de `Plan_Wayland_Unificado.md`.

## 0. Principios

1. **Compatibilidad a nivel de código fuente, no binaria.** Los drivers de Linux se **compilan** contra cabeceras de Aurora que imitan `include/linux/*`, `include/drm/*`, `include/uapi/*` y se enlazan como `.kur`. Es la técnica de FreeBSD (`linuxkpi` + `drm-kmod`) y la de Haiku/illumos en sus puertos DRM.
2. **El driver de Linux se toca lo mínimo** (parches pequeños y mantenibles sobre una versión fija). Todo lo que falta se resuelve en la capa, no editando `i915`/`amdgpu`.
3. **Se prueba con drivers de Linux que funcionan en QEMU antes de tocar GPU reales** (sección 6). Sin eso, cada fallo en hardware real sería indiagnosticable.
4. **GOP como respaldo** en todos los hitos con GPU real, y el arranque nunca depende de que la capa funcione.
5. **Una versión de Linux fijada** (propuesta: la serie LTS más reciente que traiga soporte de Ada/Alder Lake/Haswell, a confirmar en L0) y migraciones deliberadas, nunca «seguir `master`».

### Alcance: qué ahorra la capa y qué sigue siendo trabajo (26-oct-2026)

**Objetivo: todas las GPU** (Intel `i915`/`xe`, AMD `amdgpu`, NVIDIA `kernel-open/`), no solo el hardware del autor. El portátil Haswell (HD 4600) y la RTX 4060 son únicamente los **primeros bancos de prueba reales**; el plan (L7, L8) no se limita a ellos.

**Copiar el driver no basta.** El código del driver de Linux se **recompila contra las cabeceras de Aurora** (`include/linuxkpi/`) y se enlaza como `.kur`; no es compatibilidad binaria y el `.ko` de Linux no se carga tal cual (principio 1). La capa ahorra escribir cada driver (millones de líneas) y permite heredar sus correcciones, pero **no elimina**:

1. **La propia capa** (~40–45 kLOC) y su ampliación: cada driver nuevo puede pedir símbolos que aún no existen (se mide con `linuxkpi-scan.py`, §6.6).
2. **La semántica**, no solo los nombres: bloqueos, RCU, orden de memoria, DMA y tiempos de Linux. Si Aurora difiere, no falla al compilar sino en ejecución (cuelgues, corrupción), y solo se depura con hardware y log serie.
3. **Piezas de Aurora que aún no existen** y que la GPU real exige: DMA/IOMMU real (L9), energía, TTM y scheduler de GPU (L6), memoria contigua con límites de dirección y MSI-X por CPU.
4. **Firmware** de AMD y NVIDIA (con sus licencias; no se versiona).
5. **La mitad de usuario**: Mesa (o las bibliotecas propietarias de NVIDIA, misma versión que el firmware) y los ioctls que expone cada driver; ver Bloque 8 de `Plan_Wayland_Unificado.md`. Sin ella no hay 3D acelerado aunque el driver de kernel funcione.
6. **Validación por GPU y por generación** en hardware real: cada una tiene errata y rutas propias. El código es el mismo, pero hay que verlo arrancar.

Con la capa madura, cada driver nuevo debería costar bastante menos que el anterior, pero **no cero**.

### Licencias
- El código de Linux es **GPL-2.0** (con partes MIT en DRM). Todo `.kur` derivado de Linux queda bajo GPL (aceptado por el autor, D5).
- Las **cabeceras y la implementación de la capa**: escribirlas propias (no copiar cabeceras de Linux) y, si se toma código de **FreeBSD `linuxkpi`** (BSD-2), conservar su aviso. Elegir licencia de la capa en L0 (propuesta: GPL-2.0 compatible, para poder alojar los drivers sin fricción), y mantenerla **separada** del resto del kernel de Aurora (directorio, `.kur` propio).
- Firmware (`linux-firmware`, GSP de NVIDIA): redistribuible con condiciones; **no se versiona** (scripts de descarga, como `fetch-mesa.sh`).

## 1. Qué existe ya en Aurora

| Pieza | Estado | Dónde |
|---|---|---|
| Cargador `.kur` (ET_REL, imports/exports, SHA-256) | sí | `kur_loader.c`, `ksymtab.c` |
| Syscalls `init_module`/`delete_module`, `/proc/modules`, `/sys/module` | sí | `kmod_sys.c` |
| PCI, BARs 64 bits, MSI, hot-plug | sí | `pci.c`, `msi_user.c`, `aur_bus.c` |
| `request_firmware` | sí | `firmware.h` |
| Workqueues, kthreads, IRQ con hilo, wait queues, completions | sí | `workqueue.c`, `kthread.c`, `irq_threaded.c`, `wait.c`, `completion.c` |
| DRM/KMS propio (1 CRTC virtual), GEM dumb, PRIME, syncobj, sync_file | sí | `kernel/drm/` |
| ABI Linux hacia el userland (ioctl, mmap, epoll, memfd) | sí | syscalls |
| Buddy, kmalloc, paginación con PAT (WB/WC/UC) | sí | `buddy.c`, `paging.c` |

## 2. Decisión de arquitectura clave: ¿qué DRM?

Dos opciones:

- **A) Mantener `kernel/drm/` propio** y escribir un *shim* con las funciones `drm_*` que usan los drivers (`drm_dev_alloc`, `drm_mode_config`, atomic helpers, GEM/TTM…). Rápido para empezar, pero **hay que reimplementar** una superficie enorme (atomic helpers, `drm_gem_*`, `ttm`, `drm_syncobj`, `drm_fb_helper`, modeset locks, `drm_connector`/`drm_edid`…) y divergirá de Linux.
- **B) Compilar el DRM *core* de Linux** (`drivers/gpu/drm/*.c`, `ttm`, `scheduler`, `display` helpers) sobre la LinuxKPI, como hacen FreeBSD `drm-kmod` y NVIDIA con su `nvidia-drm`. El código DRM es el mismo que usan los drivers, así que los ioctls (`DRM_IOCTL_*`), propiedades, sync y PRIME **salen idénticos a Linux** (lo que esperan Mesa y los compositores).

**Recomendación: B**, por fases:
1. Mientras tanto, `kernel/drm/` propio sigue sirviendo a la pantalla virtual y a las pruebas actuales.
2. Cuando el DRM de Linux sobre la capa pase la suite actual (`drm_test`, `modetest`, Weston, `aurora-wayland`), **se sustituye** el propio y se conserva su código como respaldo (`CONFIG` en el `Makefile`) un tiempo.
3. Los nodos `/dev/dri/card*`/`renderD*` se exponen con un puente `cdev` → VFS de Aurora.

Esto también cubre la deuda de **multi-CRTC y hot-plug (D1)** y el cambio de modo (D2), porque el DRM de Linux ya los resuelve.

## 3. Inventario de API por niveles (orden de dependencia)

Los números son orientativos (líneas de la capa, no del driver).

| Nivel | Contenido | Ejemplos de `linux/*` | Tamaño estimado |
|---|---|---|---|
| **K0 Base C** | tipos, `container_of`, `min/max`, `ARRAY_SIZE`, `BUILD_BUG_ON`, `WARN/BUG`, `printk`/`pr_*`/`dev_*`, `kstrto*`, `snprintf` con `%pI4` y `%pe`, atributos `__init/__exit/__user/__iomem`, `likely`, barreras, atomic, `READ_ONCE/WRITE_ONCE`, bitops, `bitmap`, `ktime`/`jiffies`/`ktime_get`, `ERR_PTR`, `errno` | `kernel.h`, `types.h`, `bitops.h`, `atomic.h`, `err.h` | ~4 kLOC |
| **K1 Memoria** | `kmalloc/kzalloc/kvmalloc/vmalloc`, `kfree`, `kmem_cache`, `devm_*`, `get_free_pages`, `alloc_pages` + `kmap`, `ioremap`/`iounmap` (+`_wc`, `_cache`), `memremap`, `copy_{to,from}_user`, `vm_area_struct`/`mmap`, `remap_pfn_range`, `shmem` (objetos pageables), `dma_alloc_coherent`, `dma_map_*`, `sg_table`/`scatterlist`, `dma_buf` | `slab.h`, `mm.h`, `io.h`, `dma-mapping.h`, `scatterlist.h`, `dma-buf.h` | ~6 kLOC |
| **K2 Sincronización y tiempo** | `spinlock`, `mutex` (+`ww_mutex`), `rwsem`, `semaphore`, `completion`, `wait_queue`, `seqlock`, `rcu` (aprox. con barrera de grace period), `srcu`, `percpu`, `atomic_*`, `timer_list`, `hrtimer`, `delayed_work`, `workqueue`, `tasklet`, `kthread`, `msleep/udelay`, `kref`, `refcount` | `spinlock.h`, `mutex.h`, `ww_mutex.h`, `rcupdate.h`, `workqueue.h` | ~5 kLOC |
| **K3 Estructuras** | `list`, `hlist`, `rbtree`, `interval_tree`, `idr`/`ida`, `xarray`, `radix_tree`, `llist`, `kfifo`, `sort`, `bsearch`, `string`, `hash`, `crc32`, `siphash` | `list.h`, `rbtree.h`, `idr.h`, `xarray.h` | ~4 kLOC |
| **K4 Modelo de dispositivos** | `struct device`, `kobject`, `device_driver`, `bus_type`, `class`, `attribute`/`sysfs`, `driver_register`, `dev_get_drvdata`, `module_*`, `MODULE_*`, `EXPORT_SYMBOL*`, `request_module` | `device.h`, `kobject.h`, `module.h`, `sysfs.h` | ~4 kLOC |
| **K5 PCI/IRQ/energía** | `pci_dev`, `pci_register_driver`, `pci_enable_device`, `pci_set_master`, `pci_iomap`, `pci_read/write_config_*`, BARs, ReBAR, `pci_alloc_irq_vectors` (MSI/MSI-X), `request_irq`/`request_threaded_irq`, `pm_runtime` (stubs), `suspend/resume` (stubs), `pcie_*` ASPM, FLR/`pci_reset_function`, `vga_switcheroo` (stub), `vga_client` | `pci.h`, `interrupt.h`, `pm.h` | ~5 kLOC |
| **K6 Firmware, I²C, GPIO, DMI, ACPI, backlight** | `request_firmware(_nowait)`, `i2c_adapter` + `i2c-algo-bit`, `gpio` (stub), `dmi_*`, `acpi_*` mínimo (`_DSM`, `_DOD`, `acpi_video`), `backlight_device`, `led` (stub), `thermal`/`hwmon` (stub), `debugfs` (en memoria), `component` | `firmware.h`, `i2c.h`, `acpi.h`, `backlight.h` | ~4 kLOC |
| **K7 Fences/sync** | `dma_fence`, `dma_fence_chain`, `dma_resv`, `sync_file`, `drm_syncobj` (el de Linux), `seqno_fence` | `dma-fence.h`, `dma-resv.h` | ~3 kLOC (con DRM-B casi todo viene del propio Linux) |
| **K8 Entorno de usuario/VFS** | `file_operations`, `cdev`, `inode`, `poll`/`epoll`, `ioctl` (`unlocked_ioctl`/`compat`), `fasync`, `mmap`, `vm_operations_struct`, `fdget/fdput`, `get_unused_fd_flags`, `anon_inode_getfile`, `task_struct` mínimo (`current`, `pid`, `comm`, `mm`), `signal_pending`, `capable()` | `fs.h`, `cdev.h`, `poll.h`, `sched.h` | ~5 kLOC |
| **K9 Misceláneo de DRM/GPU** | `drm/*` completo (si B), `ttm`, `gpu-sched`, `drm_gem_*`, `shmem_helper`, `drm_mm` (allocador), `drm_buddy`, `swiotlb` (stubs), `mmu_notifier` (stubs hasta HMM), `hmm`/`migrate` (solo para `amdgpu` SVM: desactivar), `devfreq` (stub), `perf` (stub), `trace/*` (no-op), `static_key`/`jump_label` (variable normal), `kasan/lockdep` (no-op) | `drm/*`, `trace/events` | grande, dependiente del driver |

Total orientativo de la capa (sin DRM de Linux): **~40–45 kLOC de C** propios, con ~25 % tomable o adaptable de FreeBSD `linuxkpi` (ya licenciado BSD).

### 3.1 Datos medidos (L0.3, 26-oct-2026)

**Sobre Linux 6.18.59** (versión fijada; repetida el mismo día en cuanto kernel.org fue accesible). NVIDIA open **615.78.08** (la fijada; desde GitHub con `git`, ya que el tarball sigue dando 403). Es lo que hay ahora en `docs/linuxkpi_api.md`/`.tsv`.

| Nivel | Símbolos (todos los drivers) | de ellos funciones | QEMU (`bochs`+`simpledrm`+`virtio-gpu`) |
|---|---:|---:|---:|
| K0 Base C | 453 | 35 | 50 |
| K1 Memoria (sin DMA) | 458 | 93 | 60 |
| K1-dma DMA, `dma_buf`, IOMMU | 104 | 35 | 35 |
| K2 Sincronización/tiempo | 452 | 96 | 32 |
| K3 Estructuras | 405 | 101 | 32 |
| K4 Dispositivos | 235 | 90 | 47 |
| K5 PCI/IRQ/energía | 425 | 153 | 53 |
| K6 Firmware/I²C/ACPI/… | 501 | 137 | 22 |
| K7 Fences | 103 | 44 | 36 |
| K8 Usuario/VFS | 137 | 52 | 3 |
| K9 *stubs* | 193 | 66 | 26 |
| **K9-drm** | **2325** | 646 | 295 |
| K9-ttm | 190 | 95 | 0 |
| K9-sched | 46 | 27 | 0 |
| Sin clasificar | 77 | 15 | 3 |

Unión acumulada en el orden de trabajo: `bochs` 180 → `simpledrm` 304 → `virtio-gpu` **694** → `i915` 3881 → `xe` 4559 → `amdgpu` 5450 → `nvidia` 6106 (con la 6.8: 135 → 277 → 634 → 3877 → 4198 → 5031 → 5717). El mayor cambio es `xe`, que pasa de 1139 a 1783 símbolos (el driver ha crecido mucho entre 6.8 y 6.18). Estimación heurística total: ≈ 40 kLOC (5 kLOC para el primer hito en QEMU), en la misma horquilla que la corregida abajo. NVIDIA (615.78.08): 1679 símbolos, **992 (59 %) compartidos con Intel/AMD** (con la 615.71.09 de Ubuntu: 1675 y 990); lo exclusivo es sobre todo K6 (130), K1 (108), K3 (70), K4 (67) y K5 (61).

**Corrección del 27-oct-2026 (tras L3): `virtio-gpu` con el núcleo virtio.** La medida de arriba solo miraba `drivers/gpu/drm/virtio`, pero `virtio-gpu` no habla con PCI ni con las IRQ: lo hace el núcleo virtio (`drivers/virtio/`: bus, anillo, transporte PCI moderno y legado, `virtio_dma_buf`), que también se compila desde las fuentes de Linux. Ahora `linuxkpi-scan.py` lo cuenta con el driver (y `fetch-linux-drivers.sh` extrae `drivers/virtio`). `virtio-gpu` pasa de 590 a **933** símbolos: K4 16 → 49, K5 40 → **253** (191 de ellos de las cabeceras virtio, que la capa tendrá que escribir; 49 de PCI y 7 de IRQ), K0 43 → 83, K1-dma 35 → 51, K2 32 → 46. Columna QEMU de la tabla con la corrección: K0 89, K1 78, K1-dma 51, K2 46, K3 36, **K4 74**, **K5 260**, K6 23, K7 36, K8 3, K9 28, K9-drm 295. Unión acumulada: `bochs` 180 → `simpledrm` 304 → `virtio-gpu` **1022** → `i915` 4128 → `xe` 4796 → `amdgpu` 5672 → `nvidia` 6322 (los demás drivers no cambian; lo que sube es lo que ya no aportan porque lo pidió antes virtio). Estimación heurística: ≈ 41 kLOC en total, ≈ 7 kLOC para el primer hito. Cobertura de la capa tras L3 sobre los 143 símbolos K4/K5 de `bochs`/`simpledrm`/`virtio-gpu` fuera de las cabeceras virtio: 137; faltan 6, de pantalla (`aperture`, `vgaarb`), para L5.

Huecos de K2 tras L2 (símbolos de la 6.18 que la capa aún no define, sin contar los generados por macros): `nop`/`clflush`/`halt` (`asm/special_insns`), `cpufeature`/`boot_cpu_data`, `irq_handler_t`/`IRQF_SHARED` (K5), `on_each_cpu`, `stop_machine`, `percpu_counter`, `synchronize_hardirq`. Todos de uso bajo y casi todos de otros niveles.

#### Medida anterior (provisional, Linux 6.8 de Ubuntu)

`scripts/linuxkpi-scan.py` → `docs/linuxkpi_api.md` (resumen) y `docs/linuxkpi_api.tsv` (tabla completa). **Medido sobre Linux 6.8 de Ubuntu y NVIDIA open 615.71.09**, no sobre la 6.18 fijada (kernel.org y GitHub bloqueados en el entorno de desarrollo); hay que repetirlo con la 6.18 en cuanto se pueda descargar. El orden de magnitud no debería cambiar entre 6.8 y 6.18, pero los nombres sí (p. ej. `simpledrm` se movió a `drm/sysfb/`, `drm_aperture` a `aperture`). Es un análisis léxico: cuenta macros, *inlines* y tipos (que la capa también debe dar), incluye todas las ramas `#if` y tiene algo de ruido.

| Nivel | Símbolos (todos los drivers) | de ellos funciones | QEMU (`bochs`+`simpledrm`+`virtio-gpu`) | Estimación anterior | **Estimación corregida** |
|---|---:|---:|---:|---|---|
| K0 Base C | 404 | 35 | 45 | ~4 kLOC | **2–3,5 kLOC** (casi todo macros) |
| K1 Memoria (sin DMA) | 430 | 96 | 61 | ~6 kLOC (con DMA) | **3–5 kLOC** |
| K1-dma DMA, `dma_buf`, IOMMU | 97 | 39 | 24 | (en K1) | **1–2 kLOC** (+ IOMMU real en L9) |
| K2 Sincronización/tiempo | 454 | 100 | 31 | ~5 kLOC | **3–6 kLOC** (RCU y `ww_mutex` son lo delicado) |
| K3 Estructuras | 349 | 89 | 28 | ~4 kLOC | **2,5–5 kLOC** |
| K4 Dispositivos | 210 | 80 | 42 | ~4 kLOC | **2–3,5 kLOC** |
| K5 PCI/IRQ/energía (+ bus `virtio`) | 386 | 140 | 46 | ~5 kLOC | **3–6 kLOC** |
| K6 Firmware/I²C/ACPI/… | 458 | 135 | 20 | ~4 kLOC | **3–6 kLOC** (mucho *stub*) |
| K7 Fences | 96 | 37 | 33 | ~3 kLOC | **1–2 kLOC** (o compilar `drivers/dma-buf/` de Linux) |
| K8 Usuario/VFS | 116 | 46 | 3 | ~5 kLOC | **1–2 kLOC** |
| K9 *stubs* (trace, kasan, hmm, cgroup…) | 181 | 59 | 25 | grande | **1–3 kLOC** |
| **K9-drm** (DRM core y helpers) | **2292** | 578 | 273 | (venía de Linux con B) | **≥15–30 kLOC** con la opción A: el *shim* sobre `kernel/drm/` (la cifra mínima; los *atomic helpers* tienen mucha semántica) |
| K9-ttm (TTM, `drm_buddy`, `drm_exec`, gpuvm) | 174 | 83 | 0 | — | **≥2 kLOC** si se compilan los de Linux sobre el *shim*; mucho más si se reescriben |
| K9-sched (`gpu_scheduler`) | 45 | 27 | 0 | — | igual que TTM |
| Sin clasificar (ruido sobre todo) | 25 | 2 | 3 | — | — |

Fórmula de la estimación: líneas por definición (función 15, inline 6, tipo 8, macro 2…) × 1–2 (la cota baja es lo que da el escáner; la alta, margen por semántica). **Total corregido de la capa sin DRM: ~22–45 kLOC** (coincide con la cota alta de la estimación anterior). **El *shim* DRM de la opción A es la pieza mayor**: `i915` solo consume 1669 símbolos de `include/drm/`, `amdgpu` 917, `virtio-gpu` 208 y `bochs` 70.

Para llegar al primer hito en QEMU (`bochs` + `simpledrm` + `virtio-gpu`) bastan **634 símbolos** (≈5–10 kLOC, de ellos 273 de DRM). Unión acumulada en el orden de trabajo: `bochs` 135 → `simpledrm` 277 → `virtio-gpu` 634 → `i915` 3877 → `xe` 4198 → `amdgpu` 5031 → `nvidia` 5717.

**NVIDIA (`kernel-open/nvidia`, `nvidia-modeset`, `nvidia-drm`, `common`; sin `uvm`):** 1672 símbolos, de los que **953 (57 %) ya los necesitan Intel/AMD**. Los 719 restantes son sobre todo memoria (NUMA, `get_user_pages`, `pin_user_pages`…: K1 116), K6 (132), K5 (75) y K4 (68); 340 son DRM (`nvidia-drm`). Es el dato para la decisión abierta 4.


## 4. Fases e hitos

Cada fase deja la suite existente (183 pruebas del kernel + pruebas de usuario) en verde. La capa vive en `kernel/linuxkpi/` (código) y `include/linuxkpi/` (cabeceras), con su propio `Makefile` y un `.kur` por driver.

### L0 — Preparación (sin hardware)
- **L0.1** Fijar versión de Linux y leer licencias; elegir licencia de la capa. *Salida:* sección «Decisiones» en este fichero.
- **L0.2** `scripts/fetch-linux-drivers.sh`: descarga (no versiona) el árbol de Linux elegido, `linux-firmware` y, para NVIDIA, `open-gpu-kernel-modules` de la versión que coincida con el firmware GSP.
- **L0.3** Medir: `scripts/linuxkpi-scan.py` extrae de los `.c` de cada driver objetivo (`bochs`, `simpledrm`, `virtio-gpu`, `i915`, `xe`, `amdgpu`, `kernel-open/nvidia*`) los **símbolos externos** que consumen y los clasifica por nivel K0–K9. *Salida:* `docs/linuxkpi_api.md` con la lista ordenada por frecuencia y el nivel. **Esto sustituye las estimaciones de arriba por datos reales** (la tabla de la sección 3 se actualiza).
- **L0.4** Sistema de compilación: regla `make linuxkpi-drv DRV=<nombre>` que compila las fuentes de un driver de Linux con `-I include/linuxkpi`, flags de kernel y `-D__KERNEL__`, y las enlaza en un `.kur` (con `kurbuild`). Revisar límites del cargador (tamaño, tipos de reubicación, `.init_array`, `.ctors`, secciones `__ksymtab*`, `__param`, `.modinfo`, `.altinstructions`, `__jump_table`, `__bug_table`).
- **L0.5** Marco de pruebas: `tests/linuxkpi/` con un `.kur` de prueba por nivel y arranque en QEMU (se integra en `kernel/tests/kernel_tests.c`).

### L1 — Base y memoria (K0, K1, K3)
- Cabeceras y funciones de K0, K3 y la parte de memoria de K1 sin DMA.
- **Hito:** un `.kur` de prueba usa `kzalloc`, `list_head`, `rb_tree`, `idr`, `printk`, `snprintf` y los resultados coinciden con los esperados (pruebas unitarias).

### L2 — Sincronización, tiempo y trabajo diferido (K2)
- `mutex`, `spinlock`, `rwsem`, `ww_mutex`, `completion`, `wait_queue`, `rcu` (aproximado: un *grace period* que espera a que todas las CPU pasen por un punto seguro), `workqueue`/`delayed_work` sobre `workqueue.c`, `timer_list`, `hrtimer`. *(Hecho el 26-oct-2026; las workqueues se escribieron en la capa y no sobre `workqueue.c`, ver §11 L2.)*
- **Hito:** pruebas de estrés con 4 CPU (contención de mutex/ww_mutex, work + timers) sin interbloqueos; una prueba de `ww_mutex` con `-EDEADLK` y reintento.

### L3 — Modelo de dispositivos, PCI e interrupciones (K4, K5)
- `struct device`, `kobject`, `driver_register`, `sysfs` básico (`/sys/class`, `/sys/bus`, atributos) sobre `sysfs.c` actual.
- `pci_register_driver` enlazado con `pci_bus` de Aurora (probe/remove, hot-plug), BARs, `ioremap`, `pci_alloc_irq_vectors`, `request_irq`/`request_threaded_irq`, MSI/MSI-X con vectores de kernel repartidos entre CPU, FLR.
- **Hito:** un driver PCI de Linux de juguete (p. ej. un `uio_pci_generic`-like o `pci-stub` con contadores) hace probe de un dispositivo QEMU (`e1000e`, `edu` de QEMU para DMA/IRQ), lee MMIO, recibe MSI-X y hace DMA.

### L4 — DMA, `sg_table`, `dma_buf`, firmware y E/S de usuario (K1-DMA, K6 básico, K8)
- `dma_alloc_coherent`, `dma_map_page/sg` con máscaras de dirección (`dma_set_mask`) y **bounce buffers** cuando la máscara no alcanza; `dma_buf` enlazado con el PRIME de Aurora.
- `request_firmware` con ficheros reales de `linux-firmware` (initrd o `/lib/firmware`), `i2c` con `i2c-algo-bit`, `debugfs` en memoria, `backlight`.
- `cdev`/`file_operations` ↔ VFS de Aurora, `ioctl`, `mmap` (`vm_operations_struct`, `remap_pfn_range`, `fault`), `poll`/`epoll`, `fdget`.
- **Hito:** driver de prueba que expone `/dev/lkpi_test` con `ioctl`, `mmap` de DMA, `poll` y firmware, ejercitado desde un programa de usuario (`tests/linuxkpi/lkpi_test.c`).

### L5 — DRM/KMS de Linux sobre la capa (K7, K9: opción B)
- Compilar `drm.ko` equivalente: `drm_drv`, `drm_file`, `drm_ioctl`, `drm_gem`, `drm_gem_shmem_helper`, `drm_atomic*`, `drm_crtc*`, `drm_connector`, `drm_edid`, `drm_syncobj`, `drm_prime`, `drm_fb_helper` (fbdev **desactivado**), `drm_mm`, `drm_vblank`, `drm_sysfs`.
- Puentes: `/dev/dri/cardN` y `renderD128+` con las mismas semánticas que hoy (los clientes no cambian), eventos `flip/vblank`, `uevent` de hot-plug.
- **Primeros drivers de Linux reales (sin hardware):**
  1. **`bochs`** (QEMU `-vga std`): `drivers/gpu/drm/tiny/bochs.c` sustituye a `bochs.aur`.
  2. **`simpledrm`**: usa el framebuffer GOP (sirve también en PC real).
  3. **`virtio-gpu`** (2D): `drivers/gpu/drm/virtio/*`, sustituye a `virtio_gpu.aur`. Después **3D virgl/Venus** (D4) cuando haya un QEMU con GL.
  4. Opcionales para ampliar la cobertura: `cirrus`, `qxl`, `vmwgfx` (vmsvga).
- **Hito:** `drm_test`, `modetest`, `kmscube`, Weston y `aurora-wayland` pasan **sobre el DRM de Linux** en QEMU igual que hoy; se sustituye `kernel/drm/` (el antiguo queda tras una opción de `Makefile`).
- Aquí se **resuelven D1 y D2** a nivel de núcleo: multi-CRTC, hot-plug y modo dinámico son capacidades normales del DRM de Linux; falta adaptar el compositor.

### L6 — `ttm`, scheduler de GPU y memoria de dispositivo (K9)
- `ttm` (buffer objects con VRAM/GTT), `gpu-sched`, `dma_resv`, `drm_buddy`, `shrinker` (mínimo), `swap`/`shmem` (páginas «pageables» respaldadas por RAM; sin disco de swap), `mmu_notifier` (stub), `hmm` (desactivado).
- **Hito:** un driver de memoria de dispositivo simulado (un `.kur` de prueba que usa `ttm` con un «VRAM» en RAM) crea, mapea y evicta buffers; las pruebas de estrés de GEM/`ttm` pasan.

### L7 — Intel (primer hardware real)
- **Alcance: todas las generaciones de Intel que traiga `i915`/`xe` de la versión de Linux fijada**, no una GPU concreta. `i915` cubre desde Gen2 hasta Gen12 (según la serie de Linux) y `xe` Gen12+ en adelante; el objetivo es compilar y alojar esos dos drivers completos, con las opciones de cada generación (GuC/HuC solo donde existan, power wells, display engine DDI, etc.).
- **Bancos de prueba reales (no limitan el alcance):** el portátil del autor, Toshiba Portégé R30-A con Core i5-4210M e Intel HD Graphics 4600 (Haswell, Gen7.5, confirmado el 26-oct-2026), es la primera máquina; cualquier otra generación disponible se añade a la matriz de pruebas (§6) y se valida igual.
- Aquí por primera vez se necesitan: `vga_client`, `acpi` (opregion, `_DSM`, `acpi_video`), `intel_gtt`/GGTT, `i2c` GMBUS, `power wells`, `rc6` desactivado, `GuC/HuC` (firmware) según generación, `drm_display_helper` (DP/HDMI), reset de GPU, DMAR/IOMMU (K5 + L9).
- **Hitos:** (1) `i915` hace probe y muestra la identidad por serie; (2) pantalla nativa con GOP de respaldo; (3) 3D con el driver de Mesa que corresponda a cada generación (de memoria: `crocus` para las antiguas hasta Gen7.5, `iris` desde Gen8, `anv` para Vulkan; confirmar el reparto exacto contra la versión de Mesa fijada) + `aurora-wayland` con `linux-dmabuf`. Estos tres hitos se repiten por generación disponible.
- Pruebas: **PC real** con log serie; o **VFIO + KVM** (passthrough de la iGPU) desde Linux en ese PC.

### L8 — AMD (`amdgpu`) y NVIDIA (`kernel-open/`)
- `amdgpu`: tamaño enorme (~5 M de líneas); el objetivo es el driver completo para las GPU de la serie de Linux fijada; lo que se **desactiva** es lo que no sea gráfico o necesite piezas que Aurora no tiene (SVM/HMM, KFD/compute), como decisión de configuración y no por limitarse a una GPU concreta. Requiere `ttm`, `gpu-sched`, `drm_buddy`, `ioremap` de BARs grandes (ReBAR), `acpi` y firmware.
- NVIDIA: alojar `kernel-open/` (`nvidia.ko`, `nvidia-modeset.ko`, `nvidia-drm.ko`) sobre esta capa, con el núcleo abierto `src/nvidia`/`src/nvidia-modeset` como objetos binarios del paquete de NVIDIA. Fases N1–N5 de `Plan_NVIDIA.md`. **Alternativa**: capa de SO propia (ver nota de ese plan).
- Pruebas: RTX 4060 por serie o VFIO.

### L9 — IOMMU/VT-d, seguridad, energía (en paralelo desde L4)
- **IOMMU/VT-d (8.0)**: dominio por dispositivo, `dma_map_*` con tablas de IOMMU, aislamiento de DMA (si no, un driver de GPU puede escribir cualquier RAM).
- Energía: `pm_runtime` real (D0/D3), `suspend/resume`, ASPM y `PCIe link state`, `pci_reset_function`.
- Endurecimiento: pruebas de fuzz de los ioctls de DRM, límites de memoria por cliente, *watchdog* de GPU (`TDR`).

## 5. Qué hay que tocar en Aurora (puntos de integración)

| Cambio en el kernel de Aurora | Fase |
|---|---|
| Cargador `.kur`: reubicaciones adicionales, secciones de `modinfo/param`, `__ksymtab_gpl`, módulos grandes (> 50 MB), versiones/`vermagic` opcional — **hecho en L0.4** salvo módulos > 64 MiB (lectura por trozos, L8) y `vermagic` (no hace falta: `EXPORT_SYMBOL*` va a `.kur_ksymtab`) | L0 |
| `ksymtab`: exportar los símbolos de la capa con nombres de Linux; `EXPORT_SYMBOL_GPL` como alias | L0–L1 |
| `kernel/drm/`: opción de compilación «propio» / «Linux sobre LinuxKPI» | L5 |
| VFS: `cdev` genérico + `file_operations` completo (`fault`, `llseek`, `poll`, `fasync`) — **hecho en L4** (operaciones por fd en `vfs_ops`, `VMA_DEV`, `F_SETOWN`/`O_ASYNC`) | L4 |
| `sysfs`: crear atributos desde `kobject`/`device` (`drm/cardN`, `class/drm`, `bus/pci/drivers`) — **hecho en L3** (atributos con `show`/`store` en `sysfs_dyn`; faltan los binarios) | L3 |
| `udev`/`uevent`: eventos `change` (hot-plug de conector) hacia `libudev` | L5 |
| Memoria: `shmem` para objetos GEM, páginas con atributos WC/UC, `vmap`, límites por proceso — `vmap` y WC (región `vmap`) **hechos en L4**; falta quitar el alias WB de la ventana directa | L1/L4/L6 |
| IOMMU/VT-d y `dma_map` real | L9 |
| `task_struct` mínimo visible a drivers (`current->mm`, `pid`, `comm`) sobre `task_t` — **hecho en L4** (sin campo en `task_t`) | L4 |

## 6. Estrategia de pruebas (sin y con hardware)

1. **Unitarias de la capa** (`tests/linuxkpi/`): cada nivel con un `.kur` de prueba, dentro de la suite del kernel.
2. **Drivers de Linux reales que funcionan en QEMU** (no necesitan GPU): `bochs`, `simpledrm`, `virtio-gpu` (+ `edu` de QEMU para DMA/IRQ, `e1000e` para PCI/IRQ y `snd-hda`/`virtio-*` para variedad). Son el banco de pruebas principal.
3. **Regresión**: suite del kernel (183 al empezar L0; 191 tras L2), `drm_test`, `sig_test`, `rseq_test`, Weston, `aurora-wayland` (X11 y Wayland), `clipboard-x11.sh`, `primary-icons.sh`. **Ninguna fase se da por terminada si empeora.**
4. **Estrés en SMP**: ejecución con `-smp 4` y TCG; con KVM cuando haya (medirá el rendimiento de verdad, D9).
5. **Hardware real** (L7–L8): PC con **log serie** siempre; passthrough VFIO + KVM desde Linux para iterar sin reiniciar; `screendump` por cámara si no hay captura.
6. **Análisis estático**: `linuxkpi-scan.py` en CI para detectar símbolos faltantes cuando se actualice la versión de Linux.

## 7. Riesgos y mitigaciones

| Riesgo | Mitigación |
|---|---|
| Superficie de API enorme y movediza (Linux cambia cada versión) | versión fijada; `linuxkpi-scan.py` para medirla; migraciones deliberadas, no continuas |
| Semántica sutil (RCU, `ww_mutex`, `dma_resv`, orden de memoria) que rompe drivers en carga | empezar por drivers pequeños (`bochs`, `simpledrm`, `virtio-gpu`); estrés SMP; `lockdep` ligero propio opcional |
| `i915`/`amdgpu` usan APIs que Aurora no tiene (HMM, `mmu_notifier`, `cgroups`, `perf`, `trace`) | stubs documentados y desactivar opciones (`CONFIG_*`) como hace FreeBSD |
| Rendimiento (copias de DMA, bounce buffers) | medir con KVM; evitar bounce con máscaras de 64 bits |
| Seguridad (Ring 0, DMA sin IOMMU) | L9 en paralelo; no cargar drivers de GPU en equipos no confiables hasta tener IOMMU |
| Licencia (GPL) | aceptada (D5); capa y drivers en directorios y `.kur` separados; ficheros de firmware no versionados |
| Tiempo (meses) y sin GPU en el entorno | hitos observables en QEMU hasta L6; L7–L8 solo con hardware del autor (VFIO/serie) |
| Pantalla en negro en hardware real | GOP intacto; temporizador de reversión; `gpu=off` en la línea de comandos |
| Cargador `.kur` insuficiente para módulos enormes | L0.4 lo descubre pronto |

## 8. Orden global

`L0 → L1 → L2 → L3 → L4 → L5 → L6 → L7 (Intel) → L8 (AMD/NVIDIA)`, con `L9` (IOMMU/energía/seguridad) en paralelo desde L4 y **obligatoria antes de L7**. Los hitos observables en QEMU llegan hasta **L6**; L7 y L8 dependen del hardware.

## 9. Lo que se puede empezar ya (sin hardware)

L0.1–L0.5 y L1: cabeceras base, `kmalloc`/`list`/`rbtree`/`idr`, pruebas en la suite y el escáner de símbolos. El escáner dará la primera lista real de qué consumen `bochs`, `simpledrm` y `virtio-gpu`, y servirá para afinar todo este plan.

## 10. Decisiones abiertas

1. ~~**DRM A o B**~~ → **decidido: A** (ver sección 11, 26-oct-2026).
2. ~~**Versión de Linux**~~ → **decidido: 6.18 LTS** (sección 11).
3. ~~**Licencia de la capa**~~ → **decidido: GPL-2.0-only** (sección 11).
4. **NVIDIA**: `kernel-open/` sobre LinuxKPI **o** capa de SO propia (decidir tras L0.3 con los datos reales).
5. **Primer driver en hardware real**: el portátil Intel (necesita `lspci -nn` y log serie).
6. ~~**Punto exacto de 6.18.y** y **etiqueta de `linux-firmware`**~~ → **6.18.59** y **20261019** (sección 11); L0.3 repetida, también con NVIDIA 615.78.08.
7. ~~**RCU antes de usar `xarray`/`idr` con lectores concurrentes**~~ → hecho en L2 (`kfree_rcu` en `xa_node_free`).
8. **Cambios en Aurora tras L2**: ~~plazos de espera a 1 ms~~ y ~~`sched_current()` atómico~~ → hechos (§11, «Versión fijada, `sched_current()`, plazos a 1 ms»). ~~`current` estable y `signal_pending`~~ → resuelto en L4 sin campo en `task_t` (tabla validada por tid y `signal_unblocked_of` de Aurora).
9. **Abiertas tras L3** (§11, «L3»): (a) ~~opción de arranque para que `aur_bus` no reclame los ID que deba llevar un driver de Linux~~ → hecho: `aur.skip=` (§11, «Tras L3»); (b) `on_each_cpu()`/IPI de función (`IPI_VECTOR_CALL` sin handler; L7, `i915`); (c) ECAM para el espacio de configuración extendido (L7/L8); (d) ampliar los 21 vectores MSI de kernel o darles espacio por CPU (L7/L8); (e) ~~`irq_user.c` fuerza INTx activas a nivel bajo en líneas que la MADT de QEMU declara activas a nivel alto~~ → corregido (§11, «Tras L3»); (f) uevents hacia el userland (L5).
10. **Abiertas tras L4** (§11, «L4: resultados»): (a) WC sin alias WB en la ventana directa (L7/L9); (b) `dma_resv` con fences y fence implícito de PRIME (K7, L5); (c) Aurora: `poll` no filtra `revents` por los eventos pedidos y `/dev/kmsg` incremental se salta líneas al rotar el anillo (arreglos pequeños fuera del alcance; a decidir); (d) `/dev/i2c-N` y atributos binarios de sysfs.

## 11. Decisiones (registro)

Cada entrada lleva fecha. Cambiar una decisión requiere discutirlo y añadir una entrada nueva (no editar la anterior).

### 26-oct-2026 — L0.1
- **Versión de Linux: serie 6.18 LTS.** Es la LTS más reciente (diciembre 2025, soporte hasta al menos finales de 2027). Trae `i915` para Haswell (Gen7.5, el portátil) y Alder Lake, y `xe` para Gen12+ (Alder Lake solo con `force_probe`); las ramas recientes de `open-gpu-kernel-modules` compilan contra ella. La versión exacta está en `kernel/linuxkpi/linux-version.mk` (una sola fuente, la leen make y bash). **Provisional: `LINUX_VERSION=6.18` (base)**, porque desde el entorno de desarrollo `kernel.org` está bloqueado y no se pudo consultar el último `6.18.y`; pasar a ese punto estable se hará en una entrada nueva.
- **DRM: opción A** (decisión del autor, contra la recomendación B de la sección 2). Se mantiene `kernel/drm/` propio y la capa aportará un *shim* con las funciones `drm_*` que usen los drivers. Consecuencias: (1) la superficie `include/drm/*` (K9-DRM) la escribe Aurora; `docs/linuxkpi_api.md` la mide aparte para dimensionar el *shim*; (2) multi-CRTC, hot-plug (D1) y cambio de modo (D2) no vienen «gratis» del DRM de Linux: hay que generalizar `kernel/drm/drm_core.c` (hoy un solo CRTC virtual); (3) L5 cambia de «compilar `drm.ko`» a «*shim* + `bochs`/`simpledrm`/`virtio-gpu` contra él». L0–L4 no cambian.
- **Licencia de la capa: GPL-2.0-only.** Igual que los drivers que aloja. Vive en `kernel/linuxkpi/` e `include/linuxkpi/`, se enlaza en su propio `.kur` (`linuxkpi.kur`) y no la incluye el resto del kernel. Cada fichero lleva `SPDX-License-Identifier: GPL-2.0-only`; si se toma código de FreeBSD `linuxkpi` se conserva su aviso BSD-2-Clause (compatible).
- **NVIDIA (`kernel-open` sobre LinuxKPI o capa propia):** sigue abierta; se decide con los datos de L0.3 (`docs/linuxkpi_api.md`).
- **Fuentes externas:** `scripts/fetch-linux-drivers.sh` → `build/linuxkpi_src/` (ignorado por git). `linux-firmware` sin fijar todavía (no hace falta hasta L4).

### 26-oct-2026 — L0.4, L0.5 y L1 (diseño)
- **Cargador `.kur`** (`kernel/kur_loader.c`): sin tope de secciones (`sec_off[]` por índice, índices extendidos), carga de toda sección `SHF_ALLOC`, `.preinit_array`/`.init_array`/`.fini_array`, relocaciones `PC64` y `GOTPCREL*` (GOT propia), `__start_SEC`/`__stop_SEC`, `_GLOBAL_OFFSET_TABLE_`, débiles a 0, error explícito si se usa un símbolo de una sección no cargada (antes se resolvía en silencio). Exports en tabla hash sin límite (antes 256 y nombres de 63 caracteres) y prohibido tapar un símbolo del kernel. `KUR_MAX_SIZE` 16 → **64 MiB**: es el tope de `kmalloc` (`heap.c`) y `kur_loader_load_file()` lee el fichero entero. **Pendiente para NVIDIA (L8)**: leer módulos mayores por trozos.
- **Nombres**: todo lo que exporta `linuxkpi.kur` lleva el prefijo **`lkpi_`**; las cabeceras ofrecen el nombre de Linux como `static inline` o macro. Así un driver no puede enlazar por accidente con un símbolo homónimo de Aurora (`kmalloc`, `device_register`, `request_firmware`… existen en el ksymtab con otra semántica). Excepción: `memcpy/memset/memmove/memcmp`, que el compilador puede emitir solo y vienen del kernel.
- **Puente con Aurora**: la capa no incluye cabeceras de Aurora. `kernel/linuxkpi/aurora.h` declara cada servicio con nombre `aurora_*` y etiqueta asm del símbolo real; `kernel/ksymtab_exports.c` los exporta. Si cambia una firma de Aurora hay que cambiarla allí.
- **Compilación** (`kernel/linuxkpi/flags.mk`): como un módulo de Linux x86-64 (`-nostdinc`, sin SSE/x87, `-mcmodel=kernel`, `-include linux/kconfig.h -include linux/compiler_types.h`, `-ffunction-sections`). `CONFIG_*` fijos en `include/linuxkpi/lkpi/config.h` (solo lo que la capa implementa). `make linuxkpi-drv DRV=<n>` usa un descriptor `kernel/linuxkpi/drivers/<n>.mk` (o `tests/linuxkpi/<n>/drv.mk`); `linuxkpi.kur` y las pruebas se instalan en `/lib/modules/linuxkpi/` (no se cargan solos al arrancar).
- **`struct page`** propio: `alloc_pages()` da un array contiguo de descriptores (`page + i` vale dentro de la asignación) y un registro hash pfn → página resuelve `pfn_to_page`/`virt_to_page`; para marcos ajenos se crea un descriptor suelto (vive hasta descargar la capa). No hay `mem_map` global: si un driver necesita `page + i` sobre memoria que no salió de `alloc_pages`, habrá que revisarlo (FreeBSD tiene el mismo problema).
- **`kmalloc`** propio sobre páginas del PMM: físicamente contiguo, alineación natural en potencias de dos y `virt_to_page` válido, como en Linux (el `kmalloc` de Aurora no garantiza nada de eso: cabecera de 64 bytes por página en el slab y heap virtual). `kmem_cache` con constructor, `SLAB_HWCACHE_ALIGN` y alineación.
- **`vmalloc`** sobre el heap de Aurora (virtualmente contiguo, alineado a página; ≤ 64 MiB por bloque). `vmap()` de páginas arbitrarias queda para L4.
- **`ioremap`**: la ventana MMIO de Aurora asigna la virtual por la física, así que una página ya mapeada por Aurora (LAPIC, framebuffer…) se reutiliza y nunca se desmapea; las de la capa se cuentan y se desmapean cuando ningún `ioremap` vivo las cubre. `memremap(WB)` de RAM es la ventana directa.
- **`xarray`** propio (radix de 64 ranuras con bitmaps de presencia/lleno/marcas); `idr`/`ida` encima. Lecturas sin bloqueo, como en Linux; **liberar nodos con lectores concurrentes necesita RCU (L2)**: hoy `xa_node_free` libera al momento.
- **Spinlock mínimo en L1** (lo necesita `xarray`): desactiva la expulsión de Aurora; `_irq/_irqsave` también las interrupciones. K2 completo (mutex, rwsem, ww_mutex, RCU, workqueue, timers) es L2.
- **Tiempo**: `HZ = 1000` (el tick de Aurora); `ktime_get()` tiene la resolución del tick (1 ms) hasta usar el TSC.
- **Pruebas** (`tests/linuxkpi/`): cada `.kur` de prueba comprueba en `module_init` y cuenta en la suite con `test_pass`/`test_fail` del kernel; `kernel/tests/kernel_tests.c` tiene 6 pruebas `linuxkpi: …` (cargador, capa, K0, K1, K3, descarga). **Suite del kernel: 189/189** en QEMU (183 previas + 6).
- **Fuentes de Linux**: `kernel.org` y GitHub están bloqueados en el entorno de desarrollo. `scripts/fetch-linux-drivers.sh` descarga la 6.18 y NVIDIA cuando haya red; `--ubuntu` toma Linux 6.8 y NVIDIA 615.71.09 del archivo de Ubuntu solo para desarrollar el escáner (`docs/linuxkpi_api.md` es provisional por eso).

### 26-oct-2026 — L2 (K2: sincronización, tiempo y trabajo diferido)
Decisiones tomadas con el autor antes de escribir código:
- **RCU: contador de estados quiescentes por CPU en el tick** (opción A). Único cambio en el planificador de Aurora: `sched_tick()` incrementa `g_rcu_qs_count[cpu]` si la tarea actual tiene `preempt_count == 0`, y lo exporta como `sched_rcu_qs_count(cpu)`. `rcu_read_lock()` es `preempt_disable()` (RCU no expulsable), así que un tick con la expulsión activa cae fuera de cualquier lectura. `synchronize_rcu()` toma una foto de los contadores de las CPU en línea y espera a que avancen todas (≈ 8–16 ms en la práctica, por la resolución de los plazos; ver abajo). Descartadas: un kthread fijado a cada CPU (periodo de ~10 ms + IPI) y un contador global de lectores (contención e inanición).
- **K2 se escribe en la capa** sobre un único servicio de Aurora: aparcar y despertar una tarea. Cada `task_struct` lleva una `wait_queue_t` de Aurora propia; `schedule()` se aparca solo si `__state != TASK_RUNNING` y `wake_up_process()` pone `TASK_RUNNING` y desaparca, ambos bajo el cerrojo de esa cola (como `futex.c`). Encima, con la semántica de Linux: `wait_queue_head` (funciones de despertar, exclusivas, `wait_woken`), `completion`, `mutex` (propietario, traspaso FIFO), `ww_mutex`, `rwsem`, `semaphore`, `schedule_timeout`, `workqueue`, `timer_list`, `hrtimer`, tasklets e `irq_work`. No se envuelven `wait.c`/`completion.c`/`workqueue.c`/`mutex.h` de Aurora: no tienen la semántica de Linux y cambiarla afectaría al resto del kernel (ver «Contradicciones» en `docs/LinuxKPI_Sesion_L2.md`).
- **Cambios en Aurora aceptados: solo el gancho de RCU.** Rechazados por ahora: (b) plazos de espera a 1 ms, (c) puntero `lkpi_task` en `task_t` y (d) exportar `signal_pending`. Consecuencias:
  - Los plazos (`schedule_timeout`, `msleep`, `timer_list`, `hrtimer`, `delayed_work`) tienen la resolución de `sched_wake_expired()` (cada 8 ticks): pueden llegar **hasta ~8 ms tarde**, nunca antes. `usleep_range()` con mínimo < 2 ms hace espera activa con el TSC.
  - `current` se busca en una tabla hash `task_t* → task_struct` sin cerrojo para leer. Las entradas no se liberan hasta descargar la capa; las de kthreads terminados se reutilizan. Si Aurora reutiliza la dirección de un `task_t` muerto de una tarea ajena, la nueva hereda su descriptor (pid/comm informativos en K2).
  - `signal_pending()` devuelve 0: las variantes `_interruptible`/`_killable` no se interrumpen (K8, L4).
- **Timers, hrtimers, tasklets e `irq_work` corren en un hilo** (`lkpi_softirqd`), con la expulsión desactivada durante cada función, no en la IRQ del tick. Así `spin_lock_bh()` puede ser un `spin_lock()` normal (el hilo no expulsa a quien tiene el cerrojo en su CPU) y `in_softirq()` es cierto dentro de esas funciones. `in_hardirq()` es 0 hasta L3.
- **Workqueues**: grupo de hilos compartido (`lkpi_wq/N`, mínimo `max(2, CPUs)`, máximo 64) con un gestor (`lkpi_wq_mgr`) que crea hilos cuando hay trabajo, ninguno libre y menos hilos *ejecutándose* (no dormidos) que CPUs; los inactivos 10 s terminan. Semántica de Linux: reencolar en curso vuelve a ejecutar, no reentrada, `max_active` por cola (ordenadas FIFO), `flush_work` con barrera (espera solo a esa instancia), `cancel_*_sync`, `mod_delayed_work`, `rcu_work`. `disable_work_sync()` es de momento `cancel_work_sync()` (no impide encolar después).
- **`ktime_get()` con el TSC** (frecuencia calibrada por Aurora, exportada `klog_get_tsc_freq`): ancla en un flanco de tick al cargar la capa, `ns = ancla + ciclos·mult >> 32` y un máximo global para que sea monótono entre CPU. `udelay`/`ndelay`/`mdelay`: espera activa sobre el TSC. `jiffies` sigue siendo el tick (1 ms).
- **SRCU básico**: dos contadores globales por `srcu_struct` (no por CPU) y cambio de índice. `call_srcu()` va por `system_unbound_wq`.
- **percpu básico**: `DEFINE_PER_CPU` es un array de `NR_CPUS` con nombre interno; se usa por nombre (`per_cpu(v, cpu)`, `this_cpu_inc(v)`), no con `this_cpu_ptr(&v)`. `alloc_percpu(tipo)` da `NR_CPUS` copias seguidas y `per_cpu_ptr(p, cpu)` avanza `sizeof(*p)`.
- **CPU en línea** = CPU con ticks quiescentes al cargar la capa (sin hotplug). `smp_processor_id()` lee `%gs:16` como Aurora.
- **`xa_node_free()` → `kfree_rcu()`** y las lecturas sin cerrojo de `xarray` (`xa_load`, `xa_find`) van bajo `rcu_read_lock()`: pendiente de L1 resuelto.
- **Pruebas**: `lkpi_test_k2` (funcional, `TEST_FLAG_BLOCKING`) y `lkpi_test_k2_smp` (estrés, `TEST_FLAG_BLOCKING | TEST_FLAG_NEEDS_SMP`). Se separan para que la funcional cuente aunque no haya SMP.
- **Hallazgos del estrés con 4 CPU** (corregidos en la capa):
  - *Despertar perdido en wound-wait*: la víctima comprobaba `wounded` antes de `set_current_state()`, y quien la hiere escribe y despierta desde el `wait_lock` de otro cerrojo. Ahora el estado se fija al principio de cada vuelta, antes de mirar las condiciones (como en Linux).
  - *`sched_current()` de Aurora no es atómico*: lee `this_cpu(current_task)` en dos pasos (`%gs:16` y luego `cpu_local_data[cpu]`). Si la tarea se expulsa y migra entre ambos, devuelve la tarea de su CPU anterior. Con eso, `preempt_disable()` incrementaba el contador de **otra** tarea, que quedaba no expulsable y, al terminar, ocupando su CPU para siempre en `kthread_do_exit` (y el periodo de gracia de RCU no avanzaba en esa CPU). La capa lee `current` y `preempt_count`, y llama a `preempt_disable()`, `sched_yield()` y `kthread_stop()` de Aurora (que leen la tarea actual por dentro) con las IRQ desactivadas. Un interbloqueo de wait-die visto una vez antes de esta mitigación no se ha reproducido (6/6 en verde después); no se capturó su estado. **Pendiente en Aurora** (no se ha tocado, a decidir por el autor): leer `current_task` con una sola instrucción relativa a `%gs`. El fallo afecta también a los usos internos de Aurora (`sched_yield`, `wait.c`, `kthread_should_stop`, `preempt_disable` del compositor y del *shootdown* de TLB).

### 26-oct-2026 — Versión fijada, `sched_current()`, plazos a 1 ms (tras L2)
Con confirmación del autor:
- **Linux 6.18.59** (el último `6.18.y` en `kernel.org/releases.json` ese día) y **`linux-firmware` 20261019** (`kernel/linuxkpi/linux-version.mk`). L0.3 repetida sobre la 6.18.59 (§3.1). GitHub sigue bloqueado (403 del proxy), así que NVIDIA open 615.78.08 no se ha descargado: el escaneo usa la 615.71.09 de Ubuntu para NVIDIA (resuelto el mismo día: ver abajo).
- **`sched_current()` atómico** en Aurora: `this_cpu_current_task()` (`kernel/cpu.h`) lee `%gs:current_task` con una instrucción; lo usan `sched_current()` y `sched_tick()`. Se retiran las mitigaciones de la capa (llamadas con las IRQ desactivadas).
- **Plazos de espera a 1 ms**: `sched_lock` guarda el plazo más cercano; `time_tick()` llama a `sched_wake_expired()` en el tick en que vence (además de cada 8 ticks). **Y una expulsión acotada**: si lo que despierta un plazo no encuentra CPU ociosa, se marca `need_resched` en la CPU cuya tarea lleva más tiempo seguida (sin eso esperaría la rodaja de 10 ticks). Solo para despertares por plazo; el resto de despertares no cambia (Aurora sigue sin *wakeup preemption* general). Prueba nueva del kernel (`sched: plazos de espera con resolución de 1 ms`) que mide el instante del despertar: 12 de 12 a tiempo; con el código anterior, 1 de 12.
- **Aproximaciones de la capa retiradas**: `disable_work`/`enable_work` reales (cuenta de desactivación), SRCU con contadores por CPU, y percpu estático por alias de la copia 0 (valen `this_cpu_ptr(&v)` y `per_cpu_ptr(&v, cpu)`; sigue sin valer `this_cpu_inc(p->campo)`).
- Siguen rechazados `current` estable en `task_t` y `signal_pending` (K8, L4).
- **NVIDIA open 615.78.08** descargada (mismo día): el tarball de GitHub sigue dando 403 en el proxy, pero `git` sí llega, así que `scripts/fetch-linux-drivers.sh` clona la etiqueta (`--depth 1`) si falla el tarball. L0.3 repetida: NVIDIA 1679 símbolos (antes 1675), 992 compartidos con Intel/AMD (antes 990); total 6106.


### 26-oct-2026 — L3 (K4/K5: modelo de dispositivos, PCI e interrupciones)
Preguntas de diseño hechas en una sola ronda; el autor delegó la elección («lo más profesional y acorde con el objetivo y entorno actual»). Se tomaron las recomendaciones:
- **Convivencia con los `.aur`/`.kur`**: cada `pci_driver` de Linux es un driver «sombra» en el `pci_bus` de Aurora (`bus_add_driver`, id comodín). Aurora decide: el primero que reclama un dispositivo se lo queda, también en hot-plug; la sombra crea el `pci_dev`, comprueba la `id_table` de Linux (`-ENODEV` si no casa: Aurora sigue con el siguiente driver) y vincula el driver en el modelo de la capa. En el bus `pci` de la capa no hay match propio. Un dispositivo que ya reclama un `.aur` (hoy `bochs` 1234:1111, `virtio_gpu` 1af4:1050, `e1000e` 8086:10d3) **no** se ofrece a Linux. Para L5 (bochs/virtio-gpu de Linux) hará falta una opción de arranque para que `aur_bus` no reclame ciertos ID (pendiente, no hecha).
- **Contexto de IRQ: la IRQ real.** `request_irq()` corre el handler dentro de la IRQ de Aurora (IF=0, antes del EOI): `in_hardirq()` es cierto (contador por CPU de la capa) y `preempt_count()` suma `HARDIRQ_OFFSET`. La parte con hilo va en un kthread de la capa por acción; `IRQF_ONESHOT` enmascara las INTx (nivel) hasta que acaba el hilo; las MSI (flanco) que llegan deshabilitadas se reproducen en `enable_irq()`. `IRQF_SHARED` vale entre drivers de la capa; una línea que ya tiene Aurora da `-EBUSY`.
- **`sysfs` en el `/sys` real**: los kobject de la capa se publican en el registro dinámico de Aurora (`sysfs_dyn`), con atributos que llaman a `show`/`store`. `/sys/devices/...`, `/sys/bus/<bus>/{devices,drivers}`, `/sys/class/<clase>`; para PCI, `/sys/devices/pci0000:00/<BDF>` es de la capa y `/sys/bus/pci/devices/<BDF>` sigue siendo el de Aurora (la capa le añade el enlace `driver`). Los atributos binarios y los uevents no salen todavía (L4/L5).
- **Dispositivos del hito: `edu` y `virtio-rng-pci`** (comprobado en QEMU 8.2.2: `edu` solo tiene INTx y MSI de un vector, sin MSI-X; `e1000e` lo reclama un `.aur`). `edu` para MMIO/INTx/MSI/DMA y virtio-rng moderno para MSI-X con virtqueue. `scripts/qemu-drive.py` los añade siempre; las pruebas se omiten si faltan.
- **`on_each_cpu()`: aplazado a L7** (no lo usan `bochs`, `simpledrm` ni el núcleo virtio; sí `i915`). `IPI_VECTOR_CALL` existe en `ipi.h` pero no tiene handler.
- **FLR**: implementado (`pcie_flr`, `pci_reset_function` con guardar/restaurar la cabecera y MSI/MSI-X) pero **sin probar**: la máquina `pc` de QEMU no tiene dispositivos PCIe con FLR; solo se prueba el camino `-ENOTTY`. Sin reset de bus ni por PM.
- **`platform_bus` básico ya en L3** (lo necesita `simpledrm` en L5).
- **Cambios en Aurora (aceptados)**: `bus_del_driver()` desvincula (llama a `remove`); `irq_free()` espera al `ack` que esté corriendo en otra CPU; `pmm_alloc_pages_below()`; `smp_cpu_lapic_id()`; atributos con `show`/`store` en `sysfs_dyn` (sin tope de 512 entradas, ficheros escribibles, mezcla con `/sys/bus` y `/sys/bus/pci/devices/<BDF>`); `ioapic_route_pci_intx()` (polaridad del ISO de la MADT si existe, si no nivel/activa baja); exports para la capa y las pruebas. Cada cambio con prueba del kernel donde tiene sentido (3 nuevas).
- **No se cambió en Aurora**: MSI/MSI-X los programa la capa (espacio de configuración + su propio `ioremap` de la tabla), no `pci_enable_msi/msix` de Aurora, para tener destino por vector, máscara por entrada y afinidad, y no compartir el mapeo de la tabla. Los vectores siguen siendo globales (21 de kernel, una IDT para todas las CPU): lo que se reparte entre CPU es el destino (LAPIC) de cada vector, por turnos o con `PCI_IRQ_AFFINITY` uno por CPU.
- **DMA provisional**: `dma_alloc_coherent()` con páginas del PMM, alineadas al orden del tamaño y por debajo de `coherent_dma_mask` (edu: 28 bits); dirección de bus = física (sin IOMMU). `dma_map_*`/`sg_table`/bounce son L4.
- **Espacio de configuración**: solo 256 bytes (Aurora usa 0xCF8/0xCFC, sin ECAM): `pci_find_ext_capability()` devuelve 0, sin SR-IOV/AER/BAR redimensionable. Hará falta ECAM para `amdgpu`/NVIDIA (L7/L8). Los BARs los mide la capa al vincular (con la decodificación apagada): `pci_device_t.bar` de Aurora es de 32 bits.

### 27-oct-2026 — Tras L3: `aur.skip`, polaridad de las INTx y medida de `virtio-gpu`
A petición del autor (puntos 1, 2 y 5 de lo pendiente de L3):
- **Línea de comandos del kernel y `aur.skip`.** Los bootloaders UEFI y BIOS ya leían `cmdline=` de `aurora.conf` pero no la pasaban: ahora va en `kernel_boot_info.cmdline` (256 bytes; la estructura pasa de 408 a 664) y `kernel/cmdline.c` la guarda (`cmdline_get`). Opción `aur.skip=VVVV:DDDD[,…]` (`DDDD` puede ser `*`): `aur_bus` no lanza el `.aur` de esos dispositivos y quedan libres para un driver del kernel, p. ej. `bochs`/`virtio-gpu` de Linux en L5 (`cmdline=aur.skip=1234:1111,1af4:1050`). Elegida frente a un fichero en `/etc` porque `aurora.conf` está en la ESP y se edita sin rehacer el initrd. No hay `/proc/cmdline` todavía.
- **Polaridad de las INTx de los `.aur` y del AHCI**: `irq_user.c` y el modo sin MSI de `ahci.c` usan `ioapic_route_pci_intx()` (ISO de la MADT si lo hay; si no, nivel bajo). Comprobado que el IOAPIC de QEMU no aplica la polaridad (la INTx de `edu` llega igual con nivel bajo forzado): el fallo solo podía verse en hardware real.
- **Medida de `virtio-gpu` con el núcleo virtio** (§3.1, «Corrección del 27-oct-2026»). Con ella se completó lo que faltaba de K4/K5 para el primer hito y no es de pantalla: device tree como Linux sin `CONFIG_OF` (`of.h`, `of_clk.h`, `of_reserved_mem.h`), `pm_domain.h` sin dominios, `device_link` mínimo (registra, no ordena probe ni energía), `core_initcall` y compañía, `PCI_POSSIBLE_ERROR`. Quedan `aperture` y `vgaarb` para L5.

### 27-oct-2026 — Cuelgue de `bash`/`wl-copy` en `primary-icons.sh` (Aurora)
Investigado porque salió en 4 de 5 ejecuciones de la regresión tras L3 (regla de §2: «si sale, no la ignores»). Causa: `process_exit()` marcaba `TASK_DEAD` antes de despertar al padre; una expulsión en ese hueco dejaba al padre para siempre en `wait4()`. Decisiones:
- `TASK_DEAD` solo al final de `process_exit_current()`; `free_reaped_zombie()` espera a `TASK_DEAD` y `on_cpu == 0` cediendo la CPU (`2522f377`). Con el arreglo, 17 de 17 (antes ~1 de cada 3 con los `.kur` de prueba en paralelo). Detalle y tabla en `docs/LinuxKPI_Sesion_L3.md` §9.
- Se mantiene `wake_pending` en `wait_queue_wake_task()` (`fb581634`): carrera real aunque no era la causa.
- **No** se hacen ahora `alarm`/`setitimer`/`timer_create` (hoy no hacen nada y `timeout` nunca vence): se propone al autor como trabajo de Aurora aparte. (Hecho el mismo día, ver la entrada siguiente.)

### 27-oct-2026 — Correcciones de Aurora tras la investigación del cuelgue
Con el visto bueno del autor («los que puedas solucionar y que no estén ya marcados en fases posteriores»). Detalle, pruebas y commits en `docs/LinuxKPI_Sesion_L3.md` §10. Decisiones:
- **Temporizadores de proceso** en `kernel/ptimer.c` (un kthread que duerme hasta el vencimiento más cercano, como `timerfd`), no en el tick: la señal sale de contexto de tarea con `process_kill`. `ITIMER_VIRTUAL`/`PROF` sí en el tick y cuentan todo el tiempo de CPU (Aurora no separa usuario y sistema). Relojes de CPU y `*_ALARM` en `timer_create`: no.
- **Señales por hilo como Linux**: máscara y pendientes dirigidas en `task_t`, pendientes de proceso y handlers en `process_t`; tid de los hilos del contador de pids. `signalfd` sigue bloqueando su máscara al crearse (no es Linux, pero hay código que lo espera), ahora en el hilo que lo crea. `sigaltstack` sigue siendo del proceso.
- **Salida del grupo** con SIGKILL a los demás hilos; un hilo en una espera no interrumpible sale cuando vuelve de ella.
- **El buzón de IPC sale de `task_t`** (se crea al usarlo) para que `task_t` quepa en el SLAB; `_Static_assert(sizeof(task_t) <= 2048)`.
- Esto no cambia nada de la capa ni de `aurora.h`: `task_t` es opaco para LinuxKPI. Para K8 (L4) sigue pendiente decidir `current`/`signal_pending` de la capa; ahora hay señales por hilo en las que apoyarlo.

### 27-oct-2026 — L4 (K1-DMA, K6 básico, K8: diseño)
Preguntas de diseño en una sola ronda; el autor eligió las recomendaciones (1–9 «A»):
1. **Puente `file_operations` ↔ VFS**: `vfs_ops` gana llamadas opcionales con el `file_descriptor_t*` (`read_fd`/`write_fd` con el buffer de usuario y el offset del fd por puntero, `llseek`, `fasync`); el VFS las prefiere si existen. `fcntl` gana `F_SETOWN`/`F_GETOWN` y `O_ASYNC` en `F_SETFL` (llama a `fasync`); `kill_fasync` manda `SIGIO` al dueño. La capa crea un `struct file` por `open` (el nodo de devtmpfs es por apertura; `dup`/`fork` lo comparten, como en Linux). `-ERESTARTSYS`/`-ERESTARTNOINTR`/`-ERESTARTNOHAND`/`-ERESTART_RESTARTBLOCK` se traducen a `-EINTR` en el puente (Aurora no reinicia syscalls ni tiene `SA_RESTART`).
2. **`mmap` con `vm_operations_struct`**: VMA nuevo `VMA_DEV` con referencia al fichero y `ops` externas (`fault`, `open`, `close`) + `priv`. `remap_pfn_range`/`io_remap_pfn_range`/`vm_insert_page` mapean en el acto; `vm_ops->fault` se llama desde el `#PF`; `fork` copia el VMA y llama a `open` (salvo `VM_DONTCOPY`); `munmap` parcial divide (`open` del trozo nuevo, `close` del que se va); la salida del proceso llama a `close`.
3. **K8**: la tabla hash `task_t* → task_struct` se mantiene (sin campo en `task_t`, decisión de L2) pero **validada por tid**: si el tid de Aurora ya no coincide, la entrada se refresca (o se crea otra si un driver aún tiene referencia). `current->mm`: `mm_struct` por proceso, creado al usarlo y validado igual. `signal_pending()` = pendientes no bloqueadas del hilo y del proceso (`signal_pending_of` de Aurora); `fatal_signal_pending()` = `SIGKILL` pendiente; en kthreads, 0.
4. **`dma_buf` ↔ PRIME de Aurora**: el `dmabuf` de Aurora puede estar respaldado por un GEM o por un proveedor externo (páginas + liberación). `dma_buf_fd()` de la capa crea un fd de Aurora (`PRIME_FD_TO_HANDLE` lo envuelve en un GEM importado); `dma_buf_get()` sobre un fd de PRIME de Aurora da un `struct dma_buf` cuya `sg_table` son las páginas del GEM (mismo fd → mismo `dma_buf`). `dma_resv` mínimo (solo el cerrojo) hasta K7/L5; el fence implícito de Aurora no se conecta.
5. **DMA streaming**: mapeo directo si cabe en la máscara; si no, bounce reservado al momento por debajo de la máscara (`pmm_alloc_pages_below`) y copiado en `map`/`unmap`/`sync_*` según la dirección; sin pool fijo; contadores en debugfs. `dma_alloc_coherent` definitivo: WC, `NO_KERNEL_MAPPING`, `dma_mmap_*`, `dma_get_sgtable`, `dma_pool`, `dma_alloc_pages/noncoherent`.
6. **`vmap()`**: región virtual del kernel nueva en Aurora (entrada de PML4 propia con su PDPT reservado al arrancar), asignador con página de guarda, WB/WC/UC y `shootdown` de TLB al desmapear.
7. **Firmware**: `request_firmware*` de la capa sobre el de Aurora; `make iso` copia a `/lib/firmware` del initrd los ficheros de `kernel/linuxkpi/firmware.list` (con SHA-256, `Link:` de `WHENCE`) si existe el árbol de linux-firmware 20261019; la prueba omite (no falla) el fichero real si falta. Sin `.xz`/`.zst`.
8. **debugfs**: árbol en memoria de la capa montado como sistema de ficheros en `/sys/kernel/debug` con el mismo puente de `file_operations`.
9. **Hito**: `lkpi_test_k6` (suite del kernel) para lo interno; `lkpi_test_dev.kur` con `/dev/lkpi_test` (`miscdevice`), cargado y descargado por `user/musl/apps/lkpi_test` con `finit_module`. `i2c`: núcleo + emulación SMBus + `i2c-algo-bit` propio (sin `/dev/i2c-N`); backlight en `/sys/class/backlight`.

### 27-oct-2026 — L4: resultados y cambios durante la sesión
Detalle, pruebas y commits en `docs/LinuxKPI_Sesion_L4.md`.
- **SIGIO por pid**: `kill_fasync` no guarda el `file_descriptor_t` (puede liberarse con el nodo aún vivo); `F_SETOWN` avisa al fichero por `setfl` y Aurora envía con `vfs_send_sigio(owner, sig)`. El SIGIO sale siempre de un work (puede pedirse desde una IRQ o un temporizador).
- **`llseek(f, off, whence, cur)`**: Aurora pasa el offset actual y guarda el resultado; la capa no ve `file_descriptor_t`.
- **`dma_buf` sin cachés de identidad**: cada `dma_buf_fd()` crea un dmabuf de Aurora (con una referencia al `dma_buf`, soltada cuando Aurora libera el último GEM); cada `dma_buf_get()` de un dmabuf nativo de Aurora crea un envoltorio. Mismo fd de la capa → mismo `dma_buf`; dos `dma_buf_get` del mismo fd nativo → dos `dma_buf` con las mismas páginas.
- **`poll` filtrado en el puente** (`events | POLLERR | POLLHUP`, como Linux); Aurora no filtra en general y no se ha cambiado.
- **`mprotect` de Aurora conserva `PTE_SPECIAL`/`PTE_SHARED_OBJ` y la caché** (fallo previo: un memfd `MAP_SHARED` con `mprotect` perdía su página al salir el proceso).
- **Hito superado**: `/dev/lkpi_test` (driver de Linux en `.kur`) desde `user/musl/apps/lkpi_test`, 101 comprobaciones con `-smp 2` y `-smp 4`; suite del kernel 201/201 (+ `vmap` y K6) con `-smp 2` y `-smp 4`; `drm_test`, `sig_test`, `rseq_test`, `timer_test`, `thread_sig_test`, `clipboard-x11.sh` y `primary-icons.sh` en verde.


### 27-oct-2026 — L5 (K7, K9-drm: diseño)
Preguntas de diseño en una sola ronda; el autor eligió: 1 A, 2 A2, 3 A, 4 B, 5 A, 6 A, 7 A, 8 las cuatro, 9 A. La sección «L5» de arriba y la §2 hablan de la opción B (`drm.ko`); **vale lo decidido el 26-oct (opción A)** y lo de esta entrada. Hallazgo previo: los `.aur` de pantalla no manejan hardware (solo describen el adaptador) y el «scanout» de `kernel/drm` es una ventana del compositor, que es dueño del GOP.
1. **Scanout**: sigue siendo la ventana del compositor. El driver de Linux hace probe, modos, GEM y atomic completos, pero no programa ni pisa el GOP: `bochs` y `simpledrm` escriben en un destino fuera de pantalla y `kernel/drm` copia a la ventana; `virtio-gpu` envía sus transfer/flush al host. Sin cambio de modo real (D2). Que el driver de Linux sea dueño de la pantalla real queda como L5b (no se hace).
2. **Shim = A2**: cáscara propia en `kernel/drm` (nodos, `drm_file`, despacho de ioctl, GEM/PRIME/syncobj/vblank/scanout) y, desde Linux 6.18.59 con parches mínimos, el modelo de objetos KMS y los helpers puros (`drm_atomic*`, `drm_modes`, `drm_edid`, shmem/framebuffer/format/damage/probe helpers). Se mide el cierre de dependencias y un módulo que arrastre demasiado cae a escritura propia (se anota aquí).
3. **Dos backends** de `/dev/dri` detrás de una tabla de operaciones (nativo y Linux), elegidos en ejecución: el nativo por defecto; si un driver de Linux hace probe, toma `card0`/`renderD128`; si falla, sigue el nativo (GOP). `DRM_BACKEND` en el `Makefile` quita el shim.
4. **Respaldo con `.aur`**: si el driver de Linux falla, la capa pide a `aur_bus` que lance el `.aur` omitido por `aur.skip` de ese dispositivo (EDID y modos de siempre).
5. **Multi-CRTC**: el backend Linux es multi-objeto desde el diseño (ids dinámicos, una ventana de scanout por CRTC), validado con `virtio-gpu` `max_outputs=2`. El nativo sigue con 1 CRTC; compositor multi-monitor, Ajustes y D1/D2 de usuario quedan fuera.
6. **Fences**: Aurora gana un `aur_fence` pequeño (sin depender de la capa) y la capa lo envuelve en los dos sentidos con `dma_fence`; `dma_resv` se conecta al fence implícito del dmabuf de Aurora con callbacks en el nodo; `dma_fence_chain` y `dma_fence_array` en la capa.
7. **Empaquetado**: `lkpi_drm.kur` (shim y helpers), `lkpi_virtio.kur` (núcleo virtio) y un `.kur` por driver, aparte de `linuxkpi.kur`. Se prefiere interponer con cabeceras propias; los parches mínimos van en `kernel/linuxkpi/patches/`, aplicados a una copia en `build/`.
8. **Pruebas**: bochs (`-vga std`, `aur.skip=1234:1111`), simpledrm (mismo hardware sin el driver de bochs), virtio-gpu (`aur.skip=1af4:1050`) y virtio-gpu con `max_outputs=2`; con `-smp 2` y `-smp 4`.
9. **Aurora**: se arreglan `poll` (filtrado de `revents`) y `/dev/kmsg` (sin saltar líneas y sin el DEBUG por lectura), en commits aparte.

### 28-oct-2026 — L5: resultados y cambios durante la sesión
Las entradas siguen la numeración de la ronda del 27-oct; lo que cambia respecto a lo diseñado va explícito.
1. **«Compilar `drm.ko` casi entero»** (decisión del autor en tarjeta, 27-oct): `lkpi_drm.kur` compila de Linux 6.18.59 el núcleo DRM/KMS completo (`drm_drv`, `drm_file`, `drm_ioctl`, `drm_gem*`, `drm_prime`, `drm_syncobj`, `drm_atomic*`, `drm_crtc*`, `drm_edid`, `drm_vblank`, `drm_sysfs`, `drm_client` sin fbdev, shmem/framebuffer/damage/probe helpers…). Esto **sustituye a la cáscara propia A2** para el backend Linux: el nativo (`kernel/drm`) sigue con su cáscara y no se toca su ABI. Las cabeceras de interfaz `include/drm` y `include/uapi/drm` se leen del árbol descargado (`build/linuxkpi/ext-inc`, aprobado por el autor); `scripts/lkpi-ext-inc.sh` también enlaza `aperture.h`, `screen_info.h` y `platform_data/simplefb.h`.
2. **Parches versionados** (`kernel/linuxkpi/patches/`, soportados por `kernel/linuxkpi/drv.mk`: la fuente se copia a `build/`, se parchea y se compila con `-I` del directorio original): `drm_drv.c.patch` (una línea: el `driver` del `drm_device` pasa por `lkpi_drm_driver_fixup`) y `drm_atomic_helper.c.patch` (dos líneas: `lkpi_drm_pace` y `lkpi_drm_present`). El resto es capa y `glue.c`.
3. **Scanout (1A)**: sin modeset real. `lkpi_drm_present` (tras `drm_atomic_helper_commit_hw_done`) copia el plano primario del CRTC activo con `drm_gem_fb_vmap` a una ventana de scanout de Aurora por CRTC (`kernel/drm/drm_linux_scanout.c`); `lkpi_drm_pace` simula un vblank a ~16 ms con `msleep`. **Display-shadow**: para un driver de Linux el framebuffer GOP y los BAR de la función PCI de pantalla que lo aloja se sustituyen por copias en RAM (los BAR pequeños se copian con lecturas de 16 bits: los registros `dispi` de bochs lo exigen), de modo que ni `bochs` ni `simpledrm` tocan la pantalla real. Es el análogo de «no pisar el GOP».
4. **Dos backends (3A)**: `drm_backend_claim`/`drm_backend_release` (`kernel/drm/drm_core.c`) hacen que el nativo ceda `/dev/dri` y `/sys/class/drm` mientras el backend Linux tenga algún minor, y los recupere si desaparece. **`DRM_BACKEND`**: `make DRM_BACKEND=native` (o `drm.backend=native` en `cmdline=`) hace que `lkpi.load` omita `lkpi_drm`, `lkpi_bochs`, `lkpi_simpledrm` y `lkpi_virtio_gpu`; por omisión, `auto`. No se elimina el código del shim del `Makefile` (no hacía falta: el nativo es el respaldo de la 4B).
5. **Respaldo (4B)**: cada `pci_driver` de Linux se registra en `aur_bus` como un driver comodín «sombra»; si su probe falla, la capa llama a `aur_bus_launch_skipped` y se lanza el `.aur` omitido por `aur.skip` (observado con un probe forzado a fallar).
6. **Cargador (`lkpi.load=`)**: `kernel/lkpi_boot.c` carga `/lib/modules/linuxkpi/<nombre>.kur` en orden tras arrancar el compositor, solo con la opción (la suite normal no cambia). Ejemplos: `aur.skip=1234:1111 lkpi.load=linuxkpi,lkpi_drm,lkpi_bochs`; `lkpi.load=linuxkpi,lkpi_drm,lkpi_simpledrm`; `aur.skip=1af4:1050 lkpi.load=linuxkpi,lkpi_virtio,lkpi_drm,lkpi_virtio_gpu`.
7. **Cuña de ABI («quirk»)**: los drivers KMS tiny (`bochs`, `simpledrm`) no declaran `DRIVER_RENDER` ni `DRIVER_SYNCOBJ`, y el backend nativo siempre ofreció `renderD128` y syncobj (Mesa, Weston y `drm_test` lo esperan). `lkpi_drm_driver_fixup` los añade sobre una copia `devm_kmemdup` del `drm_driver` (la estructura original es `const`); `lkpi.drm_quirks=0` lo desactiva. Los ioctl dumb **no** valen en `renderD128` en Linux (EACCES); `drm_test` lo acepta (usa `card0` para el GEM importado).
8. **Semántica de syncobj de Linux** (a diferencia del nativo, que acepta lo que usaba `drm_test`): un syncobj sin fence no se exporta como `sync_file` ni se espera sin `WAIT_FOR_SUBMIT` (EINVAL), `HANDLE_TO_FD` solo admite 0 o `EXPORT_SYNC_FILE`, e `IMPORT_SYNC_FILE` entra en un handle **existente**. `drm_test` se ajustó a la ABI de Linux (compatible con las dos); el nativo no se cambia. Pendiente (anotado, no hecho): que el nativo adopte la misma ABI para el import.
9. **PRIME y fences (K7/6A)**: los fds que crea `drm_prime` de Linux (`fd_install(dmabuf->file)`) se convierten (`lkpi_dma_buf_fd_install`) en nodos `dmabuf` de Aurora con las operaciones de `dma_resv`: ioctl `EXPORT/IMPORT_SYNC_FILE`, `poll` y `SCM_RIGHTS` funcionan igual que con el nativo. Hito en `lkpi_test`: un fence de escritura pendiente que el driver pone en el `dma_resv` de un `dma_buf` exportado hace que `poll(POLLIN)` y el `sync_file` exportado estén pendientes hasta que el driver lo señala (108 comprobaciones).
10. **uevents**: `kobject_uevent[_env]` de un dispositivo sale por `NETLINK_KOBJECT_UEVENT` (`nl_uevent_broadcast`, exportado a la capa) en el formato del kernel (`change@/devices/..\0ACTION=..\0DEVPATH=..\0SUBSYSTEM=..\0…\0SEQNUM=n`), con `HOTPLUG=1`/`CONNECTOR=` de `drm_kms_helper_hotplug_event`; escribir `add|change|…` en `uevent` lo reenvía. `DEVNAME` ahora sale de `devnode()` (`dri/card0`); antes Weston recibía `/dev/card0`.
11. **Multi-CRTC (5A)**: `virtio-gpu` con `max_outputs=2` da 2 CRTC, 2 conectores, 2 encoders y 2 ventanas de scanout; `drm_test` pone y apaga el segundo CRTC. El compositor no es multi-monitor (D1 de usuario sigue abierto).
12. **Qué no se hizo / se aplazó**: `kmscube` (no hay Mesa en `precompiled/`); el plano de cursor de `virtio-gpu` no se compone en la ventana de scanout (Weston arrancó y cerró bien sobre `virtio-gpu`; no se comprobó el puntero en pantalla); fbdev desactivado como se pidió; vgaarb y aperture compilan y funcionan con el display-shadow, pero sin arbitraje real entre varias VGA; los uevents de hot-plug físico no se probaron (QEMU no los genera), solo el reenvío por `uevent`.

### 28-oct-2026 — L6 (K9: ttm, planificador de GPU y memoria de dispositivo)
Detalle en `docs/LinuxKPI_Sesion_L6.md`. Decisiones:
- `ttm`, `drm_buddy` y `drm_gpu_scheduler` se compilan sin parches desde Linux 6.18.59 en `lkpi_ttm.kur`.
- VRAM simulada = RAM con `gpu_buddy` (4 MiB: máximo orden de `alloc_pages` 10); GTT con `ttm_range_man`.
- Shrinker real sin LRU ni cgroups, invocado desde `lkpi_alloc_pages` cuando el PMM falla; «swap» en RAM (`get_nr_swap_pages` = mitad de las libres, `shmem_writeout` → `-EBUSY`), sin disco.
- `ttm_pool` sin dma32/NUMA/THP; `mmu_notifier` stub funcional; `hmm` desactivado (`-EOPNOTSUPP`).
- Evicción pipelined: el recurso origen se libera al instante; los jobs capturan sus campos al crearse. Tras `drm_sched_stop` el fence del culpable acaba con `-ECANCELED`.
- El alias WB sigue en la ventana directa (L7/L9).
- Aurora no cambia salvo exportar `pmm_free_pages_count`. D1/D2/D5/D6/D10 no se alteran.

### 28-oct-2026 — L7 (Intel: `i915` completo sobre la capa)
Detalle en `docs/LinuxKPI_Sesion_L7.md`. Decisiones:
- `i915` (298 ficheros de 6.18.59) en `lkpi_i915.kur`; helpers de pantalla (DP/MST/DSC/HDCP/HDMI/SCDC/panel/DSI) en `lkpi_drm_display.kur`. Sin parches salvo `intel_rps.c`.
- Includes transitivos de Linux resueltos con `-include lkpi/gpu_prelude.h`, no editando el driver.
- Desactivados: USERPTR, PXP, GVT, DP_TUNNEL, COMPRESS_ERROR, perf events, HWMON, FBDEV_EMULATION, COMPAT. ACPI en stub; opregion por `ASLS`. Gen2–5 (GMCH GTT) no portado. Sin IOMMU hasta L9.
- Stubs a resolver con hardware: `on_each_cpu`/`stop_machine` locales, métodos ACPI, sin ECAM, `vm_mmap` de GEM legacy.
- Aurora: el cargador de `.kur` asigna dirección a secciones `SHF_ALLOC` vacías (relocaciones contra su símbolo). El resto de Aurora no cambia; D1/D2/D5/D6/D10 no se alteran.
- i915 nunca se autocarga (`lkpi.load=` explícito). `xe` diferido hasta tener hardware Gen12+.
- Validado en QEMU: compilar, enlazar, cargar y descargar (suite 208/208) y toda la regresión. El probe real lo valida el autor.

### 28-oct-2026 — L7: primer probe real de i915 (Haswell HD 4600)
- El display-shadow de L5 dejaba los registros de la GPU (BAR0) en RAM: forcewake, SBI y FDI no podían responder. Nueva opción de arranque `lkpi.shadow=fb` (solo el BAR del framebuffer en sombra; `0` = nada, por defecto = todos los BAR). i915 sigue sin autocargarse.
- La sombra de un BAR grande se limita a 32 MiB (`lkpi_vmalloc` usa `kmalloc(size + 4 KiB)` y el heap no pasa de 64 MiB); si no hay memoria se avisa en vez de callar.
- `clflushopt` solo si CPUID lo anuncia. Detalle en `docs/LinuxKPI_Sesion_L7.md` §8.

### 28-oct-2026 — L7 hito 2: presentacion del compositor por KMS (doble buffer, page-flip, vblank)
Detalle en `docs/LinuxKPI_Sesion_L7.md` §12. Decisiones:
- **Quien presenta:** el compositor del kernel (`aurora-wayland` no toca DRM). No se añade presentacion por ioctls de userland: el compositor es kernel y un cliente DRM del kernel evita copias y ioctls por frame.
- **Cliente `drm_client` de Linux, sin fbdev:** se engancha con un parche minimo en `drm_client_setup()` (`patches/drm_client_setup.c.patch`) y hace el modeset propio con dos buffers dumb XRGB8888. Commit atomico bloqueante por frame (`drm_client_modeset_commit`): con i915 espera al vblank real (flip_done); sin vblank de hardware (bochs, simpledrm, virtio-gpu 2D) el ritmo lo sigue marcando el compositor (16 ms). Se descarta `DRM_IOCTL_MODE_PAGE_FLIP` con evento desde userland y el commit no bloqueante: complican el hilo del compositor sin ganancia hasta que se mida en hardware.
- **Modo:** el del tamaño del compositor (el GOP: 1366x768 en la Portégé). El compositor no cambia de resolucion en caliente; si el conector no tiene ese modo no se hace KMS. Modeset en el primer flip (frame ya pintado, sin pantalla negra).
- **GOP de respaldo:** 3 commits fallidos del primer modeset (que no sean `-EBUSY`) → backend `-ENODEV` y el compositor vuelve a volcar a VRAM. `-EBUSY` (otro maestro DRM de userland): no se intercambia y se reintenta.
- **Coherencia:** en i915 el mapeo del buffer es WB; se hace `clflush` de las filas escritas antes de cada commit (`lkpi.kms_flush`). Buffer age 2: el trasero recibe tambien el daño del frame anterior.
- **Politica:** `lkpi.kms=auto` (por defecto) solo para i915; `1` para cualquier driver (QEMU); `0` apaga y deja el comportamiento del hito 1. i915 sigue sin autocargarse.
- **Medida:** `[COMP-PERF]` añade, en GOP, la exposicion al barrido (porcentaje del periodo de refresco con VRAM en escritura) y, en KMS, flips/fallos/vblanks perdidos/duracion del commit. Diagnostico en la Portégé: `drm.debug=0x14 lkpi.i915_diag=1` y trazas `kms:` (vblank avanza, modo elegido, modeset hecho).
- Aurora cambia: `kernel/drm/drm_linux_scanout.c` (registro del backend), `kernel/gfx/compositor.c` (presentacion y estadisticas), `ksymtab_exports.c`. D1/D2/D5/D6/D10 no se alteran. No se empieza L8 sin confirmacion.
- (Correccion, misma fecha) i915 sin FBDEV_EMULATION no llama a `drm_client_setup()`: parche `i915_driver.c.patch` para que lo haga al final de `i915_driver_register()`. Detectado con el log de la Portégé (sin lineas `kms:`).
- (Correccion, misma fecha) `nth_page()` de la capa pasa de `page + n` a `lkpi_pfn_to_page(pfn + n)`: las `struct page` no son un array por pfn y i915 coalesce paginas contiguas en el sg (GPF en `drm_clflush_sg`, visto en la Portégé). El KMS cierra la ventana «AuroraOS DRM» del commit inicial del driver.
- (Correccion, misma fecha) Primer KMS verificado en la Portégé. El daño del frame anterior se repasa desde `front_shadow` (imagen final presentada), no desde el `backbuffer`: `compose_scene()` repinta fuera del clip y dejaba rayas en el Dock.

### 28-oct-2026 — Estado del plan tras el hito 2 de L7
- **Hecho:** L0–L6 completas; L7 hitos 1 (probe y dibujo) y 2 (presentacion KMS con doble buffer, verificada en la Portégé: vblank real, 0 flips fallidos; rayas del Dock corregidas pendientes de confirmar en hardware).
- **Falta de L7:** hito 3 (3D con Mesa `crocus` + `linux-dmabuf` en `aurora-wayland`); medir y bajar los vblanks perdidos (0-10 por 120 flips: commit no bloqueante con evento); `on_each_cpu`/IPI de funcion, ECAM y vm_mmap de GEM (stubs de §4 de la sesion L7); cursor por plano de hardware; hotplug de conectores y cambio de resolucion en caliente.
- **L9 (en paralelo, "obligatoria antes de L7" segun §8 pero aun sin hacer):** IOMMU/VT-d, `pm_runtime` real, suspend/resume, TDR. Es el riesgo abierto mas serio antes de dar L7 por cerrado.
- **L8:** AMD (sin hardware de pruebas) y NVIDIA (`Plan_NVIDIA.md`, RTX 4060). `Plan_Power.md` (apagado/reinicio ACPI) se ha integrado en esta rama.
- Rama integrada: `claude/power-acpi-apm-plan` mergeada; el resto de ramas claude/ ya estaban contenidas. `main` sigue sin recibir esta rama (a peticion del usuario).

### 29-oct-2026 — L7 hito 3: Mesa `crocus` y `linux-dmabuf` (rama `claude/l7-hito3-crocus-dmabuf`)
- **Auditoria.** El Mesa de Ubuntu 24.04 que ya baja `scripts/fetch-mesa.sh` (25.2.8, `libgallium`) **incluye `crocus`** (y `iris`, `swrast`, `virtio_gpu`...): no hay que compilar Mesa ni fijar otra version; se elige con `MESA_LOADER_DRIVER_OVERRIDE=crocus` (Haswell es Gen7.5: crocus; `iris` empieza en Gen8). `aurora-wayland` **ya tenia** `zwp_linux_dmabuf_v1` v3 (XRGB/ARGB8888 lineales, un plano, LINEAR; el fd se mapea y se vuelca al almacen de la superficie): no se reescribe, se prueba con un cliente EGL/GBM real.
- **Bloqueo resuelto: `vm_mmap`.** El `GEM_MMAP` legacy de i915 (el camino de crocus si `I915_PARAM_MMAP_GTT_VERSION < 4`) hace `vm_mmap(obj->base.filp, ...)` sobre un fichero shmem, y la capa devolvia `-ENODEV`. Ahora: (1) los ficheros shmem de la capa tienen `->mmap` con fallo de pagina sobre su cache de paginas (`mm/shmem.c`); (2) `vm_mmap`/`vm_munmap` (`mm/mmap.c`) llaman a dos funciones nuevas del kernel, `vfs_mmap_node_current`/`vfs_munmap_current` (`vfs.c`, exportadas), que llevan el nodo a `sys_mmap` con un fd temporal que se cierra al volver (el VMA conserva sus referencias). Solo con contexto de proceso de usuario.
- **Limite conocido:** `GEM_MMAP` con `I915_MMAP_WC` sigue fallando con `-ENOMEM` (necesita `find_vma`, que la capa no implementa; cambiar la cache de un VMA ya creado). Mesa pide WC solo por el camino legacy; si hace falta en la Portégé se cambia el parche de i915 para fijar el `pgprot` antes del mmap.
- **Decision: el cliente de diagnostico es EGL/GBM propio, no kmscube.** `tests/wayland/aw_gbm_test.c` (declaraciones locales de GBM/EGL/GLES2, sin `-dev`): `gbm_device` sobre el nodo, EGL plataforma GBM, contexto GLES2 sin superficie, `gbm_bo` XRGB8888 LINEAR+RENDERING importado como EGLImage, barras con scissor y un triangulo con shaders, `glReadPixels` comprueba el color, `gbm_bo_get_fd` (PRIME) y presenta por `linux-dmabuf` en un toplevel de `aurora-wayland`. Cubre justo lo que kmscube comprobaria y ademas el tramo hasta el compositor. Guest: `tests/wayland/guest/gbm.sh [swrast|crocus]`.
- **Decision: nodo del cliente.** swrast reserva con dumb buffers, que Linux solo permite en el nodo primario (`card0`; en `renderD128` dan `EACCES`, como en un Linux real); un driver de GPU usa `renderD128`. El backend nativo de Aurora si los permitia en el nodo render. `gbm.sh` elige el nodo segun el modo.
- **Verificado en QEMU (sin KVM):** GBM→EGL→GLES2 (llvmpipe)→dma-buf→`aurora-wayland` pasa con el backend DRM nativo y con el DRM de Linux (virtio-gpu de `lkpi_virtio_gpu`). Test nuevo en `lkpi_test` (`vm_mmap` de shmem desde un ioctl: el proceso ve lo que escribe el kernel y al reves; 145 comprobaciones). **No verificado:** nada de crocus/i915 (QEMU no tiene GPU Intel): ioctls `I915_GEM_CREATE/MMAP_OFFSET/SET_DOMAIN/EXECBUFFER2/WAIT/CONTEXT_*`, tiling, presentacion de un bo de GPU por PRIME→dma-buf→mapeo de `aurora-wayland` (coherencia de cache del mapeo de un bo de i915 desde otro proceso).
- **Observado y pendiente:** con virtio-gpu de Linux y `-vga none` el compositor no tiene framebuffer y `aurora-wayland` da `win_create fallo` (el cliente termina bien; es un caso de configuracion de QEMU, no de este hito).
- **Para la Portégé:** `WITH_MESA=1 make iso` (o `scripts/fetch-mesa.sh`), `lkpi.load=linuxkpi,lkpi_drm,lkpi_ttm,lkpi_drm_display,lkpi_i915 lkpi.shadow=fb lkpi.i915_diag=1 drm.debug=0x14`, y en el guest `/usr/bin/gbm.sh crocus` (primero `/usr/bin/aw_gbm_test --no-wayland /dev/dri/renderD128` con `MESA_LOADER_DRIVER_OVERRIDE=crocus` para separar GPU de compositor). Lineas utiles: `GL_RENDERER` (esperado `Mesa Intel(R) HD Graphics 4600 (HSW GT2)`), `pixel de la barra verde`, y el kernel con `drm.debug`. `INTEL_DEBUG=bat,submit` y `EGL_LOG_LEVEL=debug` dan mas detalle.
- Aurora cambia: `kernel/vfs.c|h`, `ksymtab_exports.c` (dos exports). No se empieza L8 ni L9 sin confirmacion.
- **Pendiente anotado (29-oct-2026, a peticion de Alvaro): `init` no deberia exportar `LIBGL_ALWAYS_SOFTWARE=1` cuando hay GPU real (driver de Linux con nodo render, p. ej. i915) o cuando lo gestione Linux.** Hoy `user/musl/apps/init/main.c:62` lo fija para todo el sistema y Mesa lo obedece antes que `MESA_LOADER_DRIVER_OVERRIDE`: en la Portégé `eglInitialize` fallaba con `DRI2: failed to get compatible render device`. Mientras tanto, `gbm.sh crocus` lo quita y a mano vale `env -u LIBGL_ALWAYS_SOFTWARE ...`. Cambio sin hacer.
- **Pendiente anotado: `grep` en una tuberia del terminal de Aurora dice «input file is also the output»** (entrada y salida parecen el mismo fichero para `grep`; revisar `fstat` de pipe/tty). Sin investigar.
- **Hallazgos sysfs de este hito en la Portégé (29-oct-2026):** libdrm necesita `/sys/dev/char/<maj:min>`, el enlace `device`, `device/config` (binario) y `<padre>/drm/<nodo>`; con eso Mesa ya identifica `8086:0416` (Haswell GT2) y elige crocus. `sysfs_create_bin_file` de la capa ya publica ficheros de solo lectura.
- **Panic de crocus en la Portégé (29-oct-2026):** `drm_clflush_virt_range` con una direccion baja (~0x42f49) desde `eb_relocate_entry` → `relocate_entry` → `clflush_write32` (execbuffer2). Causa: `__i915_gem_object_get_page()` hacia `sg_page(sg) + offset`, aritmetica de punteros de `struct page` que en Linux vale por el vmemmap pero no en la capa (descriptores sueltos, `lkpi_alloc_pages` solo es contiguo dentro de una asignacion); los sg de shmem se fusionan si los marcos son contiguos, asi que `page + n` apuntaba a basura. Corregido con el parche `kernel/linuxkpi/patches/i915_gem_pages.c.patch` (`nth_page()`, que resuelve por pfn). Sin verificar en la Portégé. El backtrace del panic ahora sigue frames dentro de modulos `.kur` (`kernel/panic.c`).
- **Hito 3 verificado en la Portégé (29-oct-2026):** con el parche `nth_page`, `aw_gbm_test --no-wayland /dev/dri/renderD128` con crocus da `GL_RENDERER=Mesa Intel(R) HD Graphics 4600 (HSW GT2)`, OpenGL ES 3.2, pixel de la barra verde `33c759` y `ALL PASS` (log de Alvaro). Pendiente: la presentacion via `linux-dmabuf` en el compositor con GPU real (sin `--no-wayland`), y que `init` deje de exportar `LIBGL_ALWAYS_SOFTWARE`.
- **Arranque automatico de la pila grafica (29-oct-2026, a peticion de Alvaro: "como Linux", sin parametros):** (1) `kernel/lkpi_boot.c`: sin `lkpi.load=` se recorre el PCI y, si hay una GPU Intel (8086, clase VGA), se cargan solos `linuxkpi,lkpi_drm,lkpi_ttm,lkpi_drm_display,lkpi_i915`; `lkpi.auto=off` lo desactiva, `lkpi.load=` explicito manda y `drm.backend=native` lo impide. (2) `init` ya no fija `LIBGL_ALWAYS_SOFTWARE` si el driver de `card0` (`DRIVER=` en su uevent) es i915/amdgpu/radeon/nouveau: Mesa elige crocus/iris/... por el PCI id. Con bochs/simpledrm/virtio/nativo sigue en llvmpipe. (3) Los `.kur` ya entran en el initrd con `make` (el target `drivers` depende de `linuxkpi`); Mesa sigue siendo opcional (`WITH_MESA=1` en setup-env / `scripts/fetch-mesa.sh`). Bochs, virtio-gpu y simpledrm NO se autocargan (cambiaria el backend por omision de QEMU); AMD/NVIDIA son L8. Sin verificar en la Portégé; en QEMU no hay GPU Intel que dispare la deteccion.
- **Autodeteccion y sombra del framebuffer (29-oct-2026):** en la Portégé con arranque automatico seguia el parpadeo sin la ventana de DRM porque la sombra (`lkpi_display_shadow_probe`) cubria TODOS los BAR de la GPU, registros incluidos, y i915 los necesita reales; antes se evitaba con `lkpi.shadow=fb` a mano (ademas, el `cmdline` de Alvaro tenia `lkpi.load=` corrupto, asi que ese arranque tambien era el automatico). Ahora, sin `lkpi.shadow=`, una GPU Intel usa `fb` (solo el BAR del framebuffer) y el resto sombra completa; la opcion explicita sigue mandando. Tambien: la suite del kernel no carga/descarga la capa si el arranque la cargo por autodeteccion (`lkpi_boot_loaded()`).
- **Hito 3 cerrado en la Portégé (29-oct-2026):** arranque automatico sin parametros funcionando (crocus + KMS sin parpadeo), confirmado por Alvaro. Detalle en `docs/LinuxKPI_Sesion_L7.md` §15. Siguiente: L9 (recomendado antes que NVIDIA) o L8, ambos esperan su confirmacion.
- **Pendientes consolidados tras el hito 3 (29-oct-2026, anotados a peticion de Alvaro):**
  1. Probar en la Portégé la presentacion de un cliente GPU por `linux-dmabuf` en el compositor (`/usr/bin/aw_gbm_test` sin `--no-wayland`, con el compositor en marcha).
  2. Mesa no se descarga con `make`: hay que ejecutar una vez `scripts/fetch-mesa.sh` (o `WITH_MESA=1` en setup-env). Valorar integrarlo en el `env`.
  3. Autocarga de bochs, virtio-gpu y simpledrm (hoy requieren `lkpi.load=`; cambiaria el backend por omision de QEMU): decidir con Alvaro.
  4. `grep` en una tuberia del terminal de Aurora dice "input file is also the output" (sin investigar).
  5. `on_each_cpu`/IPI solo en la CPU local (afecta a `wbinvd` en SMP); sin ECAM.
  6. Restos del hito 2 pendientes de logs: franjas del Dock y perdida de vblank.
  7. L8 (AMD/NVIDIA) y L9 sin empezar; esperan confirmacion (recomendado L9 antes que NVIDIA).
