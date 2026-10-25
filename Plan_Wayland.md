# Plan: Wayland + más drivers gráficos en AuroraOS, con librerías Linux sin recompilar

## Contexto

Objetivo: ejecutar libwayland, libdrm, Mesa, libxkbcommon, libinput, pixman, un compositor Wayland (Weston o similar) y clientes (foot, GTK/Qt) **tal cual vienen de una distro glibc**, sin recompilarlos. Eso significa que el trabajo está en el **kernel** (ABI Linux + nodos de dispositivo), no en portar bibliotecas. Reglas: no tocar las libs; todo lo que falte se emula en `linux-compat` y se expone con la interfaz estándar del kernel Linux.

## Estado actual (verificado leyendo el código)

**Linux-compat (sólido en userspace, vacío en lo que Wayland necesita)**
- ELF ET_DYN + PT_INTERP + ld.so.cache, vDSO, `clone3`, futex, epoll/eventfd/signalfd, `memfd_create` con sellos (`kernel/memfd.c`, mmap vía VFS), PTY, `/proc` y `/sys` extensos. Python, Perl, git, bash funcionan (glibc_port.md).
- **Faltan** (todo imprescindible para Wayland):
  - `AF_UNIX`: `k_socket` solo acepta `AF_INET` (`kernel/syscall.c:5110`), `k_socketpair` devuelve `-EOPNOTSUPP` (`:5824`); `sendmsg/recvmsg` solo sobre sockets inet → no hay `SCM_RIGHTS`, que es el mecanismo por el que Wayland pasa fds de memfd/dma-buf.
  - `mknod` solo crea ficheros regulares (`:4538`); no hay devtmpfs/`/dev/dri`, `/dev/input/event*`.
  - `timerfd_*`, `inotify_*`, `pidfd_open`, `AF_NETLINK` (uevent/libudev), `epoll` sobre AF_UNIX/DRM/evdev, `rseq` real, `ptrace` (opcional).
  - `ioctl` solo enruta a netif y luego a `vfs_ops.ioctl` (`:2784`): el mecanismo existe y sirve para DRM/evdev.

**Gráficos (compositor en kernel, sin capa de dispositivo)**
- `kernel/gfx/` (compositor 1672 líneas, `winsrv.c`, window, dock, menubar…) pinta sobre el framebuffer GOP/VBE entregado por el bootloader (`kernel/main.c:284`, `kernel_boot_info.h`). Apps nativas usan `SYS_WIN_*` (crear ventana, `winsrv_blit` de píxeles, eventos).
- **No hay ningún driver de GPU**, ni DRM/KMS, ni EDID, ni modeset: ROADMAP.md:330 lo marca pendiente. Solo framebuffer plano.
- Framework de drivers reutilizable: `.aur` (userspace, con capacidades `AUR_CAP_*` en `kernel/aur.h`: MMIO, DMA, IRQ, MSI, NETIF, INPUT_DEVICE…) y patrón *proxy* ya probado: `input_proxy.c`, `net_proxy.c`, `block_proxy.c`, `dma_user.c` (buffer DMA mapeable por mmap), `msi_user.c`, `irq_user.c`. Drivers `.aur` existentes: ahci, e1000e, ps2, xhci.
- Input: ps2.aur → `/dev/inputproxy` → kernel; xhci con `hid_parser.c`.
- sysroot solo trae `libudev.so.1` y `libffi.so.8`; no hay libwayland/drm/mesa/xkb aún.

## Arquitectura propuesta

```
 clientes Wayland (foot, GTK, Qt)  ──AF_UNIX+SCM_RIGHTS──►  compositor
        │ wl_shm: memfd (existe)                              │
        │ linux-dmabuf / EGL: Mesa kms_swrast (sin GPU real)  │
        ▼                                                     ▼
   libwayland, Mesa, libdrm, xkbcommon, libinput  (binarios de distro, sin tocar)
                                   │ ioctl / mmap / read / poll
 ═════════ linux-compat (kernel) ═══╪════════════════════════════════
   AF_UNIX │ devtmpfs │ DRM core (card0/renderD128) │ evdev │ uevent/sysfs
                                   │
           ┌───────────────────────┴──────────────┐
      drm_core ← backends GPU (.aur vía gpu_proxy, igual que net/input)
           │            │              │             │
        GOP/fb      bochs-dispi     virtio-gpu    vmsvga … Intel/AMD (largo plazo)
           │
   scanout → compositor nativo Aurora (gfx/compositor.c) o framebuffer completo
```

