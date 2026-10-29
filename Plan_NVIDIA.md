# Plan de implementación: NVIDIA en AuroraOS (decisión D10: obligatorio)

Estado: **PLANIFICADO, nada implementado** (26-oct-2026). Hardware de pruebas: RTX 4060 (Ada, AD107) del sobremesa del autor. Contexto y decisiones: `docs/Wayland_Estado_y_Decisiones.md` (D5, D6, D10) y Bloque 8 de `Plan_Wayland_Unificado.md`.

## 0. Datos verificados del código de NVIDIA

Fuente: `github.com/NVIDIA/open-gpu-kernel-modules` (README, comprobado el 26-oct-2026).

- Cada módulo se divide en **componente independiente del SO** (`src/nvidia`, `src/nvidia-modeset`, `src/common`) y **capa de interfaz con el kernel** (`kernel-open/`, una carpeta por módulo: `nvidia`, `nvidia-modeset`, `nvidia-drm`, `nvidia-uvm`). `nvidia-drm` y `nvidia-uvm` **no** tienen parte independiente del SO: son pura capa Linux.
- Soporta **Turing o posterior**. La RTX 4060 (Ada) entra.
- Requiere el **firmware GSP** y los componentes de usuario **de la misma versión** (hoy 615.78.08; no se admiten versiones mezcladas). Los componentes de usuario se obtienen del paquete `.run` con `--no-kernel-modules`. El firmware GSP va codificado en las fuentes y `nouveau/` trae un script que lo extrae a ficheros.
- El README no dice qué licencia aplica a cada parte (hay `COPYING`). **Hay que leerlo antes de empezar** (tarea T0.1) para confirmar que se puede enlazar sin obligar a GPL en el resto de Aurora.
- Compilación: GCC/Clang modernos, `make modules`; la capa de interfaz se compila contra el kernel destino (aquí: contra la ABI de Aurora, no de Linux).

**Consecuencia de diseño:** lo que se porta es el **núcleo `src/nvidia` + `src/nvidia-modeset`** (código abierto, independiente del SO) y se escribe una capa de SO **nueva** de Aurora en lugar de `kernel-open/`. Esto es lo mismo que hace NVIDIA para FreeBSD/Solaris y evita emular Linux entero (opción D5(a)).

> **Nota (26-oct-2026, D5):** el autor decidió hacer una capa de compatibilidad con drivers de Linux (LinuxKPI) para todas las GPU. Eso abre una alternativa a la «capa de SO propia» de este plan: alojar `kernel-open/` de NVIDIA (la capa Linux) sobre esa LinuxKPI en lugar de reescribirla. Ventaja: una sola capa y menos código nuevo; coste: LinuxKPI debe estar bastante completa antes de N1. Decidir al terminar T0.2 (cuando se conozca la lista exacta de APIs que usa `kernel-open/`). Las fases N0–N6 no cambian.

> **Datos repetidos (26-oct-2026) contra Linux 6.18.59 y NVIDIA open 615.78.08** (la versión fijada, descargada de GitHub con `git`): 1679 símbolos, **992 (59 %) compartidos con Intel/AMD**; lo exclusivo es sobre todo K6 (130), K1 (108), K3 (70), K4 (67) y K5 (61). Detalle en `Plan_LinuxKPI.md` §3.1.
>
> **Datos (26-oct-2026, LinuxKPI L0.3, provisionales sobre NVIDIA open 615.71.09 de Ubuntu):** `kernel-open/{nvidia,nvidia-modeset,nvidia-drm,common}` (90 k líneas) consume **1672 símbolos** de la API de Linux; **953 (57 %)** los necesitan también `i915`/`xe`/`amdgpu`, así que la capa ya los tendrá. Lo exclusivo de NVIDIA son 719 (memoria/NUMA y `get/pin_user_pages` 116, K6 132, K5 75, K4 68, DRM 44…). La alternativa «capa de SO propia» supone reescribir el equivalente a esos 90 k líneas de `kernel-open/`. Detalle: `docs/linuxkpi_api.md` y `Plan_LinuxKPI.md` §3.1. Decisión pendiente del autor.

## 1. Qué reutiliza Aurora (ya existe)

