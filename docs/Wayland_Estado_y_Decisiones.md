# Wayland, X11 y GPU en AuroraOS — estado, cómo probarlo y decisiones abiertas

Este documento resume **qué hay hecho y verificado**, **cómo reproducirlo**, **qué no se ha podido comprobar** y las **decisiones que siguen abiertas** (con opciones y una recomendación, pero sin cerrar). El plan por bloques está en `Plan_Wayland_Unificado.md` (es el cuaderno de bitácora y tiene el detalle fino); este documento es la vista de conjunto.

Última actualización: sesión del 26-oct-2026. Rama de trabajo: `claude/gracious-lamport-7eumou`.

---

## 1. Qué es el producto

Las apps gráficas de Linux (Wayland y X11) se ejecutan **como ventanas normales del compositor de Aurora** (barra de título, sombras, Dock, menú, foco). No hay un escritorio aparte.

- **`aurora-wayland`** (`user/aurora-wayland/`): servidor Wayland propio, proceso de usuario con la `libwayland-server` de Ubuntu sin tocar. Cada `xdg_toplevel` es una ventana de `winsrv`.
- **Xwayland** (Ubuntu, sin tocar) en modo rootless con `-wm`; `aurora-wayland` hace de gestor de ventanas X11 (`xwm`) y de puente de portapapeles.
- **Kernel**: capa de compatibilidad Linux (AF_UNIX, memfd, evdev, DRM/KMS, PRIME, syncobj, señales, rseq, pty…).
- Weston, `modetest`, `kmscube`… son **solo herramientas de validación** del ABI Linux.

## 2. Cómo reproducir las pruebas

Todo se probó en **QEMU sin KVM (TCG)**: es lento (un arranque + autotests ≈ 1–3 min de reloj; una prueba con varias apps, 5–15 min). El entorno de pruebas **no tiene GPU ni PC real**.

Paquetes del host (Ubuntu 24.04): `qemu-system-x86 ovmf xorriso mtools dosfstools xxd rsync musl-tools clang lld libxcb1-dev libxcb-composite0-dev libxcb-xfixes0-dev libpng-dev xclip xsel imagemagick python3-pil`.

```
make iso MUSL_CC=/usr/bin/musl-gcc KUR_CC=gcc        # ISO (sin x86_64-elf-gcc)
bash tests/wayland/build.sh                            # tests, guiones guest y aurora-wayland
scripts/qemu-drive.py -t 590 -m 6G \
    --boot-test "/usr/bin/<test o guion>" \
    --step "MARCADOR=shot /tmp/x.ppm;sleep 2"          # capturas / clics / teclas por el monitor
```

- **`-m 6G` es obligatorio** con el initrd actual (≈500 MB; con 3 GB el cargador EFI falla con `OUT_OF_RESOURCES`).
- Si la prueba pasa de ~10 min, lanzar en segundo plano y subir `-t`.
- Un solo QEMU a la vez (comparten `/tmp/boot-test.log` y `/tmp/qmon.sock`).
- Tras tocar una struct de un `.h` del kernel hay que borrar los `.o` (`find kernel -name '*.o' -not -name '*blob*' -delete`): el Makefile no rastrea cabeceras.
- `make iso` regenera `build/mbr_part.hex` (artefacto, no se commitea).
- Dependencias de datos: `scripts/fetch-wayland.sh`, `fetch-weston.sh`, `fetch-xwayland.sh`, `fetch-fonts.sh`, `fetch-mesa.sh` (Mesa/LLVM no están versionados).

Pruebas automáticas relevantes (todas **ALL PASS** al cierre de esta sesión; suite del kernel: 183 pruebas, 0 fallos):