**Decisión de diseño recomendada:** la capa DRM/KMS que ve Linux es **emulación de la ABI en kernel** (ioctls de `drm.h`/`drm_mode.h`), no un port del DRM de Linux. El hardware se maneja con drivers `.aur` que registran *connector/CRTC/plane* en un `drm_core`. Así los drivers futuros no dependen del código de Linux.

Primer objetivo de render: **dumb buffers + pixman/ Mesa llvmpipe-softpipe (`kms_swrast`)**; no hace falta aceleración 3D para que Wayland funcione. La aceleración real (virgl vía virtio-gpu) es una fase posterior.

## Fases

### Fase 0 — Cimientos y banco de pruebas
1. Añadir al sysroot (desde paquetes de distro, sin compilar): libwayland-{client,server,cursor}, libdrm, libxkbcommon (+ `/usr/share/X11/xkb`), pixman, libinput, libevdev, libmtdev, Mesa (`libgbm`, `libEGL`, `libGLESv2`, `dri/kms_swrast_dri.so`, `libLLVM` si se quiere llvmpipe), `weston` y `foot`. Integrarlo en la generación de `ld.so.cache`.
2. Smoke tests reproducibles (P0 de glibc_port.md) y un script `tests/wayland/` que ejecute cada hito en QEMU y guarde logs.
3. Activar los logs `[SYSCALL-GAP]` / `[OPEN-FAIL]` y usarlos para guiar cada fase (reproducir primero, no implementar a ciegas).

### Fase 1 — ABI Linux requerida (independiente de gráficos)
Ficheros: `kernel/syscall.c`, nuevo `kernel/unix_socket.c`, `kernel/timerfd.c`, `kernel/inotify.c`, `kernel/devtmpfs.c` (patrón: `epoll.c`, `signalfd.c`, `memfd.c`).
1. **AF_UNIX** `SOCK_STREAM/SEQPACKET/DGRAM`, `bind/connect/listen/accept4`, `socketpair`, rutas de filesystem y abstractas, `SO_PEERCRED`, `MSG_CMSGCLOEXEC`, **`SCM_RIGHTS`** (duplicar `file*` entre tablas de fds) y `SCM_CREDENTIALS`. Generalizar `k_sendmsg/k_recvmsg` para despachar por tipo de socket en vez de `inet_socket_from_fd`. Integración con `poll/epoll` (`poll_wq`).
2. **devtmpfs** montado en `/dev`, con `mknod` de char devices (major:minor) y nodos creados por los drivers al registrarse; `/dev/shm` y `/run` (tmpfs) con `XDG_RUNTIME_DIR=/run/user/<uid>`.
3. `timerfd_create/settime/gettime`, `inotify_*`, `pidfd_open`, `eventfd` ya existe, `rseq` básico, `fcntl` completo (`F_DUPFD_CLOEXEC`, locks, sellos ya hechos).
4. `mmap` con `MAP_SHARED` sobre nodos de dispositivo ya soportado vía `vfs_ops.mmap`: verificar semántica compartida entre procesos para memfd (cliente escribe, compositor lee) — es el camino `wl_shm`.
- **Hito:** un cliente y servidor de prueba (libwayland-client/server sin tocar) intercambian un `wl_shm_pool`; `wayland-info`/`wl_display_connect` OK.

### Fase 2 — Input estilo Linux
Ficheros: `kernel/input.c`, `input_proxy.c`, nuevo `kernel/evdev.c`.
1. `/dev/input/eventN` con `struct input_event`, `EVIOCGVERSION/EVIOCGID/EVIOCGNAME/EVIOCGBIT/EVIOCGABS/EVIOCGRAB/EVIOCGPROP`, `poll`/`epoll`, clock `CLOCK_MONOTONIC` (`EVIOCSCLOCKID`). Alimentarlo desde PS/2 y HID USB (traducir scancodes a `KEY_*`).
2. `/sys/class/input/*`, `/run/udev/data/c13:*` y propiedades `ID_INPUT_*` para que libinput/libudev (ya en sysroot) detecte teclado/ratón/touchpad.
3. VT/seat mínimo: `/dev/tty0`, `KDSETMODE`, `VT_*` como no-ops coherentes (Weston/wlroots los usan en el backend DRM), y `seatd` o `libseat` backend `builtin`/sin logind.
- **Hito:** `libinput debug-events` (binario de distro) muestra eventos.

