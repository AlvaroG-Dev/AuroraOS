# Evaluación: ¿se puede jugar a juegos Linux en AuroraOS?

Fecha: 2026-11-01. Es un análisis de huecos hecho **leyendo el repositorio** (rama `main`, `086b118b`); no se ha ejecutado ningún juego ni se ha arrancado el kernel para escribirlo. Lo que sale de documentación del proyecto va marcado como *consta*; lo que es una deducción mía o de conocimiento general sobre los juegos va marcado como *deducción* y hay que verificarlo con una prueba.

## 1. Respuesta corta

- **Hoy no se puede jugar a nada «de verdad».** La parte gráfica ya está muy cerca (OpenGL ES/EGL y Vulkan aceleran en la RTX 4060, *consta* en `Plan_NVIDIA.md`), pero faltan **audio (no existe ningún driver ni capa ALSA)**, **SDL2 en la imagen**, **mando/gamepad**, **rendimiento de presentación** (cada fotograma se copia por CPU) y una prueba real con un juego.
- **Hoy sí se puede** (según lo documentado): abrir ventanas Wayland/X11 con GTK3/Qt5, correr `weston-simple-egl` en la 4060 con 3D real, enumerar la 4060 con Vulkan y enviar trabajo a la GPU, y ejecutar binarios glibc dinámicos con hilos, futex, epoll, memfd, AF_UNIX y rseq.
- **Rocket League no es un objetivo realista**, ni a medio plazo ni en Linux real: Psyonix retiró el cliente nativo de Linux en 2020 (la versión Windows se ejecuta en Linux solo con Wine/Proton) y usa Easy Anti-Cheat. Hacerlo funcionar exigiría Steam/Epic, Wine/Proton, binarios de 32 bits, DXVK sobre Vulkan y un anti-cheat que depende de integraciones con el kernel de Linux real. Descartarlo como meta.
- **Primer juego razonable**: un port de Doom/Quake con SDL2 (para validar el camino) y después **SuperTuxKart** o **Xonotic** (OpenGL 3.x, libres, sin anti-cheat, sin Steam). Orden de hitos en §6.

## 2. Qué tiene AuroraOS hoy (relevante para juegos)

| Área | Estado | Fuente |
|---|---|---|
| ABI Linux x86-64 + glibc dinámica | Funciona (bash, Python, Perl, Git, GTK3, Qt5, Xwayland) | `glibc_port.md`, `ROADMAP.md` |
| Hilos | `clone`/`clone3`, futex WAIT/WAKE (+BITSET), `set_robust_list`, `rseq` real, `membarrier`, `sched_{get,set}affinity` | `kernel/syscall.c`, `kernel/futex.c`, `kernel/rseq.c` |
| E/S asíncrona | epoll (ET/ONESHOT), eventfd, signalfd, timerfd, inotify (**stub**: nunca emite eventos), pidfd_open | `Plan_Wayland_Unificado.md` §2.3 |
| Memoria | `mmap`/`mremap`/`madvise`, memfd con `MAP_SHARED` real, swap (`swapon`) | `kernel/memfd.c`, `kernel/swap.c` |
| Sockets | AF_UNIX completo (SCM_RIGHTS, PEERCRED), AF_INET con TCP/UDP/DHCP, driver e1000e, loopback | `kernel/unix_socket.c`, `kernel/net/` |
| Gráficos | DRM/KMS, dumb buffers, PRIME/dmabuf, syncobj, `linux-dmabuf` en el compositor | `kernel/drm/`, `Plan_NVIDIA.md` |
| 3D en la 4060 | EGL/GBM + egl-wayland2: `weston-simple-egl` animado (N5a). Vulkan: loader 1.3.275 + ICD NVIDIA, `vkCmdFillBuffer` en GPU, swapchain Wayland con 3 fotogramas presentados (N5b) | `Plan_NVIDIA.md` |
| 3D en Intel | Mesa crocus en la Portégé (HD 4600), dmabuf verificado | `docs/LinuxKPI_Sesion_L7.md` |
| Wayland/X11 | `aurora-wayland` propio, `xdg_shell`, subsuperficies, cursor, portapapeles, Xwayland funcional | `docs/Wayland_Estado_y_Decisiones.md` |
| Entrada | evdev para teclado y ratón (PS/2 y USB HID), `/sys/class/input`, udev mínimo, libinput 1.25 sin tocar | `kernel/evdev.c`, `Plan_Wayland_Unificado.md` §3 |
| Sysroot (glibc) | libwayland, xkbcommon, libdrm, libinput, libudev, libdbus, libX11, libXrandr, Mesa (GL, GLES2, EGL, GLX), libstdc++, libssl, libcurl, freetype, libpng | `libs/glibc/lib/x86_64-linux-gnu/` |

