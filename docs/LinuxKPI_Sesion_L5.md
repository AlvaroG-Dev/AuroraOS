# LinuxKPI: sesión del 27/28-oct-2026 (fase L5: K7 y K9-drm)

Registro de la fase L5: `dma_fence`/`dma_resv`/`sync_file`/`drm_syncobj` de Linux sobre la capa (K7) y el DRM/KMS de Linux con los primeros drivers reales en QEMU (K9-drm). El plan es `Plan_LinuxKPI.md` (decisiones con fecha en su §11, entradas «27-oct-2026 — L5» y «28-oct-2026 — L5»); sesiones anteriores: `docs/LinuxKPI_Sesion_L0_L1.md` a `docs/LinuxKPI_Sesion_L4.md`.

Estado: **L5 terminada** (rama `claude/project-thread-wy7kwn`, sin PR ni merge a `main`). **L6 no empezada.**

## 1. Qué se hizo

| Pieza | Resultado |
|---|---|
| K7 | `dma_fence`, `dma_fence_chain`, `dma_fence_array`, `dma_resv`, `sync_file` y `drm_syncobj` compilados de Linux 6.18.59 con parches mínimos; `aur_fence` en Aurora envuelto en los dos sentidos; el fence implícito de los dmabuf de Aurora vive en el `dma_resv` de la capa. Test `lkpi_test_k7`. |
| `lkpi_virtio.kur` | Núcleo virtio de Linux (bus, virtqueues, `virtio-pci` moderno y legado) con cabeceras propias. Test `lkpi_test_virtio` con `virtio-rng`. |
| `lkpi_drm.kur` | Núcleo DRM/KMS de Linux casi entero (lista en `kernel/linuxkpi/drivers/lkpi_drm.srcs`), fbdev desactivado. Dos parches (3 líneas). `glue.c`: scanout, vblank por software, cuña de ABI. |
| Drivers | `lkpi_bochs.kur` (`drivers/gpu/drm/tiny/bochs.c`), `lkpi_simpledrm.kur` (GOP) y `lkpi_virtio_gpu.kur` (2D, con KMS). |
| Aurora | `kernel/drm/drm_linux_scanout.c` (ventana de scanout por CRTC), relevo de backend (`drm_backend_claim/release`), `kernel/lkpi_boot.c` (`lkpi.load=`), `aur_bus_launch_skipped` (4B), uevents por netlink, `DRM_BACKEND`. |
| Pruebas | `tests/linuxkpi/lkpi_test_drm` (suite), `drm_test` (multi-CRTC y ABI de Linux), hito K7 en `lkpi_test` (108 comprobaciones). |

## 2. Cómo arrancar el DRM de Linux

`cmdline=` de `aurora.conf` (o `--cmdline` de `scripts/qemu-drive.py`):

```
bochs       aur.skip=1234:1111 lkpi.load=linuxkpi,lkpi_drm,lkpi_bochs            (QEMU -vga std)
simpledrm   lkpi.load=linuxkpi,lkpi_drm,lkpi_simpledrm
virtio-gpu  aur.skip=1af4:1050 lkpi.load=linuxkpi,lkpi_virtio,lkpi_drm,lkpi_virtio_gpu
            (QEMU -vga none -device virtio-gpu-pci[,max_outputs=2])
```

`drm.backend=native` en la cmdline (o `make DRM_BACKEND=native`) omite los módulos DRM de Linux. `lkpi.drm_quirks=0` desactiva la cuña de ABI. Con `modetest` hay que dar el nombre del driver: `modetest -M bochs-drm -c -p`.

## 3. Resultados

Todas las pruebas con `-m 6G`, un QEMU a la vez.

| Prueba | Configuración | Resultado |
|---|---|---|
| `drm_test` | bochs, `-smp 4` | ALL PASS (83) |
| `drm_test` | simpledrm, `-smp 2` y (Weston) `-smp 4` | ALL PASS (83) |
| `drm_test` | virtio-gpu, `-smp 4` | ALL PASS (83) |
| `drm_test` | virtio-gpu `max_outputs=2`, `-smp 2` y `-smp 4` | ALL PASS (86, con el segundo CRTC) |
| `drm_test` | bochs con uevent por netlink | ALL PASS (incluye el evento `change` de `card0`) |
| `drm_test` | `drm.backend=native` | ALL PASS (uevent omitido: el nativo no los emite) |
| Weston 13 (`weston.sh`: `simple-shm`, `flower`, `terminal`) | bochs `-smp 2`, simpledrm `-smp 4`, virtio-gpu 2 salidas | salida habilitada, todos los clientes `exit 0`, cierre limpio |
| `aurora-wayland` | bochs | servidor listo, 3 ventanas, `exit 0` |
| `modetest -M bochs-drm -c -p` | bochs | lista conector, 21 modos, planos y propiedades |
| `lkpi_test` | suite normal | 108 comprobaciones (101 + 7 del fence implícito), 0 omitidas |

