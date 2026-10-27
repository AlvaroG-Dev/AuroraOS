# Prompt para la siguiente sesión (L5 de LinuxKPI)

Texto para pegar en una sesión nueva de Claude Code. Antes, preparar el contenedor con `scripts/claude-env-setup.sh` (como «Setup script» del entorno o a mano dentro del repositorio).

---

Continúa el plan LinuxKPI de AuroraOS con la fase L5 (K7 y K9-drm: DRM/KMS de Linux sobre la capa y los primeros drivers reales en QEMU). Trabaja en la rama claude/beautiful-newton-1neyw4 (crea una nueva desde ella si la plataforma te asigna otra), con commits pequeños y push. No abras PR ni hagas merge a main salvo que te lo pida. Para al terminar L5 e informa; no empieces L6 sin confirmación.

Lee antes de programar:
- Plan_LinuxKPI.md (sobre todo §0 principios y alcance, §2 y la decisión DRM «A» de §11 del 26-oct-2026, la sección de L5, §10 decisiones abiertas y §11 registro). Ojo: la sección de L5 todavía dice «opción B, compilar drm.ko», pero lo decidido es la opción A (kernel/drm propio + shim drm_* en la capa); si ves otra contradicción, señálala en la ronda de preguntas.
- docs/LinuxKPI_Sesion_L0_L1.md, docs/LinuxKPI_Sesion_L2.md, docs/LinuxKPI_Sesion_L3.md y docs/LinuxKPI_Sesion_L4.md (en especial §3, §5, §7 y §9 de L4: lo que quedó pendiente y las notas de entorno).
- docs/linuxkpi_api.md (símbolos K7 y K9-drm que usan bochs, simpledrm y virtio-gpu) y docs/Wayland_Estado_y_Decisiones.md (D1, D2, D5, D6, D10).
- kernel/drm/ (DRM propio de Aurora: un CRTC virtual, GEM dumb, PRIME, syncobj, sync_file) y tests/wayland/drm_test.c.

