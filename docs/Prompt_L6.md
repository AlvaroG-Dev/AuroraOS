# Prompt para la siguiente sesión (L6 de LinuxKPI)

Texto para pegar en una sesión nueva de Claude Code. Antes, preparar el contenedor con `scripts/claude-env-setup.sh` (como «Setup script» del entorno o a mano dentro del repositorio).

---

Continúa el plan LinuxKPI de AuroraOS con la fase L6 (K9: `ttm`, scheduler de GPU y memoria de dispositivo). Trabaja en la rama que te asigne la plataforma; si es nueva, créala desde `claude/project-thread-wy7kwn` (tiene L0–L5). Commits pequeños y push. No abras PR ni hagas merge a main salvo que te lo pida. Para al terminar L6 e informa; no empieces L7 sin confirmación.

Lee antes de programar:
- Plan_LinuxKPI.md (§0 principios, la sección de L6, §10 y todo §11; sobre todo las entradas de L5 del 27 y 28-oct-2026).
- docs/LinuxKPI_Sesion_L0_L1.md … docs/LinuxKPI_Sesion_L5.md (L5: cómo se arranca el DRM de Linux, problemas encontrados, pendientes).
- docs/linuxkpi_api.md (símbolos de ttm, gpu-sched, drm_buddy) y docs/Wayland_Estado_y_Decisiones.md (D1, D2, D5, D6, D10).
- kernel/linuxkpi/drivers/lkpi_drm.mk, lkpi_drm.srcs, lkpi_drm/glue.c y kernel/linuxkpi/patches/ (cómo se compila y parchea el DRM de Linux), y kernel/linuxkpi/mm/shmem.c.

Alcance de L6 según el plan:
- `ttm` (buffer objects con VRAM/GTT, evicción, mapeo), `drm_gpu_scheduler` (`gpu-sched`), `drm_buddy`, `dma_resv` en uso real, `shrinker` mínimo, `shmem`/swap (páginas «pageables» respaldadas por RAM, sin disco de swap), `mmu_notifier` como stub, `hmm` desactivado. Compila el código de Linux 6.18.59 desde build/linuxkpi_src con parches mínimos versionados, como en L5 (`lkpi_drm.kur` o un `.kur` nuevo; justifica cuál).
- Hito: un driver de memoria de dispositivo simulado (un `.kur` de prueba que usa `ttm` con un «VRAM» en RAM) crea, mapea y evicta buffers; las pruebas de estrés de GEM/`ttm` pasan. Añade además el uso de `gpu-sched` con un trabajo simulado y fences reales.

Pendientes heredados de L5 que puedes recoger si encajan (pregunta antes de ampliar el alcance):
- `kmscube`: faltan `libgallium` y `libLLVM` (`scripts/fetch-mesa.sh`, necesita red y QEMU con ≥ 1,5 GB) y el paquete `kmscube` de Ubuntu. La ruta GBM sobre DRM nunca se ha ejercitado en Aurora.
- El plano de cursor de virtio-gpu no se compone en la ventana de scanout.
- El syncobj nativo (`kernel/drm/drm_syncobj.c`) no sigue la ABI de Linux en `IMPORT_SYNC_FILE` (handle existente).
- Hot-plug físico de monitores sin probar (QEMU no lo genera); compositor multi-monitor y cambio de modo real (D1/D2 de usuario) siguen pendientes.

Haz todas las preguntas de diseño en una sola ronda antes de programar, con tu recomendación en cada una (entre otras: hasta dónde llega `ttm` frente a un gestor de memoria más simple, qué `.kur` lo aloja, cómo se simula la VRAM y la evicción, qué se hace con `shrinker` y swap sin disco, y si recoges algún pendiente de L5).