| Prueba | Qué cubre |
|---|---|
| `drm_test` | KMS, atomic, PRIME, GEM_FLINK/OPEN, fences de dma-buf, drm_syncobj/sync_file |
| `sig_test` | sigpending, sigtimedwait, sigsuspend, pause, SIGPIPE, WIFSIGNALED |
| `rseq_test` | registro, cpu_id, aborto de sección crítica |
| `sockfile_test`, `unix_test` | inodo `S_IFSOCK`, AF_UNIX |
| `jobctl_test` | job control / pty |
| `wl_test`, `shm_test`, `dev_test`, `timerfd_test` | base Wayland / ABI |
| `tests/wayland/guest/x11-demo.sh` | xeyes + xclock + xterm como ventanas, teclado y ratón X11 |
| `tests/wayland/guest/clipboard-x11.sh` | portapapeles X11↔Wayland |

Capturas en `build/capturas/` (`x11_varias_apps.png`, `x11_teclado.png`, `iconos_apps_x11.png`, `aw_*.png`, `dmabuf-prime.png`…).

## 3. Qué está hecho (resumen)

### 3.1 Servidor Wayland y escritorio
Hecho y validado antes y durante esta sesión: `wl_compositor/subcompositor/shm/output/seat`, `xdg_shell` (toplevel, popup, move/resize, maximizar/minimizar/pantalla completa), decoraciones del lado servidor + política para clientes CSD, subsuperficies con composición y alfa, `wp_viewporter`, `cursor-shape` (formas con el estilo de Aurora), selección primaria, `linux-dmabuf`, Dock con lanzadores, GTK3/Qt5 con tema Aurora, Mesa llvmpipe sobre `wl_shm`.

### 3.2 X11 (Xwayland) — esta sesión
- **Funciona**: `xeyes`, `xclock` y `xterm` salen a la vez como ventanas de Aurora; teclado y ratón verificados (se escribe en `xterm` y se ejecuta).
- Causa raíz del «no sale ninguna ventana»: **SIGUSR1 se ignoraba por defecto en el kernel** (Xwayland la usa para avisar de que está listo) + una **carrera** entre `WL_SURFACE_ID` (socket X11) y la creación de la `wl_surface` (socket Wayland), ya corregida (`xwin_bind_pending`).
- **Portapapeles X11↔Wayland (texto, solo `CLIPBOARD`)**: con Xwayland en modo `-wm` el puente lo hace el gestor de ventanas, **no** Xwayland. `aurora-wayland` lo implementa con una ventana propia + XFixes + `SelectionRequest`/`ConvertSelection`. Probado en ambas direcciones.
- **Iconos**: `user/aurora-wayland/icons.c` resuelve el icono por `Icon=`/`app_id`/ejecutable en los temas (PNG con libpng; SVG con librsvg+cairo cargadas bajo demanda) y por `_NET_WM_ICON`, y lo convierte a BMP con alfa en `/tmp/aurora-icons`. La ventana X11 usa la instancia de `WM_CLASS` si tiene lanzador, así cuelga de su icono del Dock. Iconos de tema propios (placeholders) para `xclock`, `xeyes`, `hello-gtk`, `gtk3-icon-browser`, `qt5-*` en `scripts/gen-dock-icons.py`.

### 3.3 Kernel (compatibilidad Linux) — esta sesión
| Área | Hecho |
|---|---|
| Señales | `rt_sigpending`, `rt_sigtimedwait`, `rt_sigsuspend`, `pause`; **SIGPIPE** en pipes y AF_UNIX (respeta `MSG_NOSIGNAL`); acción por defecto de SIGUSR1/2, SIGVTALRM, SIGPROF, SIGIO, SIGPWR, SIGSYS, SIGXCPU, SIGXFSZ; `wait4` informa `WIFSIGNALED/WTERMSIG` |
| rseq | `rseq(2)` real por hilo: `cpu_id`, aborto de la sección crítica al conmutar o entregar señal |
| AF_UNIX | `bind()` crea un inodo `S_IFSOCK` (stat/`test -S`/`ls`; persiste al cerrar; `unlink` lo quita; `connect` lo exige; `open()` → ENXIO) |
| pty / tty | el líder de sesión adquiere el terminal de control al abrir el esclavo sin `O_NOCTTY` (antes `bash` en `xterm` se paraba con SIGTTIN); eco de Enter con `ICRNL` |
| DRM | `drm_syncobj` (binario y timeline) + `sync_file`; `GEM_FLINK/OPEN`; fences implícitos de dma-buf (`EXPORT/IMPORT_SYNC_FILE`); cargador de firmware `request_firmware` |
| Pantalla | EDID desde UEFI (bootloader → `kernel_boot_info`), modo nativo, tamaño físico, **modos del EDID** como modos DRM |

