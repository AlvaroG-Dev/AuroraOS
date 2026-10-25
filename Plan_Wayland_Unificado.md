# Plan unificado: Wayland + drivers gráficos + pendientes de drv_impl_plan

Fusiona `Plan_Wayland.md` y `drv_impl_plan.md`. Orden por dependencias; cada bloque termina en un hito verificable en QEMU y no debe romper la suite existente (175/175).

## OBJETIVO (aclarado por el autor) — leer primero

**Las apps gráficas de Linux deben ejecutarse integradas en la interfaz de Aurora**: cada ventana Wayland (`xdg_toplevel`) es una ventana más del compositor de Aurora, con su barra de título, sombras, animaciones, Dock, barra de menús, foco, cursor y atajos nativos. **No** se busca Weston ni ningún otro escritorio aparte, ni una ventana "pantalla completa" con otro entorno dentro.

Consecuencias para el plan:
- **Weston, `modetest`, `drm_info` y `kmscube` son solo herramientas de validación** del ABI Linux (DRM/KMS, seat, input, memfd, señales...). Que corran bien demuestra que la capa de compatibilidad es correcta; no son el producto final.
- El producto final es un **servidor Wayland propio de Aurora**: un proceso de usuario con la `libwayland-server` de Ubuntu (sin tocar) que implementa `wl_compositor`, `wl_shm`, `xdg_shell`, `wl_seat`, `wl_output`, `wl_data_device` (portapapeles), etc. y los traduce a ventanas de `winsrv`/compositor (crear ventana por toplevel, volcar el `wl_buffer` a su `content_buffer`, eventos de teclado/ratón/foco/redimensión/cierre de vuelta al cliente). El input llega por el foco del compositor de Aurora, no por un seat exclusivo.
- La ventana "AuroraOS DRM" del Bloque 4 (scanout como una ventana que cubre el área de trabajo) queda como **modo de validación/legado**, útil para ejecutar un compositor externo en una ventana, pero no es la integración buscada.
- Los bloques que siguen se reordenan en consecuencia: el Bloque 5 pasa a ser la integración nativa (5.A), y Weston/DRM queda como 5.V (validación, ya hecho).

Decisiones por defecto (cambiar si no valen): integración **A** (cada toplevel Wayland = ventana de `winsrv`), hardware **solo QEMU/VMs**, **render por software** primero (pixman/kms_swrast), virgl al final.

## Bloque 0 — Cimientos (sin dependencias)
- 0.1 Smoke tests reproducibles + `tests/wayland/run.sh`; activar logs `[SYSCALL-GAP]`/`[OPEN-FAIL]`. (Wayland F0.2-3, 7.2-7.4 de drv_impl_plan)
- 0.2 Sysroot: libwayland, libdrm, xkbcommon, pixman, libinput, libevdev, mtdev, Mesa (kms_swrast), weston, foot; regenerar `ld.so.cache`.

## Bloque 1 — Prerrequisitos de drv_impl_plan que Wayland necesita
- 1.1 **Canal genérico de dispositivo para .aur (4.6 pendiente)** → base de `gpu_proxy`. Hacerlo antes de Fase 5 de Wayland.
- 1.2 **`virtio_bus` real (2.1 pendiente)**: requisito de virtio-gpu (y virtio-input opcional).
- 1.3 **Cap `AUR_CAP_GPU`** en `kernel/aur.h` + `aur_loader.c`.
- 1.4 **MSI-X para .aur (limitación 4.5)**: virtio-gpu y xhci moderno lo usan; mientras tanto polling/MSI.
- 1.5 **Per-CPU runqueues (1.7)**: opcional, recomendable antes de cargar compositor + clientes + drivers.
- 1.6 5.6 *winsrv.aur*: **no** se hace; el scanout vive en kernel (decisión A). Marcar como descartado.