Alcance de L5 según el plan (con la opción A):
- K7: dma_fence (+ chain/array), dma_resv completo (fences de lectura/escritura, iteradores, wait), sync_file y drm_syncobj enlazados con los de Aurora; el fence implícito de los dmabuf de Aurora conectado con el dma_resv de los dma_buf de la capa (pendiente de L4).
- K9-drm: el shim de include/drm/* sobre kernel/drm, lo que necesiten bochs, simpledrm y virtio-gpu (2D): drm_device/drm_driver, modo y propiedades, atomic helpers, GEM (shmem helper, VRAM helper si bochs lo pide), PRIME, fbdev desactivado, drm_gem_* y los ioctls que ven Mesa y los compositores, con las mismas semánticas que hoy.
- Generalizar kernel/drm para más de un CRTC/conector si el shim lo exige (D1/D2: decide con el autor hasta dónde llega en L5).
- aperture y vgaarb (los 6 símbolos K4/K5 que faltaban), el núcleo virtio (drivers/virtio desde las fuentes de Linux 6.18.59) y uevents de hot-plug hacia el userland.
- Arranque: aur.skip=1234:1111,1af4:1050 en cmdline= de aurora.conf para que bochs.aur/virtio_gpu.aur no reclamen los dispositivos; GOP como respaldo si el driver de Linux falla (el arranque nunca depende de la capa).
- Hito: drm_test, modetest, kmscube (si hay Mesa), Weston y aurora-wayland pasan sobre bochs, simpledrm y virtio-gpu de Linux en QEMU igual que hoy, con /dev/dri/card0 y renderD128 servidos por el driver de Linux. El kernel/drm propio sigue disponible con una opción de compilación.

Haz todas las preguntas de diseño en una sola ronda antes de programar, con tu recomendación en cada una (entre otras: hasta dónde llega el shim frente a reescribir partes de kernel/drm; cómo conviven los .aur gráficos y los drivers de Linux; multi-CRTC en L5 o después; cómo se compila y parchea el código de Linux de drivers/gpu/drm y drivers/virtio).

Reglas que se mantienen:
- Licencia GPL-2.0-only; cabeceras propias, sin copiar las de Linux; conserva el aviso de FreeBSD si usas su código. El código de drivers/ de Linux se compila tal cual desde build/linuxkpi_src (no se versiona); los parches mínimos van versionados aparte.
- Todo lo que exporta linuxkpi.kur lleva prefijo lkpi_; las cabeceras dan el nombre de Linux como static inline o macro (si choca con un miembro de struct, como vmap/vunmap, static inline).
- La capa no incluye cabeceras de Aurora: cada servicio se declara en kernel/linuxkpi/aurora.h (aurora_* con etiqueta asm) y se exporta en kernel/ksymtab_exports.c con la misma firma y _Static_assert de las estructuras replicadas. task_t es opaco para la capa.
- Una prueba .kur por nivel en tests/linuxkpi/<nombre>/ con drv.mk, KT_ASSERT y REGISTER_TEST_FLAGS(..., TEST_FLAG_BLOCKING); las de varias CPU, también TEST_FLAG_NEEDS_SMP. El .c de la prueba no puede llamarse como el directorio (choca con el objeto enlazado).
- Nada de arrays grandes en la pila de module_init; klog_vprintf no añade salto de línea; la memoria que pase por sg/DMA no puede estar en la pila.
- No toques drivers de GPU reales (i915, xe, amdgpu, nvidia) ni escribas código de pantalla de Intel/AMD/NVIDIA: L5 es solo bochs, simpledrm y virtio-gpu.
- Cada decisión de diseño va fechada a Plan_LinuxKPI.md §11; actualiza docs/Wayland_Estado_y_Decisiones.md si afecta a D1/D2/D5/D6/D10.
- No cambies las versiones fijadas (Linux 6.18.59, linux-firmware 20261019, NVIDIA open 615.78.08) sin hablarlo.
- Un solo QEMU a la vez, con -m 6G. Usa scripts/qemu-drive.py en segundo plano, sin cadenas largas de sleep; suite del kernel con --done "passed, "; -smp 4 con --extra=-smp --extra=4. qemu-drive.py recompila la ISO desde el árbol en cada arranque: para compilaciones con trazas temporales, haz la ISO aparte y arranca QEMU a mano. Espera por PID, nunca con pgrep -f/pkill -f usando un patrón que aparezca en tu propia orden. No edites fuentes mientras qemu-drive.py compila.
- Las pruebas de usuario necesitan /usr/bin/musl-gcc; los binarios se suben junto al fuente en user/musl/<app>. El hito de L4 se repite con --boot-test /usr/bin/lkpi_test --done "LKPI-TEST:" (espera solo a la suite).
- task_t debe seguir en 2048 bytes o menos (_Static_assert); si cambian on_cpu o fs_base, actualiza kernel/switch.asm.
- git add con rutas explícitas, nunca -a.
- No rompas la suite: kernel 201/201 (también con -smp 4), lkpi_test (101), drm_test, sig_test, rseq_test, timer_test, thread_sig_test, clipboard-x11.sh y primary-icons.sh (este último con -t 600; hoy pasa en ~135 s; si se cuelga, investígalo, no lo ignores).
- Si encuentras fallos de Aurora por el camino, arréglalos si son pequeños y del alcance; si no, apúntalos y pregunta. Pendientes conocidos de Aurora (L4 §3): poll no filtra revents y /dev/kmsg se salta líneas y se alimenta a sí mismo con un DEBUG por lectura.

Al terminar, crea docs/LinuxKPI_Sesion_L5.md (qué se hizo, decisiones, pruebas, commits, regresión, pendientes), actualiza la línea de estado de Plan_LinuxKPI.md y dame el informe final.