## 3. Huecos frente a un juego nativo Linux

Prioridad: **B** = bloquea cualquier juego; **A** = bloquea la mayoría; **M** = mejora/solo algunos.

| # | Hueco | Prio | Detalle |
|---|---|---|---|
| H1 | **Audio inexistente** | A | No hay driver de sonido (`drivers/aur`: ahci, bochs, e1000e, ps2, virtio_gpu, xhci; ningún HDA/USB-audio/virtio-snd) ni nodos `/dev/snd/*`, ni libasound/PulseAudio/PipeWire en el sysroot. Un juego puede arrancar en silencio con `SDL_AUDIODRIVER=dummy`, pero no es jugable «de verdad». Camino: driver Intel HDA (en la 4060 el audio HDMI cuelga de la propia GPU, y en el sobremesa hay códec en placa) como `.aur` o reutilizando `snd-hda-intel` con LinuxKPI, más ALSA userspace (o un servidor propio con ABI PulseAudio/PipeWire-compat) |
| H2 | **SDL2 no está en el sysroot** | B | Casi todos los juegos nativos usan SDL2 (o GLFW). El plan ya prevé `SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR=0` en `init`, pero no hay `libSDL2-2.0.so` en `libs/glibc` (revisado). Hay que añadirla a `scripts/fetch-*.sh` (Ubuntu, como las demás) y probar el backend Wayland (EGL) y el X11 (vía Xwayland) |
| H3 | **Rendimiento de presentación** | A | El compositor lee los dmabufs de VRAM por EGL + `glReadPixels` y los copia por CPU (*consta*, memoria N5a y `Plan_NVIDIA.md`): «lento a pantalla grande». A 1080p/60 Hz es el cuello de botella nº 1. Soluciones: componer con GL en el propio compositor (importar el dmabuf como textura EGL) o *direct scanout* del buffer a pantalla completa. Necesita además explicit sync (`wp_linux_drm_syncobj_v1`); hoy se fuerza `__NV_DISABLE_EXPLICIT_SYNC=1` |
| H4 | **Vsync real y pantalla completa** | A | `wp_presentation` existe pero es ficticio: la «presentación» se informa en el `commit`, no en el vblank (`aurora-wayland.c`, ~l.2902), así que los juegos no pueden sincronizar con la pantalla (tearing o ritmo irregular). Cambio de resolución/modo, multi-monitor y pantalla completa real están aplazados |
| H5 | **Mandos/gamepad** | A (para la mayoría de juegos de coches/plataformas; M si basta teclado) | `evdev.c` solo publica teclado y ratón. El parser HID existe (`kernel/usb/hid_parser.c`) y xhci funciona, pero falta exponer mandos como `/dev/input/event*` con `EV_ABS`/`BTN_GAMEPAD`, `ID_INPUT_JOYSTICK` en `/run/udev/data` y (opcional) `/dev/input/js*`. SDL2 lee evdev + udev, así que es viable (*deducción*) |
| H6 | **Ratón relativo y captura** | A | Los juegos 3D usan `zwp_relative_pointer_v1` y `zwp_pointer_constraints_v1` (SDL los usa en modo relativo). No aparecen en `aurora-wayland.c` (búsqueda sin resultados), así que el ratón de un juego en primera persona no funcionaría. `zwp_idle_inhibit` sí está aceptado |
| H7 | **Syscalls ausentes en la tabla** | M | No aparecen por nombre en `linux_table`: `preadv/pwritev`, `msync`, `mincore`, `mlock*`, SysV IPC (`shmget/shmat/semget`, lo usa MIT-SHM de X11), `splice`, `copy_file_range`, `sched_setscheduler/setparam`, `ptrace`, `io_uring`, `futex_waitv`, `unshare/setns/mount`. Los primeros (preadv/pwritev, mlock, sched_setscheduler, SysV shm) son los que más veces aparecen en motores de juego; ninguno hay que implementarlo sin reproducir el fallo (`[SYSCALL-GAP]` en el log, como pide `glibc_port.md`). Comprobar antes que no estén bajo un número distinto en la tabla |
| H8 | **Memoria y arranque** | A | `QEMU_MEM` por defecto es 2 GB y con Mesa/LLVM el initrd supera 500 MB (el Makefile pide 3 GB). Un juego de 1–4 GB de datos no cabe en initrd: hace falta **instalarlo en disco** (FAT32 RW funciona; falta confirmar rendimiento y límites de tamaño de fichero >4 GB, y que haya un ext4 o similar si se quiere) y probar la presión de memoria (swap) |
| H9 | **Hilos y señales bajo carga** | M | *Consta* que `clone3` rechaza algunos flags y que faltan pruebas de estrés de futex/hilos. Los motores crean decenas de hilos y usan `pthread_setaffinity`, `SCHED_*` y prioridades; se verá en cuanto se ejecute un juego real |
| H10 | **Tiempo, relojes y E/S de disco** | M | `clock_gettime`/vDSO están, pero medir el coste: los juegos llaman miles de veces por fotograma. Comprobar `CLOCK_MONOTONIC_RAW`, `timerfd` con 1 ms de resolución (*consta*: «resolución 1 ms») y `sched_yield` |
| H11 | **Red** | M | Para solo-un-jugador no hace falta. Para multijugador: DNS, TLS (libssl está) y NIC. Hoy solo `e1000e`; el PC con la 4060 y la mayoría de placas modernas usan Realtek 2.5G (`r8169`/`r8125`) o Intel `igc`, no cubiertos; Wi-Fi no existe |
| H12 | **Xwayland para juegos X11/GLX** | M | Xwayland funciona para apps 2D. Para GLX acelerado (`glxgears`) falta DRI3/dmabuf con NVIDIA; está listado como pendiente. Es la vía de los juegos que no tengan backend Wayland |