## Bloque 2 — ABI Linux (Wayland F1)
- 2.1 **[HECHO]** `kernel/unix_socket.c`: AF_UNIX STREAM/SEQPACKET/DGRAM, bind/connect/listen/accept4, socketpair, abstractos, `SO_PEERCRED`, **SCM_RIGHTS**/CREDENTIALS, integración poll/epoll. Generalizar `k_sendmsg/k_recvmsg` por tipo de socket.
- 2.2 **[HECHO]** devtmpfs en `/dev` + `mknod` de char devices; `/dev/shm`, `/run`, `XDG_RUNTIME_DIR`.
- 2.3 **[HECHO salvo rseq]** `timerfd`, `inotify`, `pidfd_open`, `rseq` básico, `fcntl` completo.
- 2.4 **[HECHO]** Verificar `MAP_SHARED` de memfd entre procesos (camino `wl_shm`).
- **Hito:** cliente/servidor libwayland sin tocar intercambian `wl_shm_pool`.

## Bloque 3 — Input estilo Linux (Wayland F2)
- 3.1 **[HECHO]** `evdev.c`: `/dev/input/eventN`, ioctls EVIOC*, alimentado por `input_proxy` (PS/2) y HID USB.
- 3.2 **[HECHO]** `/sys/class/input`, `/run/udev/data`, `ID_INPUT_*`; `AF_NETLINK` uevent mínimo.
- 3.3 **[VT hecho; libseat pendiente]** VT/seat mínimo (`/dev/tty0`, `KDSETMODE`, `VT_*`) + libseat builtin.
- **Hito:** `libinput debug-events`.

## Bloque 4 — DRM/KMS core (Wayland F3)
- 4.1 **[HECHO]** `kernel/drm/{drm_core,drm_dumb,drm_sysfs}.c`, nodos `card0`/`renderD128`, sysfs.
- 4.2 **[HECHO salvo PRIME]** Ioctls legacy + atomic, blobs, eventos flip/vblank (timer 60 Hz), GEM dumb. PRIME/dma-buf pendiente (`DRM_CAP_PRIME=0`).
- 4.3 **[HECHO]** Scanout como ventana sin decoración del compositor (modo A); copia del fb activo a `content_buffer`.
- **Hito:** `modetest`, `drm-info` **hechos**; `kmscube` pendiente de Mesa/GBM (Bloque 0.2).

## Bloque 5 — Wayland funcionando (F4)
**5.V Validación con Weston (HECHO, ver notas):** Weston `--backend=drm --renderer=pixman` + clientes de demo. Solo demuestra que la capa Linux es correcta.

**5.A Integración nativa en la interfaz de Aurora (OBJETIVO REAL, pendiente):**
- 5.A.1 Servidor Wayland de Aurora (`aurora-wayland`, proceso de usuario, `libwayland-server` de Ubuntu): `wl_compositor`, `wl_subcompositor`, `wl_shm`, `wl_output`, `wl_seat`, `xdg_wm_base`/`xdg_toplevel`/`xdg_popup`, `wl_data_device_manager`, `xdg-decoration` (decoración del lado del servidor = la de Aurora).
- 5.A.2 Puente con `winsrv`: un toplevel = una ventana de Aurora (título, tamaño, minimizar/maximizar/cerrar nativos, icono en el Dock); `wl_surface.commit` copia el `wl_buffer` a la ventana; popups y subsuperficies como capas dentro de ella.
- 5.A.3 Input por foco: teclado/ratón del compositor de Aurora hacia el `wl_seat` del cliente enfocado (keymap con xkbcommon), cursor del cliente (`wl_pointer.set_cursor`) integrado en el cursor de Aurora, portapapeles compartido.
- 5.A.4 Lanzar apps Linux gráficas desde el Dock/Finder como cualquier otra app (variables `WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR` ya preparadas por el sistema).
- 5.A.5 Mesa/EGL (kms_swrast) para clientes GL por `wl_drm`/dmabuf o `wl_shm` con renderizado por software.
- **Hito real:** `foot`, `weston-terminal`, `weston-flower` y una app GTK/Qt se abren como ventanas normales de Aurora, con teclado y ratón, junto a las apps nativas, sin ningún otro escritorio.