### Fase 3 — DRM/KMS core (emulación de ABI)
Ficheros nuevos: `kernel/drm/drm_core.c`, `drm_ioctl.c`, `drm_dumb.c`, `drm_sysfs.c`; `kernel/drm/fb_gop.c` (primer backend).
1. Nodos `/dev/dri/card0` (226:0) y `/dev/dri/renderD128` (226:128); `/sys/class/drm/card0{,-Virtual-1}/…` con `dev`, `uevent`, `device/{vendor,device,subsystem_vendor,subsystem_device}`, `status`, `modes`, `edid`. `libdrm.drmGetDevices()` depende de esto.
2. Ioctls: `DRM_IOCTL_VERSION`, `GET_CAP` (`DUMB_BUFFER`, `PRIME`, `ADDFB2_MODIFIERS`, `CRTC_IN_VBLANK_EVENT`, `TIMESTAMP_MONOTONIC`, `CURSOR_WIDTH/HEIGHT`), `SET_CLIENT_CAP` (`UNIVERSAL_PLANES`, `ATOMIC`), `SET_MASTER/DROP_MASTER`, `MODE_GETRESOURCES/GETCONNECTOR/GETENCODER/GETCRTC/SETCRTC/GETPLANE(RESOURCES)`, `MODE_CREATE_DUMB/MAP_DUMB/DESTROY_DUMB`, `MODE_ADDFB2/RMFB`, `MODE_PAGE_FLIP`, `MODE_CURSOR/CURSOR2`, `MODE_OBJ_GETPROPERTIES`, `MODE_GETPROPERTY`, **atomic commit** (necesario para Weston/wlroots modernos), blobs de propiedades, `GEM_CLOSE`.
3. **Eventos de vblank/page-flip** legibles con `read()` sobre el fd de card0 (`drm_event_vblank`), con temporizador de 60 Hz si el backend no tiene vblank real.
4. **GEM + PRIME:** `PRIME_HANDLE_TO_FD/FD_TO_HANDLE` exportando como fd (mismo mecanismo que `memfd`/`dma_user.c`), para `linux-dmabuf` y `wl_drm`. Fences `sync_file` más adelante.
5. Backend 1 (`fb_gop`): un connector/CRTC/plane primario sobre el framebuffer del bootloader; modeset = no-op con las dimensiones de GOP; flip = copia/swap.
6. **Integración con el compositor nativo (decisión a confirmar con el usuario):**
   - **A (recomendada para empezar): modo ventana.** El scanout del CRTC es un `window_t` de `winsrv` ("Escritorio Wayland") → Weston corre dentro del compositor Aurora, conviven apps nativas y Wayland. Reutiliza `winsrv_blit`.
   - **B: modo exclusivo.** El `drm_core` toma el framebuffer completo (`SET_MASTER`), suspendiendo gfx/compositor al estilo VT switch.
   - **C (más ambiciosa, más macOS-like):** un compositor Wayland **nativo** propio (nuevo binario userspace enlazado contra libwayland-server de distro) que convierta cada `xdg_toplevel` en una ventana nativa de Aurora (dock, menubar, sombras). Se hace después de A/B.
- **Hito:** `modetest` (libdrm-tests) lista conector/modo y pinta un patrón; `kmscube`/`drm-info` funcionan.

### Fase 4 — Wayland funcionando
1. Compositor de referencia sin modificar: **Weston** (`--backend=drm --renderer=pixman`) o `sway/wlroots` con `WLR_RENDERER=pixman`.
2. Clientes: `weston-terminal`/`foot`, `wl_shm`, `xdg-shell`, cursor theme, portapapeles (`wl_data_device`), `wp_viewporter`.
3. Mesa software: `MESA_LOADER_DRIVER_OVERRIDE=kms_swrast` / `LIBGL_ALWAYS_SOFTWARE=1`, `GALLIUM_DRIVER=llvmpipe` o `softpipe` si no se quiere LLVM; EGL sobre GBM con dumb buffers.
4. Xwayland (opcional, más tarde): necesita `AF_UNIX` abstractos, `/tmp/.X11-unix`, `SCM_RIGHTS`, dma-buf.
- **Hito:** Weston arranca, `foot` muestra un terminal, teclado y ratón funcionan, el cierre limpio libera buffers.