## 4. Lo que NO se ha podido comprobar (leer antes de fiarse)

1. **EDID desde UEFI**: OVMF/QEMU no publica el protocolo EDID. Solo está probado el analizador (datos sintéticos) y la rama «sin EDID». En un PC real hay que mirar el log (`[BOOT] EDID:` y `[EDID]`). El camino EDID por DDC/VBE en BIOS **no existe**.
2. **Preempción y rseq**: el aborto se prueba con señal; el camino por conmutación comparte código pero no tiene prueba propia.
3. **Estabilidad de Xwayland**: la carrera de `WL_SURFACE_ID` se vio resuelta en varias ejecuciones; antes del arreglo había ejecuciones sin ventanas. Conviene repetir en otra máquina.
4. **`virtio-gpu` 3D (virgl/venus)**: el QEMU del entorno no trae `virtio-gpu-gl`; **no se ha escrito ni probado**.
5. **Hardware real**: nada de GPU/pantalla se ha probado en un PC. Los cambios de kernel de esta sesión (señales, tty, rseq, scheduler) conviene verificarlos también en hardware.
6. **Intermitencia del guest** (~5/45 ejecuciones TCG: el shell deja de avanzar tras `sleep`/`echo` a `/dev/kmsg`; kernel vivo). Sin causa confirmada; se vio otra vez como un bloqueo de ~1000 s en una ejecución.
7. Subsuperficies en modo `sync` y GTK4/libadwaita: sin comprobación visual.

## 5. Decisiones tomadas (no reabrir sin motivo)

| Decisión | Detalle |
|---|---|
| Cursor | El **estilo** del cursor lo pone Aurora siempre; las apps solo pueden pedir **formas con nombre** (`wp_cursor_shape_v1`). Por tanto **no** se muestran cursores con mapa de bits del cliente (`wl_pointer.set_cursor` con superficie). |
| Integración | Cada toplevel Wayland/X11 = una ventana de `winsrv`. La ventana «AuroraOS DRM» (scanout) queda como modo de validación. |
| Render | Por software (llvmpipe/pixman) primero; aceleración al final. |
| Código a ciegas | **No** se escribe programación de pantalla de Intel/AMD/NVIDIA sin hardware para probarla (riesgo de pantallas en negro). |
| libseat | Sin trabajo pendiente: Weston corrió con el backend *builtin*. Un `seatd` solo haría falta para compositores sin ser root. |
| PC real sin driver | En un PC UEFI Aurora ya arranca con el framebuffer GOP (mapeado write-combining); el driver de GPU solo añade modos/aceleración. |

## 6. Decisiones: estado y detalle