### Lo que NO hace falta para los candidatos libres
- **32 bits (IA-32)**: no hay `int 0x80`/compat32 en el kernel (búsqueda sin resultados). Solo importa para Steam/Proton y juegos antiguos; los candidatos de §5 existen en 64 bits.
- **Anti-cheat, Steam, DRM de pago**: ninguno de los candidatos los usa.

## 4. Steam, Proton y Rocket League

| Requisito | Estado en AuroraOS | Comentario |
|---|---|---|
| Cliente de Steam | **Inviable hoy** | Es un binario de 32 bits que descarga un runtime con bibliotecas de 32 bits, usa CEF (Chromium) para la interfaz, `bwrap`/user namespaces para *Pressure Vessel*, D-Bus, udev, y muchos `ioctl`/`prctl`. Faltan IA-32, `unshare/setns/mount`, `seccomp`, espacios de nombres |
| Runtime de contenedor (Pressure Vessel/Soldier/Sniper) | Ausente | Necesita user namespaces y montajes |
| Proton/Wine + DXVK/VKD3D | Ausente | Pide 32 bits para muchos juegos, `futex_waitv`/ntsync, `mmap` de rangos fijos muy grandes, Vulkan 1.3 completo con muchas extensiones (la ICD de NVIDIA las ofrece, pero hay que probar con pipelines reales: hoy solo se probó `vkCmdFillBuffer` y swapchain) |
| Easy Anti-Cheat | **Imposible a efectos prácticos** | Requiere que el juego esté habilitado para Linux por el editor y valida el entorno (kernel, procesos, ptrace/`/proc`). En un kernel que no es Linux no se puede dar por válido. Rocket League además ya no ofrece versión Linux |
| Cuenta/licencia y red | N/A | Los juegos de Steam/Epic requieren login, TLS y ancho de banda |