Regresión con la configuración normal (sin `lkpi.load`) tras todos los cambios:

| Prueba | Resultado |
|---|---|
| Suite del kernel `-smp 2` | 204 pasadas, 0 fallos, 16 omitidas |
| Suite del kernel `-smp 4` | 204 pasadas, 0 fallos, 16 omitidas |
| `drm_test` (backend nativo) | ALL PASS (200 líneas PASS en el log, incluye las de la suite) |
| `sig_test` / `rseq_test` / `timer_test` / `thread_sig_test` | ALL PASS |
| `clipboard-x11.sh` | Wayland→X11 y X11→Wayland correctos |
| `primary-icons.sh` | PRIMARY en los dos sentidos, selecciones separadas, icono simbólico |

## 4. Problemas encontrados y arreglos

1. **Toy test**: `drm_framebuffer_init` con `funcs` NULL daba `#UD`; el test usa `drm_gem_fb_destroy`/`create_handle`.
2. **Enlace**: `kernel/Makefile` no tenía los `.o` nuevos; faltaban `EINVAL`/`ENOMEM` (incluir `uaccess.h`).
3. **bochs**: «ID mismatch» porque el BAR2 en sombra se copiaba con `memcpy_fromio`; los registros `dispi` son de 16 bits.
4. **Cabeceras/CONFIG**: `S_IRUGO` (nuevo `stat.h`), `CONFIG_PM_SLEEP`, `CONFIG_APERTURE_HELPERS`, prototipo de `devm_memremap`.
5. **Fence implícito**: el `fd_install` de `drm_prime` instalaba el fichero de la capa; `lkpi_dma_buf_fd_install` lo convierte en nodo dmabuf de Aurora (si no, `EXPORT_SYNC_FILE` daba `ENOTTY`).
6. **virtio-gpu «KMS disabled»**: faltaba `CONFIG_DRM_VIRTIO_GPU_KMS=1`.
7. **Weston `/dev/card0`**: `DEVNAME` del `uevent` ignoraba `devnode()`; ahora sale `dri/card0`.
8. **`drm_test`**: asumía 1 CRTC (con 2 se desbordaba el buffer de ids) y la ABI nativa de syncobj; ahora acepta N objetos y la ABI de Linux (ver §3 del plan, entrada 8).

## 5. Pendiente / fuera de alcance

- **`kmscube`**: no hay Mesa en `precompiled/`; se probará con la pila gráfica 3D.
- Cursor de `virtio-gpu` no compuesto en la ventana de scanout.
- Hot-plug físico de monitores (QEMU no lo genera): solo se probó el reenvío de uevents.
- Que el syncobj nativo adopte la ABI de Linux en `IMPORT_SYNC_FILE` (handle existente).
- Compositor multi-monitor y modo dinámico en Aurora (D1/D2 de usuario): el kernel con el backend Linux ya los permite, falta el compositor.
- **L6** (`ttm`, scheduler de GPU, memoria de dispositivo) no se ha empezado.

## 6. Commits (rama `claude/project-thread-wy7kwn`)

```
8771b7dc Plan §11: decisiones de diseño de L5
94b9b795 Aurora: /dev/kmsg con secuencias absolutas; poll filtra revents
16a4432a LinuxKPI L5/K7: dma_fence, chain, array, dma_resv y sync_file
dd77dceb LinuxKPI L5: núcleo virtio (lkpi_virtio.kur)
3bdba332 LinuxKPI L5: núcleo DRM/KMS (lkpi_drm.kur)
dd46cbc5 LinuxKPI L5: backend DRM de Linux (bochs, simpledrm, virtio-gpu), display-shadow, lkpi.load
287a3370 LinuxKPI L5: virtio-gpu con KMS, drm_test multi-CRTC y ABI de Linux para syncobj
9477a941 LinuxKPI L5: DEVNAME del uevent usa devnode()
9a801235 LinuxKPI L5: uevents por netlink, DRM_BACKEND=native
d7fe0e5c LinuxKPI L5: hito K7 en lkpi_test (108 comprobaciones)
```