## Bloque 6 — Drivers gráficos `.aur` (F5)
- 6.1 `gpu_proxy.c` (patrón input/net proxy) + registro de connector/CRTC/plane/EDID.
- 6.2 `bochs.aur` (1234:1111) → 6.3 `virtio_gpu.aur` 2D → 6.4 virgl (opcional) → 6.5 vmsvga.
- Reutiliza supervisor (6.3), watchdog (6.2) y hot-plug PCI (6.4) de drv_impl_plan: añadir plazos/watchdog a `gpu_proxy`.

## Bloque 7 — Robustez y pulido
- 7.1 Hardening: tests de estrés (cliente muerto con buffers, cierre de fd en flip), fuzzing de ioctls DRM/AF_UNIX.
- 7.2 IOMMU/VT-d (8.0): cada `.aur` GPU con `AUR_CAP_DMA` es riesgo; PRIME/dma-buf comparten memoria física.
- 7.3 `wp_presentation`, dmabuf, hotplug de monitores, multi-CRTC, touch.
- 7.4 Actualizar `TODO.md`, `ROADMAP.md`, `glibc_port.md`, `drv_impl_plan.md`.

## Fuera de alcance
Intel/AMD reales, Xwayland (hasta que 2.1 y dmabuf estén estables).

## Riesgos clave
SCM_RIGHTS (refcounts de `file*`), Weston exige atomic KMS + libseat, Mesa/LLVM pesada (empezar con softpipe/pixman).