Conclusión: **Steam y Proton son un hito de largo plazo** (después de que SuperTuxKart/Xonotic funcionen) y probablemente se descartarían para anti-cheats como el de Rocket League.

## 5. Candidatos realistas (en orden de menor a mayor coste)

| # | Candidato | Por qué | Requisitos principales |
|---|---|---|---|
| C0 | `es2gears` / `glxgears` / `vkcube` / test SDL2 con ventana | Validan el camino sin juego | EGL/GLES2 (ya), SDL2 (H2), SPIR-V embebido para vkcube (*consta* pendiente) |
| C1 | **Chocolate Doom / PrBoom+** (SDL2, render por software) | Sin GPU, sin audio obligatorio; valida SDL2 + Wayland `wl_shm` + teclado + temporizadores | H2 (SDL2). Audio con `SDL_AUDIODRIVER=dummy` |
| C2 | **Yamagi Quake II / ioquake3** (SDL2 + OpenGL 1.x/2.x) | Primer juego 3D acelerado de verdad, ratón relativo, pantalla completa | H2, H6, H3 |
| C3 | **Xonotic** (SDL2 + OpenGL 2.1/3.x, ~1 GB) | Shooter completo y libre, buen *benchmark*; trae su propio binario `xonotic-sdl` | C2 + datos en disco (H8) y audio (H1) |
| C4 | **SuperTuxKart** (OpenGL 3.3/GLES3, SDL2/OpenAL, ~1 GB) | El «Rocket League» más cercano: coches y físicas; usa Bullet, hilos y OpenAL | C3 + OpenAL (sobre H1) + mando (H5) |
| C5 | **0 A.D.** (SDL2, OpenGL, SpiderMonkey, ~3 GB) | Estrés de CPU, memoria y E/S de disco | Todo lo anterior + RAM suficiente |
| C6 | Juegos de Steam nativos (p. ej. Left 4 Dead 2, 64 bits ya) | Requiere el cliente y su runtime | §4 |

Nota sobre GPUs: la **RTX 4060** es el mejor caso (GL/EGL y Vulkan ya van); la **Portégé con HD 4600** (Haswell) soporta OpenGL 4.0 vía crocus pero es muy lenta para C3/C4. Mesa llvmpipe (software) permite C1 y quizá C2 en QEMU, pero no más.

## 6. Hitos propuestos (G0–G7)

Cada hito termina con una prueba verificable. No se avanza al siguiente sin ella.