### Fase 5 — Más drivers gráficos (`.aur` + `gpu_proxy`)
Nueva capacidad `AUR_CAP_GPU` (`kernel/aur.h`) y `kernel/gpu_proxy.c` (patrón `input_proxy.c`/`net_proxy.c`): el `.aur` recibe BAR/MMIO (`AUR_CAP_MMIO`), DMA (`dma_user.c`), MSI (`msi_user.c`), y registra con el kernel connectors/CRTC/planes/EDID y callbacks `modeset/flip/cursor`.
Orden por coste/beneficio (todos testeables en QEMU):
1. **bochs-dispi / QEMU stdvga** (PCI 1234:1111): modeset real por registros VBE-DISPI, sin 3D. Es el driver más pequeño; valida todo el camino `.aur`→`drm_core`.
2. **virtio-gpu 2D** (1af4:1050): comandos por virtqueue (`RESOURCE_CREATE_2D`, `ATTACH_BACKING`, `SET_SCANOUT`, `TRANSFER_TO_HOST_2D`, `RESOURCE_FLUSH`), cursor queue, EDID. Requiere `virtio_bus` (stub en drv_impl_plan.md:50). Da modeset dinámico y *page-flip* real.
3. **virtio-gpu 3D / virgl** (opcional): `CTX_CREATE`, `SUBMIT_3D` + Mesa `virgl_dri.so`/`renderD128`, `DRM_IOCTL_VIRTGPU_*`. Aceleración sin recompilar Mesa, pero exige emular los ioctls específicos de virtio_gpu. Solo si se necesita GL rápido en QEMU.
4. **vmsvga (VMware SVGA II)** para VMware/VirtualBox.
5. **Hardware real:** Intel (i915/Xe) y AMD (amdgpu) son proyectos de varios meses (firmware, GTT/PPGTT, ring buffers, display engine). Recomendación: modeset-only con *GOP + Intel/AMD display-engine mínimo* y render por software; GPU real queda fuera de este plan salvo petición explícita.

### Fase 6 — Pulido
`wp_presentation`, `zwp_linux_dmabuf`, suspensión/hotplug de monitores (uevent netlink), multi-CRTC, `wl_seat` con touch, IME, tests de estrés (clientes muertos con buffers pendientes, cierre del fd en medio de un flip), y actualizar `TODO.md`/`ROADMAP.md`/`glibc_port.md` a la vez.

## Archivos críticos
- Modificar: `kernel/syscall.c` (socket/sendmsg/recvmsg/mknod/ioctl, tabla `SYS_*`), `kernel/vfs.c` + `vfs.h` (`vfs_ops.mmap/poll/ioctl` ya existen), `kernel/aur.h`/`aur_loader.c` (nueva cap GPU), `kernel/gfx/winsrv.c` y `compositor.c` (superficie scanout), `kernel/sysfs.c` (`/sys/class/{drm,input}`), `kernel/procfs.c`, `Makefile`/`kernel/Makefile`.
- Nuevos: `kernel/unix_socket.c`, `devtmpfs.c`, `timerfd.c`, `inotify.c`, `evdev.c`, `kernel/drm/*`, `gpu_proxy.c`, `drivers/aur/{bochs,virtio_gpu,vmsvga}/`.
- Reutilizar: patrón de `epoll.c`/`signalfd.c`/`memfd.c` para nuevos fd; `dma_user.c` para buffers compartidos; `input_proxy.c`/`net_proxy.c` como plantilla de proxy; `tests/` + `make run-smp-kvm-ahci` para regresión.

## Verificación
1. Cada fase tiene un hito ejecutable en QEMU (arriba); un script `tests/wayland/run.sh` los encadena y guarda logs/códigos de salida.
2. Regresión: smoke suite existente (bash, git, python, epoll, PTY) debe seguir idéntica tras cada fase.
3. Pruebas por ABI con binarios de distro: `socat`/`wayland-info`, `libinput debug-events`, `modetest`, `weston`, `foot`, `eglinfo`.
4. QEMU: `-device VGA`, `-device bochs-display`, `-device virtio-gpu-pci` (+ `-display gtk,gl=on` para virgl).

## Riesgos
- Mesa/llvmpipe pesa (LLVM) y necesita muchos syscalls (`mprotect` JIT, `sched_*`, `membarrier`); empezar con pixman/softpipe.
- Weston/wlroots piden atomic KMS + `libseat`: sin ellos no arrancan en DRM backend.
- `SCM_RIGHTS` toca la gestión de `file*` y cierre de fds (refcounts, CLOEXEC); es el cambio más delicado de la Fase 1.
- Sin seguridad entre procesos en `/dev/dri` hasta definir permisos/`SET_MASTER`.

## Preguntas abiertas
1. ¿Modo ventana dentro de tu compositor (A), pantalla completa exclusiva (B) o compositor Wayland nativo integrado (C)?
2. ¿Hardware objetivo: solo QEMU/VMs (bochs, virtio-gpu, vmsvga) o también Intel/AMD reales?
3. ¿Prioridad: Wayland funcionando primero con software rendering, o aceleración (virgl) desde el principio?