## Estado / notas de implementación
- **2.1 AF_UNIX hecho** (`kernel/unix_socket.c`, 177/177 tests de kernel + `user/musl/apps/unix_test`, todo PASS en QEMU): STREAM/DGRAM/SEQPACKET, socketpair, bind/listen/accept4/connect (ruta y abstractos), SCM_RIGHTS, SO_PEERCRED/SO_PASSCRED, poll/epoll, EOF/EPIPE. Pendiente: SIGPIPE (hoy solo -EPIPE), crear inodo S_IFSOCK en el VFS al hacer bind (hoy solo tabla interna; `test -S` y `ls` no lo verán), SO_RCVTIMEO.
- **Límite crítico:** `MAX_PROCESS_FDS = 32` (y `fd_cloexec_mask` es `uint32_t`). Un compositor con varios clientes + buffers shm lo agota. Subirlo (p. ej. 1024, máscara por bitmap) es prerrequisito del Bloque 5; añadir al Bloque 2.
- `/etc` es un tmpfs montado en arranque: ficheros metidos en `sysroot/etc` no llegan. Para el sysroot de Wayland usar rutas fuera de `/etc` o poblar el tmpfs desde `kernel/tmpfs.c`.
- Hook de pruebas: si existe `/system/boot-tests` (una orden por línea), `init` las ejecuta con bash y envía la salida a `/dev/kmsg` (serie). QEMU headless: `qemu-system-x86_64 ... -display none -serial file:/tmp/u.log`.
- **Límite de fds resuelto:** `MAX_PROCESS_FDS = 256`; close-on-exec pasa a bitmap (`fdmask_t`, helpers `fd_cloexec_*` en `process.h`). Probado con 200 fds + `F_DUPFD_CLOEXEC`.
- **devtmpfs (2.2) hecho sobre el devfs existente** (que ya tenía tabla estática, `/dev/pts`, `/dev/irq`, block devices): nuevo `kernel/devtmpfs.c` = registro dinámico (`devtmpfs_register("dri/card0", DEVT_MKDEV(226,0), 0666, uid, gid, ops, priv)`), subdirectorios implícitos, readdir/lookup integrados en `vfs.c`. `mknod`/`mknodat` de char devices (root, solo bajo `/dev`, alias de un major:minor ya registrado; `-ENODEV` si no hay driver) y `unlink` de nodos dinámicos. `/run/user/0` creado en `tmpfs_init`. Tests: kernel `devtmpfs: registro, lookup y readdir` y `user/musl/apps/dev_test`.
- **Aviso de build:** el Makefile del kernel no rastrea cabeceras. Tras tocar una struct de un `.h` (vfs_ops_t, process_t...) hay que borrar los `.o` (`find kernel -name '*.o' -not -name '*blob*' -delete`) o salen fallos extraños (poll roto, etc.).
- `scripts/qemu-boot-test.sh [-t seg] orden...`: construye la ISO, arranca QEMU sin pantalla ejecutando las órdenes al arrancar; log en `/tmp/boot-test.log`.
- **Bloque 2 completado** (salvo `rseq` real y locks de `fcntl`): `timerfd` (kthread propio, resolución 1 ms, read/poll/epoll, ABSTIME), `pidfd_open` (legible al terminar el proceso), `inotify` **stub** (fd válido, `add_watch` valida la ruta, nunca emite eventos), `ftruncate`/`truncate` (faltaban), y **`mmap(MAP_SHARED)` real de memfd** entre procesos: memfd ahora guarda páginas físicas estables (`ctx->pages`), nuevo `vfs_ops.get_page` + `VMA_SHARED` (demand paging con `PTE_SPECIAL|PTE_SHARED_OBJ`; fork comparte las páginas; encoger el memfd pone a cero pero conserva páginas). Antes `MAP_SHARED` sobre ficheros daba `-ENODEV`. Tests: `timerfd_test`, `shm_test` (memfd por `SCM_RIGHTS` entre procesos), `unix_test`, `dev_test`; kernel 178/178.
- Otros ficheros con `MAP_SHARED` (tmpfs, fat32) siguen sin soportarlo: implementar `get_page` en tmpfs si algún cliente lo necesita.
- **Hito de la Fase 1 alcanzado:** `tests/wayland/wl_test` (compilado en el host contra las cabeceras de `libwayland-dev` y enlazado dinámicamente a **libwayland-server/client 1.22 de Ubuntu sin tocar**) corre en AuroraOS: servidor + cliente en procesos distintos, `wl_display_add_socket` (lock + bind + listen), connect, registry, `wl_shm` (memfd + `SCM_RIGHTS` + `MAP_SHARED`), `wl_surface.attach/commit`, el servidor lee los píxeles del cliente y envía `wl_buffer.release`. Reproducible: `scripts/fetch-wayland.sh` (libwayland, libxkbcommon+xkb-data, pixman, libdrm, libinput, libevdev, libmtdev de Ubuntu a `libs/glibc` y `precompiled/`) y `tests/wayland/run.sh` (encadena unix_test, dev_test, timerfd_test, shm_test, wl_test). Único ruido: `rseq` (334) devuelve `-ENOSYS`, glibc lo tolera.
- Bloque 0 parcialmente hecho: ya están las libs de Wayland/xkb/pixman/drm/input en el sysroot; faltan Mesa (`libgbm`, `libEGL`, `kms_swrast`), `weston` y `foot`.
- **Bloque 3 (input estilo Linux) hecho, salvo `libseat`:**
  - `kernel/evdev.c`: `/dev/input/event0` (teclado) y `event1` (ratón) con `struct input_event` de Linux, `EVIOCGVERSION/ID/NAME/PHYS/BIT/KEY/PROP/GRAB/REVOKE/SCLOCKID`, SYN_REPORT por paquete, autorepeat (`value=2`), `SYN_DROPPED`, poll/epoll, `O_NONBLOCK`. Alimentado por **ps2** (set 1 + prefijo 0xE0 → KEY_*) y **USB HID** (usages → KEY_*, ratón por `usb_hid_emit`). El `input_push` original sigue alimentando al compositor nativo (hoy los eventos llegan a ambos; en modo ventana habrá que enrutar según el foco).
  - `open()` ahora propaga `O_NONBLOCK` a `ops->open` y a `F_GETFL` (antes se perdía).
  - **sysfs dinámico** (`kernel/sysfs_dyn.c`): registro de ficheros/symlinks reutilizable (lo usará DRM). `/sys/class/input/{input,event}N`, `/sys/devices/virtual/input/inputN/{name,phys,id/*,capabilities/*,uevent,eventN/{dev,uevent}}`, `/sys/dev/char/13:64`. Symlinks en componentes intermedios de `/sys` (el VFS solo seguía el último), `d_type=DT_LNK`, `O_PATH|O_NOFOLLOW` sobre symlinks, `readlinkat` con dirfd/ruta vacía, `faccessat`/`faccessat2` con dirfd, `name_to_handle_at` → `EOPNOTSUPP`.
  - `/run/udev/data/c13:64` etc. con `ID_INPUT_*`; `AF_NETLINK/NETLINK_KOBJECT_UEVENT` mínimo (`kernel/nl_uevent.c`, con `nl_uevent_broadcast()` listo para hot-plug).
  - `/dev/tty0`, `/dev/tty1` con `VT_*`/`KD*` (`kernel/vt.c`), estado coherente de un único VT activo.
  - **Hito:** los binarios de la distro **`libinput-list-devices` y `libinput-debug-events` (1.25) sin tocar** detectan teclado y ratón vía **libudev 255 real** y muestran eventos inyectados por QEMU (`sendkey`, `mouse_move`). `udev_test` (libudev) y `evdev_test` pasan.
  - Pendiente/observado: `libinput-debug-events` imprime `*** (-1)` como nombre de tecla (los eventos son correctos en `evdev_test`); investigar con Weston. `libseat` (backend builtin) se probará al llegar a Weston. `rseq` sigue en `-ENOSYS`.