| | Tema | Estado (26-oct-2026) |
|---|---|---|
| D1 | Multi-CRTC / hot-plug / multi-monitor | **Decidido: SÍ, hay que hacerlo, pero cuando se pueda** (aplazado: depende de un driver de pantalla real, D5/D6) |
| D2 | Cambio de resolución en caliente | **Decidido: SÍ, hay que hacerlo, pero cuando se pueda** (aplazado: depende de un driver de pantalla real, D5/D6) |
| D3 | Portapapeles X11: alcance | **Hecho `PRIMARY`** (27-oct-2026); `INCR`/imágenes siguen fuera |
| D4 | `virtio-gpu` 3D (virgl/venus) | **Decidido: SÍ, no crítico; pendiente** |
| D5 | Intel/AMD/NVIDIA reales | **Decidido: capa de compatibilidad con drivers de Linux (LinuxKPI)** para todas; falta modelo del portátil |
| D10 | NVIDIA con el código abierto de NVIDIA | **Decidido: SÍ, obligatorio** (plan por hitos, necesita hardware real) |
| D6 | Driver de pantalla nativo | **ABIERTA** (depende de D5) |
| D7 | EDID por DDC/VBE (BIOS) y CEA | **ABIERTA** |
| D8 | Iconos | **Hecho: simbólicos de respaldo** (27-oct-2026) |
| D9 | Intermitencia del guest y rendimiento con KVM | **ABIERTA** |

Detalle de cada una:



### D1 — Multi-CRTC, hot-plug de monitores y multi-monitor
- **Contexto**: el DRM actual es un único conector/CRTC virtual cuyo scanout es una ventana; el compositor trabaja con un solo framebuffer. En un PC real el cambio de monitores lo detecta el driver de pantalla (inexistente hoy), no el DRM genérico.
- **Opciones**:
  - (a) *No hacerlo ahora*: esperar a un driver de pantalla real (D6).
  - (b) *Capa virtual*: segundo conector/CRTC virtual (otra ventana) + evento uevent de hot-plug, solo para probar el API. **No valida nada real** y complica `drm_core.c` (asume un CRTC en todo).
  - (c) *Multi-monitor en el compositor* (varios framebuffers/GOP): trabajo grande de arquitectura (`fb_width/height` global).
- **Recomendación**: (a). Retomar con (c) cuando exista un driver que entregue más de una salida.
- **Decisión (26-oct-2026): SÍ, hay que hacerlo, pero cuando se pueda.** No se hace **ahora** (sin driver de pantalla real no hay nada que validar), pero **queda como trabajo pendiente obligatorio**, no descartado: se retoma en cuanto exista un driver (D5/D6) que entregue más de una salida. Mientras tanto no se escribe la capa virtual (opción b) porque no valida nada real.

### D2 — Cambio de resolución en caliente
- **Contexto**: `fb_width/fb_height` están cableados en todo el compositor (backbuffer, sombras, caché del fondo, límites de ventana). En PC real el modo lo fija el firmware (GOP); solo se podría cambiar con un driver (`bochs.aur`/virtio-gpu en VM, o uno nativo).
- **Opciones**: (a) no hacerlo; (b) refactor del compositor para reasignar buffers y re-maquetar, probado en QEMU con `bochs.aur`; (c) elegir resolución solo al arrancar (ya hecho con el EDID).
- **Recomendación**: (c) ya cubierto; (b) solo si se quiere resolución configurable en VM.
- **Decisión (26-oct-2026): SÍ, hay que hacerlo, pero cuando se pueda.** Queda como trabajo pendiente, no descartado: se hará cuando haya un driver de pantalla real que pueda cambiar de modo (D5/D6), con el refactor del compositor necesario y su selector en Ajustes. Mientras tanto la resolución se elige solo al arrancar (modo nativo del EDID cuando el firmware lo da).

### D3 — Portapapeles X11: alcance
- **Hecho**: texto, `CLIPBOARD`. **Falta**: `INCR` (más de ~256 KB por la conexión X), imágenes (`image/png`), `PRIMARY`↔selección primaria de Wayland, `text/uri-list`.
- **Opciones**: (a) quedarse en texto; (b) añadir `PRIMARY` (barato); (c) `INCR` + imágenes (medio).
- **Recomendación**: (b) ya; (c) cuando haya una app que lo necesite.
- **Decisión (26-oct-2026): ACEPTADA, pendiente de implementar** — opción (b): añadir `PRIMARY` ↔ selección primaria de Wayland al puente del `xwm`. Imágenes e `INCR` quedan fuera hasta que una app los necesite. **Estado: HECHO.** `aurora-wayland` puentea ahora dos canales (`CLIPBOARD` y `PRIMARY`) con el mismo código (`struct xchan`); `PRIMARY` ↔ `zwp_primary_selection_v1`. Probado en ambas direcciones y sin mezclarse con `CLIPBOARD` (`tests/wayland/guest/primary-icons.sh`, 7/7 ejecuciones); `clipboard-x11.sh` sigue pasando tras el refactor.