Reglas que se mantienen:
- Licencia GPL-2.0-only; cabeceras propias, sin copiar las de Linux, salvo las de interfaz que se leen del árbol descargado por `build/linuxkpi/ext-inc` (aprobado para include/drm y uapi/drm; pregunta antes de ampliarlo a otras). El código de drivers/ de Linux se compila tal cual desde build/linuxkpi_src (no se versiona); los parches mínimos van en kernel/linuxkpi/patches/ (kernel/linuxkpi/drv.mk los aplica a una copia).
- Todo lo que exporta un `.kur` de la capa lleva prefijo lkpi_.
- La capa no incluye cabeceras de Aurora: cada servicio se declara en kernel/linuxkpi/aurora.h (aurora_* con etiqueta asm) y se exporta en kernel/ksymtab_exports.c con la misma firma y _Static_assert de las estructuras replicadas. task_t es opaco.
- Una prueba .kur por nivel en tests/linuxkpi/<nombre>/ con drv.mk, KT_ASSERT y REGISTER_TEST_FLAGS(..., TEST_FLAG_BLOCKING); las de varias CPU, también TEST_FLAG_NEEDS_SMP. El .c de la prueba no puede llamarse como el directorio.
- Nada de arrays grandes en la pila de module_init; klog_vprintf no añade salto de línea; la memoria DMA/sg no puede estar en la pila.
- No toques drivers de GPU reales (i915, xe, amdgpu, nvidia) ni escribas código de pantalla de Intel/AMD/NVIDIA: L6 es infraestructura y un driver simulado.
- Cada decisión de diseño va fechada a Plan_LinuxKPI.md §11; actualiza docs/Wayland_Estado_y_Decisiones.md si afecta a D1/D2/D5/D6/D10.
- No cambies las versiones fijadas (Linux 6.18.59, linux-firmware 20261019, NVIDIA open 615.78.08) sin hablarlo.
- Un solo QEMU a la vez, con -m 6G. Usa scripts/qemu-drive.py en segundo plano; suite del kernel con --done "passed, "; -smp 4 con --extra=-smp --extra=4. qemu-drive.py recompila la ISO en cada arranque: no edites fuentes mientras tanto. Espera por PID o por fichero (until-loop), nunca con pgrep -f/pkill -f con un patrón que aparezca en tu propia orden; el sleep en primer plano está bloqueado. Una orden de qemu en su propia línea, no tras un `&&` con otros comandos.
- DRM de Linux en QEMU: --cmdline "aur.skip=1234:1111 lkpi.load=linuxkpi,lkpi_drm,lkpi_bochs" (bochs, -vga std); "lkpi.load=linuxkpi,lkpi_drm,lkpi_simpledrm"; "aur.skip=1af4:1050 lkpi.load=linuxkpi,lkpi_virtio,lkpi_drm,lkpi_virtio_gpu" con --extra=-vga --extra=none --extra=-device --extra=virtio-gpu-pci[,max_outputs=2]. Con --boot-test /usr/bin/drm_test --done "ALL PASS". Weston: --boot-test "/usr/bin/weston.sh 12 /usr/bin/weston-simple-shm /usr/bin/weston-flower /usr/bin/weston-terminal" --done "WESTON-DONE". modetest necesita -M <nombre del driver> (p. ej. bochs-drm).
- Las pruebas de usuario necesitan /usr/bin/musl-gcc; los binarios se suben junto al fuente en user/musl/<app>. El hito de L4/L5 se repite con --boot-test /usr/bin/lkpi_test --done "LKPI-TEST:" (108 comprobaciones hoy).
- task_t debe seguir en 2048 bytes o menos (_Static_assert); si cambian on_cpu o fs_base, actualiza kernel/switch.asm.
- git add con rutas explícitas, nunca -a. Los ficheros generados por qemu-drive.py (build/drive-aurora.conf) no se versionan.
- No rompas la suite: kernel 204/204 (también con -smp 4), lkpi_test (108), drm_test (nativo y con los tres drivers de Linux), sig_test, rseq_test, timer_test, thread_sig_test, clipboard-x11.sh y primary-icons.sh (este último con -t 600; hoy pasa en ~135 s; si se cuelga, investígalo, no lo ignores).
- Si encuentras fallos de Aurora por el camino, arréglalos si son pequeños y del alcance; si no, apúntalos y pregunta.

Al terminar, crea docs/LinuxKPI_Sesion_L6.md (qué se hizo, decisiones, pruebas, commits, regresión, pendientes), actualiza la línea de estado de Plan_LinuxKPI.md y dame el informe final.