- **Herramienta de depuración:** `echo strace=NOMBRE > /dev/kmsg` activa un `[STRACE]` por syscall (con rutas) para ese proceso; `strace=` lo apaga. Útil con `scripts/qemu-boot-test.sh`.
- `tests/wayland/run.sh` encadena ahora: unix_test, dev_test, timerfd_test, shm_test, wl_test, udev_test, evdev_test (con inyección QEMU). Suite del kernel: 179/179.
- **Bloque 4 (DRM/KMS core) hecho, salvo PRIME y kmscube:**
  - `kernel/drm/`: `drm_core.c` (nodos, ioctls, KMS, atomic, eventos, scanout), `drm_dumb.c` (GEM de páginas sueltas + dumb buffers), `drm_sysfs.c` (sysfs + udev). `drm_init()` se llama tras `evdev_init()`; `drm_udev_publish()` junto a `evdev_publish_udev()`.
  - Modelo: 1 conector virtual (id 3, `Virtual-1`), 1 encoder (2), 1 CRTC (1), 1 plano primario (4, XRGB8888/ARGB8888). Modos: el área de trabajo del compositor (preferido) + 1920x1080/1280x720/1024x768/800x600/640x480 si caben. Driver "aurora" 1.0.0.
  - Ioctls: VERSION, GET_UNIQUE, GET_MAGIC/AUTH_MAGIC (sin autenticación), SET_VERSION, GEM_CLOSE, GET_CAP, SET/DROP_MASTER, WAIT_VBLANK (con evento), GETRESOURCES/CRTC/ENCODER/CONNECTOR/PROPERTY/PROPBLOB, SETCRTC, ADDFB/ADDFB2/RMFB/DIRTYFB, PAGE_FLIP (+evento), GETPLANERESOURCES/GETPLANE/SETPLANE, OBJ_GETPROPERTIES, CREATE/DESTROY_PROPBLOB, ATOMIC (TEST_ONLY, PAGE_FLIP_EVENT, ALLOW_MODESET, NONBLOCK), CREATE/MAP/DESTROY_DUMB. Cursor/gamma/GETFB devuelven error (Weston cae a cursor por software).
  - Master: el primero que abre `card0` es master; `renderD128` solo admite GEM/dumb/caps (KMS → `-EACCES`).
  - mmap de dumb buffers: offset falso `id << 32` resuelto con `vfs_ops.get_page` (`pf.c` ahora admite `VFS_CHARDEVICE` con `get_page`). **Limitación:** un dumb con `MAP_DUMB` solicitado no libera sus páginas al hacer `DESTROY_DUMB`, sino al cerrar el fichero (un munmap no avisa al driver); Weston reconstruyendo buffers en bucle acumularía memoria hasta cerrar.
  - Throttle: un flip por tick (16 ms); un segundo flip antes del tick da `-EBUSY`, como Linux con flip pendiente. El evento se entrega en el siguiente tick (kthread `drm-vblank`, duerme si no hay trabajo).
  - Sysfs: el nodo cuelga de un dispositivo *platform* `aurora-drm` (`/sys/devices/platform/aurora-drm/drm/card0`): `libdrm` solo enumera (`drmGetDevices2`) nodos con padre `subsystem`+`uevent`.
  - Tests: `tests/wayland/drm_test.c` (cabeceras uapi reales; 40 comprobaciones, con modo `hold` para capturar pantalla con `screendump`), `udev_test` enumera `drm`/`card0` con tag `seat`, y las herramientas de la distro **`modetest` y `drm_info` sin tocar** (`scripts/fetch-wayland.sh` ahora las descarga). `tests/wayland/run.sh`: 10/10 PASS + kernel 179/179.
  - Observado/pendiente: (1) el compositor nativo de Aurora sigue consumiendo ratón/teclado mientras Weston es master; enrutar por foco al llegar al Bloque 5. (2) Si el usuario cierra la ventana de scanout fuera de DRM, `g_win` quedaría colgando (la ventana no tiene decoración ni botones, pero revisar atajos del Dock). (3) Sin EDID ni plano de cursor. (4) PRIME/dma-buf y `kmscube` dependen de Mesa/GBM.