### D4 — Aceleración 3D: `virtio-gpu` virgl/venus
- **Contexto**: es la única aceleración que se puede probar en QEMU, pero **este entorno no trae `virtio-gpu-gl`**, así que no se podría validar aquí. Implica ampliar `virtio_gpu.aur` (contextos, `SUBMIT_3D`, fences, blobs) y ioctls `DRM_IOCTL_VIRTGPU_*`.
- **Opciones**: (a) hacerlo en una máquina con QEMU con GL; (b) saltarlo y dedicar el esfuerzo a PC real (D5/D6); (c) escribirlo sin probar (desaconsejado).
- **Recomendación original**: (b) mientras el objetivo sean PCs reales; (a) si se quiere buen rendimiento en VM.
- **Decisión (26-oct-2026): SÍ se implementará, pero NO es crítico** (prioridad baja, después de lo necesario para PC real). Requiere un QEMU con `virtio-gpu-gl` (no disponible en el entorno de pruebas actual) para poder validarlo; no se escribe sin poder probarlo. **Estado: pendiente, sin empezar.**

### D5 — Intel/AMD/NVIDIA reales: ¿capa de compatibilidad con drivers de Linux o drivers propios?
- **Contexto**: Mesa solo aporta la parte de usuario; hace falta el driver de kernel (ioctls `i915/xe`, `amdgpu`, `nouveau`). Ver Bloque 8 del plan.
- **Opciones**:
  - (a) **Capa estilo LinuxKPI/FreeBSD `drm-kmod`**: reutiliza `i915/xe`/`amdgpu` casi sin tocar y hereda sus correcciones; meses de trabajo, obliga a **GPL** para esos `.aur/.kur`, y necesita IOMMU, MSI-X, firmware (`linux-firmware`).
  - (b) **Reescribir** drivers para Aurora: inviable para 3D (millones de líneas, errata no documentada).
  - (c) **Solo pantalla** propia (modeset sin 3D): primero Intel Gen9–12 (documentación pública); AMD/NVIDIA se quedan en GOP + llvmpipe.
- **Recomendación**: (c) primero para Intel, y (a) como camino a medio plazo para aceleración. **Requiere un PC real con Intel** (y su log serie) para empezar.
- **Hardware real disponible (dato aportado el 26-oct-2026):**
  - *Sobremesa*: Intel Core i7-12700KF + NVIDIA GeForce RTX 4060.
  - *Portátil*: «i4-40xx» con Intel HD Graphics integrada (modelo exacto **por confirmar**: «i4» no existe como serie Core; probablemente un Core i3/i5 de la serie 4000, Haswell, con HD Graphics 4xxx, es decir Intel **Gen7.5**).
- **Implicaciones** (análisis, no decisión):
  - El sufijo **«KF» del i7-12700KF significa que no tiene gráficos integrados**: el sobremesa solo tiene la RTX 4060. Ahí solo cabe GOP + llvmpipe (NVIDIA: sin documentación de pantalla; nouveau/NVK exigen el firmware GSP firmado), así que **no sirve para empezar con Intel**.
  - El **portátil es el candidato a primer driver nativo de Intel**, pero si es Haswell la generación objetivo cambia: Gen7.5 en vez de la Gen9–12 supuesta en la recomendación. Intel publicó los PRM de Haswell y `i915` lo soporta, pero el motor de pantalla (DDI/pipes/PLL/eDP) difiere del de Gen9+; el volumen de trabajo y los registros no son los mismos. **Hay que confirmar el modelo (`lspci -nn`, o el ID de dispositivo PCI en el log de Aurora) antes de elegir generación.**
  - Para empezar hace falta un **log serie de arranque** del portátil (o su `lspci -nn`/ID PCI) con el ISO actual.