| Necesidad | Existe | Dónde |
|---|---|---|
| Carga de módulos de kernel con imports/exports | sí | `.kur`, `kur_loader.c`, `ksymtab.c` |
| Sondeo PCI genérico, BARs, bus mastering | sí | `pci.c`, `bus.c`, sysfs |
| MSI/MSI-X con pool de vectores | sí (MSI y `AURMSI_ALLOC_N`) | `msi_user.c`, `apic.c` |
| Firmware al estilo Linux | sí | `kernel/firmware.h` (`request_firmware`) |
| DRM/KMS, atomic, dumb, PRIME/dma-buf, syncobj, sync_file | sí (sobre pantalla virtual) | `kernel/drm/` |
| Framebuffer GOP con write-combining | sí | `paging.c` |
| EDID desde UEFI | sí | `display_edid.c`, bootloader |
| Workqueues, kthreads, IRQ con hilo, wait queues | sí | `workqueue.c`, `kthread.c`, `irq_threaded.c` |
| Proxy de GPU para drivers de usuario | sí | `gpu_proxy.c` (`AUR_CAP_GPU`) |
| Capa de compatibilidad Linux (ioctl, mmap, epoll, memfd...) | sí | syscalls Linux |
| Compositor Wayland propio sobre `wl_shm`/dma-buf | sí | `user/aurora-wayland` |

## 2. Qué NO existe y hay que construir (bloqueantes)

1. **IOMMU/VT-d (8.0)**: sin ella el DMA de la GPU puede leer/escribir cualquier RAM. Obligatoria antes de dar la GPU a código de usuario; para el primer driver en Ring 0 es riesgo asumido y documentado.
2. **Mapeo de BARs grandes (>4 GiB, 64 bits, Resizable BAR)**: la RTX 4060 tiene BAR1 grande; `sysfs` ya guarda 64 bits (`bar_phys`) pero hay que probar rangos enormes y ventanas del puente.
3. **Memoria física contigua y *scatter-gather* para DMA** con límites de dirección (la GPU direcciona 47–52 bits) y mapeos coherentes/no cacheables.
4. **MSI-X completo en Ring 0** con enrutado a varias CPU (hoy los `.aur` van al BSP).
5. **Gestión de energía mínima**: D0/D3, reset de función (FLR), *PCIe link state*, ASPM apagado al principio.
6. **`dma_fence`/`ttm`-like** solo si se elige la vía `nouveau` (fase 5b).

## 3. Arquitectura propuesta

```
 userspace
   Wayland (aurora-wayland)  ── EGL/GBM/Vulkan (libs NVIDIA, glibc) ──┐
                                                                      │ ioctl /dev/nvidiactl, /dev/nvidia0,
 ───────────────────────────────────────────────────────────────────  │ /dev/dri/card1 (nvidia-drm)
 kernel Aurora                                                        ▼
   nv-aurora-drm.kur   (reescritura de kernel-open/nvidia-drm sobre kernel/drm)
   nv-aurora-core.kur  = src/nvidia (RM)  +  capa de SO "nv-os-aurora.c"
   nv-aurora-modeset.kur = src/nvidia-modeset + puente a DRM/KMS
   [GSP firmware cargado con request_firmware → GSP-RM en la GPU]
   pci.c · msi · dma · mmio · firmware · workqueue · kthread · timers
```

- **Capa de SO (`nv-os-aurora`)**: implementa la interfaz `os_*` que declara `src/nvidia` (asignación de memoria con atributos, `os_acquire_spinlock`/mutex/semáforos, temporizadores, `os_delay`, acceso PCI config, `os_map_kernel_space` para MMIO, `os_alloc_pages_node`, mapeo DMA, registro de IRQ/MSI-X, lectura de registro/`os_log`, cadenas de entorno/`registry`, `os_get_current_process`, etc.). La lista exacta se extrae de las cabeceras `src/nvidia/arch/nvalloc/unix/include/os-interface.h` y `nv.h` (tarea T0.2).
- **Módulos `.kur` separados** reflejan la división de NVIDIA y permiten recargar `modeset`/`drm` sin tocar el núcleo.
- **Cuarto módulo** (`nvidia-uvm`, memoria unificada para CUDA) **fuera de alcance** hasta que el resto funcione.

## 4. Fases e hitos

Cada fase termina en algo verificable **en el PC real** (salvo las marcadas «sin hardware»). Regla de oro: **GOP sigue siendo la pantalla de respaldo**; nunca se apaga el framebuffer del firmware hasta que el nuevo modo esté confirmado, y hay un temporizador que restaura el estado previo.