| Hito | Contenido | Prueba / criterio |
|---|---|---|
| **G0** Base de pruebas | Añadir `scripts/fetch-sdl2.sh` (SDL2 de Ubuntu, sin tocar) al sysroot y regenerar `ld.so.cache`. Test mínimo: ventana con rectángulo que cambia con el teclado y con medición de FPS (software, `wl_shm`) | `sdl-test` abre ventana, recibe teclado/ratón, imprime fps, rc=0 |
| **G1** SDL2 + OpenGL | `SDL_VIDEODRIVER=wayland` con contexto EGL/GL: llvmpipe en QEMU y NVIDIA (`nvwl.sh`) en la 4060. Medir fps con 1080p | `es2gears`/test SDL GL ≥ 30 fps en la 4060 (hoy esperable: lento por copia CPU) |
| **G2** Doom (C1) | Chocolate Doom con WAD libre (Freedoom). Audio `dummy` | Se juega 5 min sin cuelgues; log sin `[SYSCALL-GAP]` nuevos |
| **G3** Presentación rápida | Quitar la copia por CPU: textura EGL del dmabuf en el compositor, explicit sync, vblank real para `wp_presentation`, pantalla completa. `zwp_relative_pointer` + `pointer_constraints` | 1080p ≥ 60 fps sostenidos en la 4060 con `vkcube`/`es2gears`; ratón relativo en test SDL |
| **G4** Quake II / ioquake3 (C2) | Datos en disco (no initrd), vsync, ratón relativo | Partida en solitario con ratón y teclado |
| **G5** Audio | Driver de sonido (HDA primero; en QEMU `-device intel-hda -device hda-duplex`), nodos `/dev/snd`, ALSA userspace (libasound + `alsa-lib` plugins) o shim PulseAudio, SDL audio. Sin audio no hay C3/C4 | `speaker-test`/`aplay` suena en QEMU; luego en la 4060 (HDMI) o en la placa |
| **G6** Mando | HID gamepad → evdev (`BTN_GAMEPAD`, `ABS_*`), udev `ID_INPUT_JOYSTICK`, hot-plug (uevent) | `evtest` y `SDL_GameController` ven el mando; se juega a SuperTuxKart con él |
| **G7** Juegos completos | Xonotic (C3), SuperTuxKart (C4), después 0 A.D. (C5) | Una partida completa con audio, ratón/mando y ≥ 60 fps en la 4060; estabilidad 30 min |
| **G8** (opcional, largo plazo) | IA-32 + user namespaces + `futex_waitv` + `seccomp`/mount para Steam/Proton | Fuera de alcance salvo decisión expresa de Alvaro |

Estimación de esfuerzo **relativo** (sin fechas): G0–G2 pequeño; G3 medio-grande (toca compositor y NVIDIA); G4 pequeño-medio; G5 grande (driver + ALSA); G6 medio; G7 pequeño si lo anterior está sólido; G8 muy grande.

## 7. Riesgos y notas

- **Cada prueba en hardware real cuesta a Alvaro**: probar antes en QEMU lo máximo posible (llvmpipe, `intel-hda`, `-usb -device usb-host`/gamepad virtual) y dejar logs útiles por ciclo. NVIDIA no se puede emular en QEMU: G1 y G3 requieren el PC real.
- **Licencias**: SuperTuxKart (GPLv3), Xonotic (GPLv3), 0 A.D. (GPLv2, arte CC), Freedoom (BSD): redistribuibles. Los datos se bajan en el script de descarga, no se versionan (como Mesa).
- **Estabilidad previa**: los 5 tests de `pmm` fallan (no relacionados con NVIDIA, *consta* en memoria del proyecto) y Xwayland tuvo una carrera ya corregida: conviene repetir pruebas largas antes de culpar al juego.
- **Qué NO se sabe aún** (verificar con una prueba): si Vulkan soporta pipelines/shaders reales en la 4060 (solo `vkCmdFillBuffer` + swapchain); si SDL2 de Ubuntu funciona con egl-wayland2 sin ajustes; el rendimiento real de la copia por CPU a 1080p; cuántos de los syscalls de H7 se llegan a usar.

## 8. Decisiones que necesito de Alvaro

1. ¿Se empieza por **G0–G2** (SDL2 + Doom, bajo riesgo, se prueba también en QEMU) o por **G3** (rendimiento de la 4060)? *Recomendación: G0–G2 primero*, porque dan un juego jugable y exponen los huecos reales (H7) antes de invertir en el compositor.
2. ¿Audio por **HDA nativo** (`.aur` propio) o **vía LinuxKPI** (`snd-hda-intel` real)? La LinuxKPI ya existe (L0–L7), pero no se debe abrir L8/L9 sin su confirmación.
3. ¿Se descarta formalmente Rocket League/Steam (G8) o se deja como meta a largo plazo?
