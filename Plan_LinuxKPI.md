# Plan de implementación: capa de compatibilidad con drivers de Linux (LinuxKPI) para Aurora

Estado: **L0, L1, L2 y L3 hechas** (26-oct-2026; L0.3 repetida sobre Linux 6.18.59 y NVIDIA open 615.78.08, las versiones fijadas, ver §3.1 y §11). L4 pendiente de confirmación. Resúmenes de las sesiones: `docs/LinuxKPI_Sesion_L0_L1.md`, `docs/LinuxKPI_Sesion_L2.md` y `docs/LinuxKPI_Sesion_L3.md`. Decisión D5 (opción a): una sola capa para **Intel (`i915`/`xe`), AMD (`amdgpu`) y NVIDIA (`kernel-open/`)**. Contexto: `docs/Wayland_Estado_y_Decisiones.md` (D5, D6, D10), `Plan_NVIDIA.md`, Bloque 8 de `Plan_Wayland_Unificado.md`.

## 0. Principios

1. **Compatibilidad a nivel de código fuente, no binaria.** Los drivers de Linux se **compilan** contra cabeceras de Aurora que imitan `include/linux/*`, `include/drm/*`, `include/uapi/*` y se enlazan como `.kur`. Es la técnica de FreeBSD (`linuxkpi` + `drm-kmod`) y la de Haiku/illumos en sus puertos DRM.
2. **El driver de Linux se toca lo mínimo** (parches pequeños y mantenibles sobre una versión fija). Todo lo que falta se resuelve en la capa, no editando `i915`/`amdgpu`.
3. **Se prueba con drivers de Linux que funcionan en QEMU antes de tocar GPU reales** (sección 6). Sin eso, cada fallo en hardware real sería indiagnosticable.
4. **GOP como respaldo** en todos los hitos con GPU real, y el arranque nunca depende de que la capa funcione.
5. **Una versión de Linux fijada** (propuesta: la serie LTS más reciente que traiga soporte de Ada/Alder Lake/Haswell, a confirmar en L0) y migraciones deliberadas, nunca «seguir `master`».

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
- **Cuál:** el portátil (probablemente Haswell, `i915` Gen7.5); si hubiese uno con Gen9–12, igual con `i915` y luego `xe` (Gen12+).
- Aquí por primera vez se necesitan: `vga_client`, `acpi` (opregion, `_DSM`, `acpi_video`), `intel_gtt`/GGTT, `i2c` GMBUS, `power wells`, `rc6` desactivado, `GuC/HuC` (firmware) según generación, `drm_display_helper` (DP/HDMI), reset de GPU, DMAR/IOMMU (K5 + L9).
- **Hitos:** (1) `i915` hace probe y muestra la identidad por serie; (2) pantalla nativa con GOP de respaldo; (3) 3D con Mesa `iris`/`crocus` + `aurora-wayland` con `linux-dmabuf`.
- Pruebas: **PC real** con log serie; o **VFIO + KVM** (passthrough de la iGPU) desde Linux en ese PC.

### L8 — AMD (`amdgpu`) y NVIDIA (`kernel-open/`)
- `amdgpu`: tamaño enorme (~5 M de líneas); **solo** las partes que usa la GPU concreta del autor (si la hay) y nada de SVM/KFD/compute. Requiere `ttm`, `gpu-sched`, `drm_buddy`, `ioremap` de BARs grandes (ReBAR), `acpi` y firmware.
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
| VFS: `cdev` genérico + `file_operations` completo (`fault`, `llseek`, `poll`, `fasync`) | L4 |
| `sysfs`: crear atributos desde `kobject`/`device` (`drm/cardN`, `class/drm`, `bus/pci/drivers`) — **hecho en L3** (atributos con `show`/`store` en `sysfs_dyn`; faltan los binarios) | L3 |
| `udev`/`uevent`: eventos `change` (hot-plug de conector) hacia `libudev` | L5 |
| Memoria: `shmem` para objetos GEM, páginas con atributos WC/UC, `vmap`, límites por proceso | L1/L4/L6 |
| IOMMU/VT-d y `dma_map` real | L9 |
| `task_struct` mínimo visible a drivers (`current->mm`, `pid`, `comm`) sobre `task_t` | L4 |

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
8. **Cambios en Aurora tras L2**: ~~plazos de espera a 1 ms~~ y ~~`sched_current()` atómico~~ → hechos (§11, «Versión fijada, `sched_current()`, plazos a 1 ms»). Siguen rechazados `task_t->lkpi_task` para `current` y exportar `signal_pending` (necesarios en K8, L4).
9. **Abiertas tras L3** (§11, «L3»): (a) ~~opción de arranque para que `aur_bus` no reclame los ID que deba llevar un driver de Linux~~ → hecho: `aur.skip=` (§11, «Tras L3»); (b) `on_each_cpu()`/IPI de función (`IPI_VECTOR_CALL` sin handler; L7, `i915`); (c) ECAM para el espacio de configuración extendido (L7/L8); (d) ampliar los 21 vectores MSI de kernel o darles espacio por CPU (L7/L8); (e) ~~`irq_user.c` fuerza INTx activas a nivel bajo en líneas que la MADT de QEMU declara activas a nivel alto~~ → corregido (§11, «Tras L3»); (f) uevents hacia el userland (L5).

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