### Fase N0 — Preparación (sin hardware)
- **T0.1** Leer `COPYING` y las cabeceras de licencia de `src/nvidia`, `src/nvidia-modeset` y del firmware; decidir política (ver D5: licencia GPL). *Salida:* nota de licencias en este fichero.
- **T0.2** Clonar la versión elegida (fijar **una** versión: la que corresponda al firmware y a los componentes de usuario) y extraer la lista de símbolos `os_*` y callbacks que exige el núcleo. *Salida:* `docs/nv_os_api.md` con cada función, su semántica y su equivalente en Aurora.
- **T0.3** Preparar `scripts/fetch-nvidia.sh` (como `fetch-mesa.sh`: el código de NVIDIA, el firmware y los componentes de usuario **no se versionan**).
- **T0.4** Entorno de compilación: construir `src/nvidia` como objeto reubicable con las flags del kernel de Aurora (`-mcmodel=kernel`, sin red zone) y comprobar que el cargador `.kur` soporta todos los tipos de reubicación que produce (TLS, `R_X86_64_GOTPCREL*`, secciones `.init_array`, `.rodata.str`…). Ampliar `kur_loader.c` si hace falta. *Es probable que el tamaño (>20 MB de objeto) obligue a revisar límites del cargador.*
- **T0.5** Prueba de «humo» sin hardware: cargar el `.kur` con la capa de SO con funciones `os_*` simuladas y llamar a una función pura del RM (p. ej. análisis de la tabla de PCI IDs) para validar cargador y ABI.

### Fase N1 — Identidad de la GPU (hito 1)
- Driver `.kur` mínimo `nv-probe` que se enlaza por PCI (`10de:*` clase 0x03), activa MEM + bus master, mapea BAR0 (registros) y lee `NV_PMC_BOOT_0`/`BOOT_42` para identificar arquitectura/implementación.
- Vuelca por serie: ID PCI, tamaños de BAR, capability PCIe (ancho/velocidad del enlace), ReBAR, MSI-X, y la **tabla EDID de cada salida** por el firmware si es accesible.
- **Criterio:** el log serie de la RTX 4060 muestra `AD107` y los BARs correctos, sin colgar ni alterar la pantalla.
- *Riesgo bajo:* solo lecturas de MMIO.

### Fase N2 — Capa de SO y arranque del RM (hito 2)
- **N2.1** Implementar `nv-os-aurora`: memoria, sincronización, temporizadores, PCI, MMIO, log, registro de claves (`registry`) con valores por defecto seguros (sin reclocking agresivo, sin gestión de energía dinámica).
- **N2.2** DMA: asignador de páginas con máscaras de dirección y atributo (WB/WC/UC), *scatter-gather*, `os_dma_map/unmap`, coherencia. Pruebas de integridad con patrones (como `dma_user`).
- **N2.3** Interrupciones: MSI-X con vectores del pool de kernel, bottom halves en workqueue/hilo IRQ (RM exige contexto de hilo para parte del trabajo).
- **N2.4** Firmware: `request_firmware()` de `gsp.bin` + `bootloader` + firmware de arranque (según versión) desde el initrd (`/lib/firmware/nvidia/<ver>/`); verificación de tamaño/firma.
- **N2.5** Arranque del GSP: llamar a `RmInitAdapter`, esperar a `GSP_INIT_DONE`, consultar `NV2080_CTRL_CMD_*` básicos (arquitectura, memoria de vídeo, número de pantallas).
- **Criterio:** `nvidia-smi`-equivalente propio (`nvq`) imprime nombre, VRAM (8 GiB) y número de salidas; el sistema sigue estable 30 min.
- *Riesgo medio:* tras el arranque del RM la GPU puede alterar el estado de pantalla; probar primero con **segunda GPU o iGPU como salida**, o por serie/SSH-like, con el equipo accesible.