- **Pregunta pendiente**: ¿es aceptable la licencia GPL en los drivers de GPU si se elige la capa de compatibilidad con Linux (opción a)? (Sin respuesta todavía.)
- **Decisión (26-oct-2026): opción (a), capa de compatibilidad con los drivers de Linux (estilo LinuxKPI), para Intel, AMD y NVIDIA.** Una sola capa que sirve a todas las GPU y hereda las correcciones de Linux. Se acepta que esos módulos queden bajo GPL (confirma la respuesta a la pregunta de licencia; revisar el alcance exacto al empezar). Consecuencias:
  - Se necesita (además de lo ya existente): `struct device`/`kobject`/`kref`, `dma_fence`, `ttm`/GEM completo, `i2c`, `firmware` (ya hay `request_firmware`), `ioremap`, workqueues/kthreads (ya hay), RCU, `mutex`/`rwsem`/`completion`, `idr`/`xarray`, `list`/`rbtree`, `pci`/MSI-X, `iommu`/`dma_map` y `acpi` mínimo.
  - **Se hace primero IOMMU/VT-d (8.0)** y el DMA con máscaras: esos módulos corren en Ring 0 y manejan memoria de la GPU.
  - **NVIDIA**: `Plan_NVIDIA.md` proponía una capa de SO propia para el núcleo abierto. Con esta decisión también se puede alojar `kernel-open/` de NVIDIA (la capa Linux) sobre la misma LinuxKPI; hay que elegir al empezar (ver nota en `Plan_NVIDIA.md`).
  - Sigue valiendo: GOP como respaldo en cada hito y pruebas solo en hardware real (o VFIO + KVM).
  - Plan detallado de la capa (fases L0–L9, API por niveles, pruebas, riesgos): [`Plan_LinuxKPI.md`](../Plan_LinuxKPI.md).
- **Decisiones de L0.1 (26-oct-2026)**, detalle en `Plan_LinuxKPI.md` §11: Linux **6.18 LTS** (fijada después en **6.18.59**, con `linux-firmware` 20261019); capa bajo **GPL-2.0-only** en `kernel/linuxkpi/` + `include/linuxkpi/` con su propio `.kur`; **DRM opción A** (se mantiene `kernel/drm/` propio y la capa aporta un *shim* `drm_*`). Consecuencia para **D1/D2**: multi-CRTC, hot-plug y cambio de modo no vendrán del DRM de Linux; hay que generalizar `kernel/drm/drm_core.c` (un solo CRTC virtual hoy) cuando haya un driver real que lo use.
- **Estado LinuxKPI (26-oct-2026)**: L0 y L1 hechas (cargador `.kur` ampliado, `make linuxkpi-drv`, pruebas en la suite del kernel, cabeceras e implementación de K0/K3/K1 sin DMA; suite del kernel 189/189). Ningún driver de GPU real todavía; L2 (sincronización) espera confirmación. Detalle en `Plan_LinuxKPI.md` §11.
- **Estado LinuxKPI (26-oct-2026, tras L3)**: L0–L3 hechas. L2: sincronización, tiempo y trabajo diferido (K2). L3: modelo de dispositivos (K4) con `/sys` real y PCI/IRQ (K5): `pci_register_driver` sobre el `pci_bus` de Aurora, INTx/MSI/MSI-X con destino por CPU, `request_irq` en la IRQ real y DMA coherente provisional; hito con drivers PCI de Linux de juguete sobre `edu` y `virtio-rng-pci` de QEMU. Suite del kernel 198/198. **Consecuencia para D5/D6**: un driver de Linux solo recibe dispositivos que no reclama un driver de Aurora; para usar `bochs`/`virtio-gpu` de Linux (L5) en lugar de los `.aur` hará falta que `aur_bus` pueda no reclamarlos (pendiente). Ningún driver de GPU real todavía; L4 espera confirmación. Detalle en `Plan_LinuxKPI.md` §11 y `docs/LinuxKPI_Sesion_L3.md`.
- **Tras L3 (27-oct-2026)**: resuelto lo de D5/D6 de arriba: la opción de arranque `aur.skip=VVVV:DDDD[,…]` (`cmdline=` de `aurora.conf`) hace que `aur_bus` no lance el `.aur` de esos dispositivos, que quedan para un driver de Linux (L5: `cmdline=aur.skip=1234:1111,1af4:1050`). La medida de `virtio-gpu` incluye ya el núcleo virtio (primer hito en QEMU: 1022 símbolos). Detalle en `Plan_LinuxKPI.md` §11 («Tras L3»).