- **Bloque 5.V (validación con Weston, no es el objetivo final; ver «OBJETIVO») — hito alcanzado en QEMU:** `weston 13` de Ubuntu sin tocar, `--backend=drm --renderer=pixman`, con `libseat` builtin, arranca, habilita `Virtual-1`, lanza `weston-desktop-shell`/`weston-keyboard` y corre clientes reales (`weston-simple-shm`, `weston-flower`, `weston-terminal`) con cierre limpio por SIGTERM. Reproducible con `scripts/fetch-weston.sh` + `tests/wayland/run-weston.sh` (captura en `build/weston_shot.png`; guest: `tests/wayland/guest/weston.sh`). Sin LLVM/llvmpipe todavía (solo pixman). `foot` está descargado pero **sin probar**.
  - Arreglos de compatibilidad que hizo falta en el kernel: (1) `mmap(MAP_SHARED, PROT_READ)` de ficheros sin `get_page` se trata como privado (xkbcommon); (2) `fallocate(285)` (modo 0 y KEEP_SIZE); (3) el asignador de `mmap` sin hint saltaba mal los VMAs existentes y devolvía `-ENOMEM` (ahora busca hueco libre); (4) **las máscaras de señales usaban el bit `sig` y Linux usa `sig-1`**: se añadió `sigset_from_user/to_user` en `signal.h` y se aplicó a sigprocmask, sigaction, sigreturn/ucontext, signalfd, epoll_pwait y /proc. Antes SIGTERM/SIGINT de bash y Weston se bloqueaban o trastocaban otras señales.
  - Faltan `rt_sigsuspend`, `rt_sigpending`, `rt_sigtimedwait`; `rseq` sigue en `-ENOSYS`.
  - **Intermitencias sin resolver:** en 3 de ~15 ejecuciones de `scripts/qemu-boot-test.sh` el hook `/system/boot-tests` no ejecutó las órdenes (init arrancó el terminal directamente) y en otra el script se quedó tras `READY-FOR-SHOT`. No las he reproducido con órdenes cortas (4/4 ok); sospecho de una carrera en el arranque o de la lectura de ficheros de un initrd ya de ~300 MB. Revisar antes de fiarse de los resultados de un solo run.
  - Pendiente del bloque: fuentes/fontconfig (`/etc` es tmpfs → `FONTCONFIG_FILE` apuntando a `/usr/share`), tema de cursores, `foot`, teclado/ratón reales dentro de Weston (hoy el compositor nativo sigue consumiendo input), clipboard.