### Fase N3 — Modeset nativo (hito 3; desbloquea D1/D2)
- **N3.1** Portar `src/nvidia-modeset` con su capa propia («NVKMS»): arranque, enumeración de *heads*/*connectors*, EDID por DP/HDMI, cambio de modo, planos, cursor por hardware, DPMS.
- **N3.2** Puente a `kernel/drm`: exponer un segundo `card` real con conector/CRTC/plano reales (hoy `drm_core.c` asume **un** CRTC virtual; hay que generalizarlo: aquí se paga la deuda de **multi-CRTC**, D1).
- **N3.3** Hot-plug de monitor (interrupciones HPD) → uevent → `aurora-wayland` recalcula `wl_output` (D1).
- **N3.4** Cambio de resolución en caliente (D2): refactor del compositor (`fb_width/fb_height` globales) + selector en Ajustes.
- **N3.5** Conmutación segura GOP → modo nativo → vuelta al GOP si falla (temporizador de confirmación de 10 s).
- **Criterio:** la pantalla de la RTX 4060 se muestra a su resolución nativa y a 60 Hz; se puede cambiar de resolución y enchufar/desenchufar un monitor con el compositor vivo.

### Fase N4 — Memoria de la GPU y canales (hito 4)
- Gestión de VRAM y GART/PDE (en Ada la hace el GSP, pero el kernel mapea BAR1 y asigna memoria de sistema), `dma-buf` de VRAM, *channels* y *fences*.
- ABI `/dev/nvidiactl`, `/dev/nvidia0`, `/dev/nvidia-modeset`: ioctls `NV_ESC_*` y `NV_IOCTL_*` que usan las bibliotecas de usuario de NVIDIA. Se implementan en la capa Linux ya existente (`ioctl`, `mmap` con `vm_ops`, `poll`/`epoll` para eventos).
- **Criterio:** un programa de usuario enlazado con `libnvidia-*` crea un contexto RM y asigna memoria (`nvidia-smi` real de NVIDIA, con el paquete `.run --no-kernel-modules`).

### Fase N5 — 3D y presentación (hito 5)
- **5a — Pila propietaria de NVIDIA (recomendada primero):** `libnvidia-glcore`, `libEGL_nvidia`, `libnvidia-vulkan-producer`, `libnvidia-egl-wayland`/`libnvidia-egl-gbm`; `nvidia-drm` propio con GBM, `linux-dmabuf` real hacia `aurora-wayland`, `drm_syncobj` explícito (NVIDIA exige *explicit sync*: ya existe en `kernel/drm`). El compositor compone con GL en lugar de `wl_shm`.
- **5b — Vía abierta (opcional, después):** `nouveau` + NVK (Mesa, GPL) sobre una capa tipo LinuxKPI (D5(a)). Solo si se quiere una pila totalmente abierta; es mucho más trabajo.
- **Criterio:** `glxgears`/`vkcube` vía Wayland a 60 FPS dentro de una ventana de Aurora; `gtk3-widget-factory` fluido.

### Fase N6 — Robustez y seguridad
- IOMMU/VT-d (8.0) con dominio por contexto; reset de función (FLR) y recuperación ante *Xid*; gestión de energía (D3, reloj dinámico); `suspend/resume`; *watchdog* del GSP; fuzzing de los ioctls `NV_ESC_*`.

## 5. Plan de pruebas

| Fase | Dónde | Cómo |
|---|---|---|
| N0 | contenedor/QEMU | compilación, carga del `.kur`, pruebas con `os_*` simulados |
| N1–N6 | **PC real** (RTX 4060) | ISO por USB; **log serie obligatorio** (la placa necesita puerto serie o un adaptador USB-TTL); `lspci -nnk` desde otro SO; *screendump* con cámara si no hay captura |
| Regresión | QEMU | la suite de kernel (183 pruebas) no debe empeorar; `bochs.aur`/`virtio_gpu.aur` siguen funcionando |

Para no depender de un solo equipo: **passthrough VFIO** de la RTX 4060 a QEMU/KVM desde Linux en el mismo PC permite iterar sin reiniciar (y con KVM, mucho más rápido que TCG). Requiere IOMMU activada en el host. Es la forma recomendada de desarrollar N1–N3 con comodidad, aunque la validación final es en hardware directo.

## 6. Riesgos y mitigaciones

| Riesgo | Mitigación |
|---|---|
| Pantalla en negro/equipo sin imagen | GOP intacto hasta confirmar; temporizador de reversión; serie siempre activa; ruta de «arranque seguro» (`nvidia=off` en la línea de comandos) |
| Licencias (MIT/GPL del núcleo, firmware redistribuible) | T0.1 antes de escribir código; módulos separados; scripts de descarga sin versionar binarios |
| Versión de firmware/usuario ≠ núcleo | fijar **una** versión; `fetch-nvidia.sh` descarga el conjunto coherente |
| DMA sin IOMMU | Fase N6 + advertencia visible; no cargar el módulo en equipos no confiables |
| Tamaño y complejidad del RM (~1 M líneas) | portar el núcleo **sin modificarlo**; todo lo específico va en `nv-os-aurora`; seguir la versión del *upstream* con un parche mínimo |
| Cargador `.kur` insuficiente | T0.4/T0.5 lo descubren pronto |
| Sin hardware en el entorno de desarrollo | VFIO + KVM en el PC del autor; hitos con criterios observables por serie |
| `drm_core.c` con un solo CRTC | generalizarlo en N3.2 (también sirve para D1) |

## 7. Orden global y dependencias

`N0 → N1 → N2 → N3 → N4 → N5(a)`; `IOMMU (8.0)` antes de N6 y recomendable antes de N4; `5b` opcional al final. D1/D2 (multi-monitor, resolución) se cumplen en N3.

## 8. Lo que se puede hacer ya, sin hardware (propuesta de siguiente paso)

T0.1–T0.5 y la generalización de `drm_core.c` a varios CRTC con pruebas en QEMU (conectores virtuales adicionales), que es trabajo previo útil y verificable.

## 9. Preguntas abiertas para el autor

1. Confirmar la RTX 4060 como equipo de pruebas y si tiene **puerto serie** o adaptador USB-TTL.
2. ¿Hay otro Linux en ese PC para usar **VFIO + KVM**? (acelera muchísimo el desarrollo).
3. Licencia: ¿se acepta que los módulos NVIDIA de Aurora queden bajo MIT/GPL según indique T0.1?
4. Vía 3D: ¿5a (pila propietaria de NVIDIA, antes) o 5b (abierta, después)? Recomendado: 5a.

## 10. Registro de decisiones y resultados

### 29-oct-2026 — N0: T0.1 (licencias), T0.2 (interfaz `os_*`), T0.3 (`fetch-nvidia.sh`)

Rama `claude/project-thread-tuxtci`. Versión fijada: NVIDIA open **615.78.08** (`kernel/linuxkpi/linux-version.mk`); fuentes en `build/linuxkpi_src/nvidia-open` (clon de la etiqueta; `scripts/fetch-linux-drivers.sh` ya lo hacía).

**T0.1 — Licencias (comprobado sobre 615.78.08).**
- `COPYING`: «salvo indicación contraria, los ficheros son **MIT**». SPDX: 2933 ficheros `MIT`; 1 `Apache-2.0 WITH LLVM-exception` (`src/nvidia/inc/libraries/nvport/clang/stdatomic.h`), 1 `Linux-OpenIB` (`kernel-open/nvidia-peermem/peer_mem.h`, fuera de alcance). 1012 `.c/.h` sin SPDX (cabeceras antiguas; revisar al incorporarlos, no bloquea).
- `src/nvidia`, `src/nvidia-modeset` y `src/common` son MIT. `kernel-open/*` declara `MODULE_LICENSE("Dual MIT/GPL")`. Los `g_bindata_*.c` (microcódigo de arranque del GSP, 64 ficheros, 47 MB) también llevan cabecera MIT.
- **Conclusión:** el núcleo abierto puede enlazarse con código GPL-2.0 (la capa LinuxKPI) y con el resto de Aurora sin obligar a GPL en este último; MIT es compatible en ambos sentidos. Se mantiene la política «fuentes de NVIDIA no versionadas, solo scripts de descarga». Decisión formal sobre la licencia de los módulos Aurora: **pregunta 3 de §9, sigue abierta** (recomendación: la capa `nv-os-aurora` propia, MIT; los módulos que alojen `kernel-open/` sobre LinuxKPI, GPL-2.0 como el resto de la capa).
- **Corrección a la nota de L8 de `Plan_LinuxKPI.md`:** `src/nvidia` **no** viene como objeto binario: se compila desde fuente (aquí con las flags de Aurora). El **firmware GSP** (`gsp_ga10x.bin`, que usa Ada/AD107) **no** está en las fuentes: viene en el paquete `.run` (`firmware/`), igual que los componentes de usuario. Lo que sí hay codificado en `src/nvidia/generated/g_bindata_*.c` es el microcódigo de arranque (Booter, bootloader del GSP, scrubber).

**T0.2 — Interfaz de SO.** `scripts/nvidia-os-api.py` genera `docs/nv_os_api.md`: **165** funciones `os_*` (`os-interface.h`), **180** `nv_*` que el RM invoca en la capa de SO, **97** `rm_*` que exporta el RM (no hay que escribirlas) y **53** `nvkms_*` de nvidia-modeset, cada una con su número de usos y el fichero de `kernel-open/` que la implementa hoy. Solo 11 `os_*` carecen de usos: son stubs triviales. Este es el tamaño real de la «capa de SO propia» (≈ las 165 + 180 + 53 funciones, más `nvidia-drm`); la alternativa LinuxKPI sigue siendo reutilizar esas implementaciones. **La decisión (D5/pregunta 4 de `Plan_LinuxKPI.md` §10) no se toma aquí**: sigue pendiente del autor; recomendación: LinuxKPI (`kernel-open/`), porque el 57 % de los símbolos ya existe y la capa propia obliga a reescribir ~90 k líneas.

**T0.3 — `scripts/fetch-nvidia.sh`.** Descarga `NVIDIA-Linux-x86_64-615.78.08.run` (existe en `download.nvidia.com`, comprobado con HEAD) a `build/nvidia/` y extrae `firmware/` y las bibliotecas de usuario **sin ejecutar el `.run`** (lee la cabecera makeself, `tail | xz | tar`). Comprueba que las fuentes de `nvidia-open` son de la misma versión. **Ejecutado el 29-oct-2026** (506 MB, sha256 `3e3f6ddd…07ffe`): la carga del `.run` 615.78.08 es **zstd** (no xz; el script detecta ambos por la firma y necesita `zstd`). Resultado: `firmware/gsp_ga10x.bin` (27 MB, el de la RTX 4060), `gsp_tu10x.bin`, `ucodes_ga10x.bin`, `ucodes_tu10x.bin` y 55 bibliotecas de usuario.

**Siguiente (T0.4/T0.5):** compilar `src/nvidia` con las flags del kernel de Aurora y comprobar el cargador `.kur` (tamaño y reubicaciones); depende de la decisión LinuxKPI-vs-capa propia solo en lo que se enlaza, no en el objeto de `src/nvidia`.

### 29-oct-2026 — Decisiones del autor

1. **LinuxKPI** (`kernel-open/` sobre la capa) en lugar de capa de SO propia. 2. **Licencia:** GPL-2.0 para lo que corra sobre LinuxKPI. 3. Hay Linux en el sobremesa (VFIO+KVM posible) y el **log persistente en el pendrive de arranque** (como con `i915`) sirve de registro sin puerto serie (`docs/Log_Persistente.md`).

### 29-oct-2026 — T0.4: `nv-kernel.o` (src/nvidia) compilado con su propio Makefile

`make -C src/nvidia -j4` (gcc 13.3, host) termina sin errores en pocos minutos y produce `src/nvidia/_out/Linux_x86_64/nv-kernel.o`: **20,1 MB** (text 14,4 MB, data 68 KB, bss 33 KB), sin `.tdata/.tbss` ni `.init_array`.
- **Reubicaciones:** solo `R_X86_64_PLT32` (76 914), `32S` (61 780), `64` (27 040) y `PC32` (7 978). `kur_loader.c` ya soporta las cuatro; sin GOTPCREL ni TLS. Tamaño < `KUR_MAX_SIZE` (64 MiB): el límite del cargador basta (queda 3× de margen, también para `nv-modeset-kernel.o`).
- **Símbolos sin resolver (425):** 165 `os_*`, 122 `nv_*`, 71 `libspdm_*` (criptografía: la implementa `kernel-open/nvidia/libspdm_*.c` sobre la API de crypto de Linux; con LinuxKPI hará falta `crypto_shash`/`crypto_aead`/`akcipher` mínimos, o desactivar la atestación SPDM, que una GPU de consumo no usa), 35 `nvswitch_*` y 17 `nvlink_*` (NVSwitch/NVLink: stubs, no aplican a la 4060), `nvidia_caps_root`, `out_string` y 13 `__x86_indirect_thunk_*`.
- **Retpolines:** el Makefile de NVIDIA compila con `-mindirect-branch=thunk-extern` si el compilador lo admite. Aurora no tiene `__x86_indirect_thunk_{rax..r15}`: hay que añadirlos al kernel (13 símbolos, `jmp *%reg` con barrera) o compilar con `-mindirect-branch=keep`. Recomendado: añadirlos a LinuxKPI (también los pedirán `i915`/`amdgpu` en cuanto se compilen con retpoline).
- El objeto se compila con las flags de NVIDIA (kernel-module-like, `-msoft-float`, sin red zone); falta fijar si hace falta `-mcmodel=kernel` (las `32S` indican que sí, como el resto de `.kur`).

**Siguiente (T0.5):** `.kur` de humo con `os_*` simulados que enlace `nv-kernel.o` (con stubs para `libspdm/nvswitch/nvlink` y los thunks) y llame a una función pura del RM; valida cargador y ABI en QEMU.