### D10 — NVIDIA: obligatorio, con el código abierto de NVIDIA (`open-gpu-kernel-modules`)
- **Decisión (26-oct-2026): SÍ, NVIDIA se implementa sí o sí.** Sustituye al «último y opcional» del Bloque 8.4. Hardware de prueba: RTX 4060 (Ada, AD107) del sobremesa.
- **Por qué es viable ahora**: desde Turing el driver oficial es el de módulos abiertos (MIT/GPL), cuyo núcleo (el *Resource Manager*, `src/nvidia`, y `src/nvidia-modeset`) es independiente del SO. Habla con la GPU a través del **firmware GSP** (binario redistribuible de NVIDIA, necesario). Es el camino por el que FreeBSD ya porta el driver.
- **Enfoque propuesto**: escribir una **capa de SO propia de Aurora** (equivalente al `os-interface.h`/`nv-linux` de NVIDIA: memoria, bloqueos, temporizadores, config PCI, MMIO, DMA, IRQ/MSI-X, carga de firmware) en vez de emular todo Linux. Como `.kur` (Ring 0), porque el RM necesita IRQs y control fino; el `.aur` queda para piezas aisladas.
- **Hitos** (cada uno validable solo en el PC real, con log serie):
  1. Sondeo PCI + mapa de BARs + lectura de identidad (`NV_PMC_BOOT_0`), sin tocar la pantalla.
  2. Carga del firmware GSP y arranque del GSP-RM.
  3. Modeset nativo (nvidia-modeset) con GOP como respaldo automático. Aquí se desbloquean D1/D2 en NVIDIA.
  4. Memoria y *channels* de la GPU, y la ABI de `/dev/nvidiactl` + `/dev/nvidia0`.
  5. 3D: dos vías, a elegir al llegar aquí. (a) Userspace propietario de NVIDIA bajo la capa Linux de Aurora (necesita sus ioctls y `nvidia-drm`). (b) `nouveau` + NVK de Mesa (abierto, GPL, necesita la capa tipo LinuxKPI de D5(a)).
- **Riesgos**: no se puede probar en este entorno (sin GPU ni passthrough); sin IOMMU (8.0) el DMA de la GPU es un riesgo de seguridad; el firmware GSP hay que obtenerlo de NVIDIA/`linux-firmware` y su licencia debe revisarse; programar sin hardware conectado puede dejar la pantalla en negro, por eso cada hito conserva GOP como respaldo.
- **Pendiente del autor**: confirmar que la RTX 4060 es el equipo de pruebas y poder enviar un log serie o `lspci -nnk`. Hasta entonces solo se puede escribir la capa de SO y los hitos 1–2 «a medias», sin validarlos.

### D6 — Driver de pantalla nativo (modeset) y cambio de modo real
- **Contexto**: depende de D5(c). Sin él: sin cambio de resolución real (D2), sin hot-plug (D1), sin cursor por hardware, sin DPMS real.
- **Recomendación**: empezar por Intel tomando el estado que deja el firmware y cambiando de modo solo después, con GOP como respaldo automático. La generación objetivo depende del portátil (ver D5: probablemente Gen7.5/Haswell, no Gen9+).
- **Decisión**: ABIERTA (hardware ya identificado; pendiente confirmar el modelo y obtener el log de arranque). D1 y D2 (hot-plug, multi-monitor y cambio de resolución en caliente) **siguen siendo trabajo obligatorio** sobre este driver, pero se pueden dejar para después de una primera versión que solo muestre imagen con GOP como respaldo.

### D7 — EDID por DDC/VBE en el arranque BIOS y extensión CEA
- **Contexto**: el EDID solo llega por UEFI. En BIOS no hay EDID; el bloque CEA (modos de TV, audio) no se analiza.
- **Recomendación**: baja prioridad; hacerlo cuando se pruebe en un PC con BIOS legado.
- **Decisión**: ABIERTA.

### D8 — Iconos: arte definitivo y apps solo-simbólicas
- **Contexto**: los iconos de `xclock`, `xeyes`, `hello-gtk`, `gtk3-icon-browser`, `qt5-*` son placeholders propios; las apps que solo traen iconos `-symbolic` no muestran icono (usan la baldosa con iniciales).
- **Opciones**: (a) sustituir por arte definitivo con los mismos nombres; (b) usar el icono simbólico recoloreado como respaldo.
- **Decisión (26-oct-2026): por ahora que se usen los iconos simbólicos** como respaldo para las apps que no traen icono normal (opción b); los placeholders propios actuales se mantienen donde existen. **Estado: HECHO.** `icons.c` busca `NOMBRE-symbolic` (`<tema>/symbolic/apps`, `scalable/apps` y los `.symbolic.png` de GTK) solo cuando no hay icono normal, lo recolorea a blanco azulado conservando el alfa y lo pone sobre una baldosa redondeada oscura. Para probarlo, `AW_APPS_DIR` permite apuntar a otro directorio de `.desktop` (el real es de solo lectura en el guest).

### D9 — Intermitencia del guest y rendimiento con KVM
- **Contexto**: el bloqueo ocasional del shell en TCG sigue sin causa; no se ha medido rendimiento con KVM (en TCG `gtk3-widget-factory` tarda 2–3 min en pintar).
- **Recomendación**: medir con KVM en una máquina con virtualización antes de optimizar; investigar la intermitencia con un trazado del scheduler cuando se reproduzca.
- **Decisión**: ABIERTA.
- **Dato nuevo (26-oct-2026):** 6 ejecuciones seguidas de `primary-icons.sh` en TCG, una a una, sin ningún cuelgue. El único cuelgue de esa sesión coincidió con **dos QEMU a la vez** (comparten `/tmp/boot-test.log` y `OVMF_VARS.fd`), así que probablemente fue un artefacto de pruebas solapadas; ejecutar siempre una sola instancia. No descarta la intermitencia antigua (~5/45).

## 7. Resumen de prioridades sugeridas

0. D1 y D2 (multi-monitor/hot-plug y cambio de resolución en caliente): hay que hacerlos, aplazados hasta que exista el driver de pantalla (D5/D6).
1. Confirmar el modelo del portátil Intel y obtener un log serie de arranque / `lspci -nn` (desbloquea D5 y D6 y valida §4.1 y §4.5). El sobremesa (i7-12700KF **sin iGPU** + RTX 4060) solo sirve para probar GOP.
2. ~~D3(b): `PRIMARY`~~ hecho. 3. ~~D8: iconos simbólicos~~ hecho.
4. D9: medir con KVM.
5. D4: `virtio-gpu` 3D (decidido que sí, no crítico; necesita QEMU con GL).

Cuando se decida algo, rellenar el campo **Decisión** de la sección correspondiente (con fecha) y reflejarlo en `Plan_Wayland_Unificado.md`.
