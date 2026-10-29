# LinuxKPI — sesión L7 (Intel: `i915` completo sobre la capa)

Fecha: 28-oct-2026. Rama `claude/project-thread-v3dezm`. Sin PR ni merge a main. Linux 6.18.59 (sin cambios de versión).

## 1. Estado

| Hito del Plan | Estado |
|---|---|
| `i915` compila y enlaza en la capa (298 ficheros, Gen2–Gen12 según Kconfig) | **Hecho** |
| `lkpi_i915.kur` carga y descarga en QEMU sin GPU Intel (suite del kernel 208/208) | **Hecho** |
| (1) probe con identidad por serie en la Toshiba Portégé R30-A | **Pendiente de hardware** (lo valida Álvaro) |
| (2) pantalla nativa con GOP de respaldo, (3) 3D con Mesa | Pendiente de (1) |
| `xe` | **Diferido**: ver §5 |

QEMU no tiene GPU Intel real: aquí solo se valida compilar, enlazar, cargar, inicializar y descargar. Nada de lo que toca registros de la GPU se ha ejecutado.

## 2. Qué se hizo

- **`lkpi_i915.kur`** (`kernel/linuxkpi/drivers/lkpi_i915.mk`, lista de fuentes `lkpi_i915.srcs` extraída del Kbuild con `scripts/lkpi-kbuild-srcs.py`; 298 ficheros, excluye `i915_mm.c`). Config de Kconfig por defecto en `lkpi_i915_cflags.mk`. i915 depende de los includes transitivos de Linux: `-include lkpi/gpu_prelude.h` los aporta sin tocar el driver.
- **`lkpi_drm_display.kur`** (`lkpi_drm_display.mk`): helpers de `drivers/gpu/drm/display` (DP, MST, DSC, HDCP, HDMI, SCDC, dual mode) más `drm_panel` y `drm_mipi_dsi`. Sin CEC, aux-dev/aux-bus, túnel DP ni audio/estado HDMI.
- **Glue de i915** (`drivers/lkpi_i915/glue.c`): `remap_io_mapping`/`remap_io_sg` sobre `lkpi_remap_pfn_range`, stubs de GMCH, `intel_graphics_stolen_res`, `ips_link_to_i915_driver`, `lkpi_init_utsname`. Un parche mínimo: `patches/intel_rps.c.patch`.
- **Capa**: `lib/random.c` (RDRAND o xorshift; `lkpi_generate_random_uuid`), `lib/misc.c` (int_pow, intlog10, hex_dump, bitmap, etc.), `pci/pci_ext.c` (`pci_bus_read_config_*`, `pci_dev_present`, `pcie_find_root_port`), x86 CPU (`x86_model`, `lkpi_x86_match_cpu`, `lkpi_tsc_khz`), `i2c_lock_operations` por defecto, `kernel_write`, dentry privada de debugfs (`d_name`, `f_path.dentry`).
- ≈90 cabeceras nuevas o ampliadas (random, radix-tree, perf_event, relay, pwm, dmi, gpio/pinctrl, acpi completa en stub, pm, uaccess, fs, etc.).
- **Cargador de `.kur`** (Aurora, `kernel/kur_loader.c`): las secciones `SHF_ALLOC` vacías ahora reciben dirección (ocupan 0 bytes). i915 tiene relocaciones contra símbolos de sección vacíos y el cargador las rechazaba (`símbolo '' en sección no cargada`).
- **Test**: `linuxkpi: L7 i915 (carga y descarga sin GPU Intel)` en `kernel/tests/kernel_tests.c` carga drm, ttm, display e i915 y los descarga en orden inverso. Los `.kur` van en la ISO (`LKPI_MODULES`).
- **i915 no se autocarga**: solo con `lkpi.load=` explícito, y el arranque nunca depende de él (GOP de respaldo).

## 3. Decisiones (ver Plan §11, 28-oct-2026)

- Config de i915: USERPTR, PXP, GVT, DP_TUNNEL, COMPRESS_ERROR, perf events (`i915_pmu`), HWMON, FBDEV_EMULATION y COMPAT **desactivados**; ACPI activado con API en stub; `MMU_NOTIFIER` no definido.
- Gen2–5 (GMCH GTT, `intel-gtt.c`) **no portado**: `intel_gmch_probe` devuelve 0. Haswell (banco de pruebas) usa GGTT propio de i915.
- Opregion se lee por `ASLS` en el espacio PCI (el AML de Aurora no evalúa métodos con argumentos).
- Sin IOMMU hasta L9.

## 4. Límites conocidos y stubs (a resolver con hardware)

- `on_each_cpu` ejecuta solo en la CPU actual (el IPI de llamada no tiene manejador): afecta a `wbinvd`/flush en SMP. `stop_machine` solo desactiva IRQ en local.
- Métodos ACPI (`_DSM`, `_DOD`, `_ADR`) fallan con `AE_NOT_FOUND`.
- Sin ECAM: el espacio de configuración PCI es de 256 bytes.
- GEM por `vm_mmap`/`vfs_mmap` devuelve `-ENODEV` (el camino moderno `mmap_offset` vía drm_gem debe funcionar); userptr desactivado.
- `sysfs_merge_group` usa `create_group` (sin verificar); `register_sysctl` es un no-op.
- Stubs: pwm, gpio, pinctrl, hwmon, relay (log de GuC), perf, PXP, bus auxiliar, `pci_map_rom`, BAR resize.
- Sin probar en ninguna GPU Intel: reset de GPU, power wells, GMBUS, DDI/DP, rc6, GuC/HuC.

## 5. `xe` y siguiente paso

`xe` solo aplica a Gen12+ con GPUs que el banco de pruebas no tiene (Tiger Lake en adelante o Arc). Se difiere hasta tener hardware o hasta que se confirme el alcance; reutilizará `lkpi_drm_display`. Antes de eso conviene el primer probe de `i915` en la Portégé: hace falta `lspci -nn` y el log serie con `lkpi.load=linuxkpi,lkpi_drm,lkpi_ttm,lkpi_drm_display,lkpi_i915` (y `gpu=off` por defecto para no tocar la pantalla).

## 6. Lección de pruebas

Tras cambiar `struct` en cabeceras de la capa hay que reconstruir limpio (`make linuxkpi-clean && make linuxkpi`): una primera pasada con objetos viejos dio un pánico en `lkpi_test_drm` que desapareció al limpiar. Ver también la nota equivalente para el kernel en `docs/Wayland_Estado_y_Decisiones.md`.

## 7. Regresión

Suite del kernel 208/208 con `-smp 2` y `-smp 4`; `lkpi_test` 141; `drm_test` nativo, bochs, simpledrm y virtio-gpu (2 salidas); `sig_test`, `rseq_test`, `timer_test`, `thread_sig_test`; `clipboard-x11.sh` y `primary-icons.sh` (valores revisados). El fallo de K7 (fences sin liberar) visto en la pasada con objetos viejos no se reprodujo tras reconstruir.

## 8. Primer probe real (Portégé R30-A, HD 4600, 28-oct-2026)

Log de Álvaro con `klog.file`. Todo lo que falló venía de una sola causa de diseño más un límite de la capa:

| Síntoma | Qué es | Dónde se resuelve |
|---|---|---|
| `kmalloc(67112960) excede el límite de 67108864` | **Bug de la capa**: la sombra de display (L5) reservaba 64 MiB con `kvzalloc`, que cae en `lkpi_vmalloc` = `kmalloc(size + 4 KiB)` y el heap rechaza > 64 MiB | Corregido: sombra de BAR grande ≤ 32 MiB (solo la parte del framebuffer) y aviso si falla |
| `render: timed out waiting for forcewake ack`, `timeout waiting for SBI`, `FDI mPHY reset assert timeout`, WARN en `intel_display_power_well.c:299`, `i915_irq.c:105` y `engine mmio_base` | **Esperado por diseño** (display-shadow de L5): con el framebuffer del firmware en la GPU, sus BAR —incluido BAR0, los registros— eran una copia en RAM y la GPU nunca recibía los accesos | Nueva opción `lkpi.shadow=fb`: solo el BAR del framebuffer (GMADR) queda en sombra, los registros son reales. `lkpi.shadow=0` quita toda la sombra |
| `#UD` en `drm_clflush_virt_range` | Bug de la capa (clflushopt sin CPUID) | Corregido en 33d30846 |

Siguiente: probar con `lkpi.shadow=fb`; lo que falle entonces ya es de i915 sobre la capa real (forcewake, power wells, GMBUS, GGTT). El 3er paso (pantalla nativa) exige que el compositor pinte en un buffer KMS de i915 en lugar del framebuffer del firmware; no está hecho.

## 9. Segundo sondeo real (pantalla aún en negro) y diagnóstico

Log de la Portégé con e574cc04: `memoria stolen 32 MiB en 0xcda00000`, `Initialized i915 1.6.0`,
sin errores, y el sistema y el compositor siguen vivos (COMP-PERF hasta el final). Con el log a
nivel INFO no se ve por qué el panel sigue negro, así que se añaden dos herramientas de arranque:
- `drm.debug=0x1e` (u otra máscara): fija `__drm_debug` como en Linux (lkpi_drm).
- `lkpi.i915_diag=1`: tras el probe vuelca PIPECONF, plano primario, DDI, panel, backlight,
  pozos de energía y las primeras PTE de la GGTT (`i915-diag:` en el log). Solo lee registros.

## 10. Causa de la pantalla en negro (log con drm.debug + i915_diag)

El volcado mostró el eDP aún activo con la config del BIOS (TRANS_DDI_FUNC_EDP=0x82204000,
DSPCNTR_A habilitado), pero i915 leyó "hw state readout: disabled" en todos los CRTC/planos y
limpió la GGTT (`clearing unused GTT space`), de modo que GGTT[0] apuntó a la página scratch y
el plano escaneó negro. Causa: `pm_runtime_get_if_in_use()`/`get_if_active()` devolvían -EINVAL
(con CONFIG_PM=1 i915 lo trata como "no hay referencia") y `intel_display_power_get_if_enabled()`
fallaba para todos los dominios. Ahora devuelven 1 y cuentan la referencia (dispositivo siempre activo).

## 11. Hito 1 cumplido en hardware real (28-oct-2026)

Con 0b85e013 la Portégé (HD 4600) dibuja: i915 lee el pipe A/plano primario/eDP como `enabled`
y conserva la imagen del BIOS; el escritorio sale como una ventana gris «AuroraOS DRM» con menos
parpadeo. Quedan: (a) el compositor aún no presenta con page-flip de i915 (parpadeo residual);
(b) hito 2 del plan: pantalla nativa con GOP de respaldo (modeset propio, vblank, hotplug);
(c) hito 3: 3D con Mesa `crocus` + linux-dmabuf. `xe` sigue diferido (sin Gen12+).

## 12. Hito 2: presentacion por KMS con doble buffer (28-oct-2026)

Problema: el compositor volcaba al framebuffer del GOP sin vsync (volcado diferencial a VRAM) y, con
i915 cargado, ademas cada commit copiaba el plano primario a una ventana gris «AuroraOS DRM». El
panel se leia a mitad del volcado: parpadeo. `aurora-wayland` no toca DRM (sus ventanas son del
compositor del kernel), asi que el unico presentador es `kernel/gfx/compositor.c`.

**Medida.** `[COMP-PERF]` ahora distingue el camino:
- GOP: `GOP: volcado sin vsync, exposicion al barrido N% (blit avg X us de 16667)`. Es la fraccion de
  cada periodo de refresco (60 Hz) durante la que se escribe VRAM; el barrido de la pantalla cae
  dentro de esa ventana con esa probabilidad. Es un proxy (sin hardware no se lee la linea de barrido).
- KMS: `KMS: N flips, F fallidos, M vblanks perdidos | commit avg=.. max=.. us`. El commit es
  bloqueante: con vblank real su duracion es el periodo del refresco; `M` cuenta vblanks de mas entre
  dos flips consecutivos.

**Diseno** (`kernel/drm/kms_present.h`, `kernel/linuxkpi/drivers/lkpi_drm/kms_present.c`):
- Un cliente DRM del kernel (API `drm_client` de Linux, sin fbdev) se engancha al final de
  `drm_client_setup()` (parche `drm_client_setup.c.patch`; i915, bochs, virtio-gpu y simpledrm ya la llaman).
  Hace `drm_client_modeset_probe`, elige el modo de **igual tamaño que el compositor** (el eDP de la
  Portégé: 1366x768; si el conector no lo tiene, no hace nada y sigue el GOP), reserva dos buffers
  dumb XRGB8888 (`drm_client_framebuffer_create`) y los mapea (`drm_client_buffer_vmap`: i915 lo hace WB).
- Registra un backend en el kernel (`drm_lx_kms_register`). El compositor, por cada frame con daño, copia
  los rectangulos al buffer trasero (con el cursor) y llama a `flip()`: commit atomico bloqueante del
  cliente (`drm_client_modeset_commit`, plano primario con el fb trasero) y intercambio. **El modeset del
  panel ocurre en el primer flip**, con el frame ya pintado: no hay pantalla negra intermedia.
- Buffer age 2: al final de cada frame se copia tambien el daño del frame anterior al trasero; si un
  commit falla (otro maestro DRM → `-EBUSY`) no se intercambia y el daño se acumula.
- En i915 se hace `clflush` de las filas escritas antes del commit (mapeo WB, el escaneo de Haswell no
  es coherente con la cache); `lkpi.kms_flush=0|1` lo fuerza.
- Los commits del cliente no pasan por el ritmo por software (`lkpi_drm_pace`) ni por la ventana
  «AuroraOS DRM» (`lkpi_drm_present`); los de userland (drm_test, etc.) siguen igual.
- **GOP de respaldo:** si el modeset propio falla 3 veces seguidas (que no sean `-EBUSY`) el backend
  devuelve `-ENODEV` y el compositor vuelve al volcado a VRAM; si no hay modo del tamaño del compositor,
  ni se registra.

**Opciones de arranque:** `lkpi.kms=` `auto` (por defecto, solo i915) | `1` (cualquier driver DRM de
Linux; para QEMU: bochs, virtio-gpu) | `0` (apagado: GOP + ventana «AuroraOS DRM» como en el hito 1).
`lkpi.kms_flush=`; `drm.debug=0x14` (KMS+ATOMIC) y `lkpi.i915_diag=1` como antes. Trazas del cliente:
`kms: vblank avanza|NO avanza (contador a -> b en 60 ms)`, `kms: modeset propio hecho (WxH, vblank real)`,
y `kms: commit de N ms (vblank/flip_done sin IRQ?)` si el commit tarda mas de 3 s.

**Para la Portégé** (`/data/aurora.log`): arrancar con `lkpi.load=linuxkpi,lkpi_drm,lkpi_ttm,lkpi_drm_display,lkpi_i915 lkpi.shadow=fb lkpi.i915_diag=1 drm.debug=0x14`.
Buscar `kms:` (modo elegido, vblank avanza, modeset hecho) y `[COMP-PERF] KMS:` (flips, commit ≈ 16.7 ms,
vblanks perdidos 0). Si el panel se queda congelado, `lkpi.kms=0` devuelve el comportamiento del hito 1.

**Limites:** el compositor no cambia de resolucion en caliente (si el panel nativo no tiene el modo del
GOP, no se hace KMS); sin multi-monitor (un CRTC); sin hotplug de conectores mas alla de reintentar el
probe en cada evento; si el modulo se descarga con el KMS activo el compositor vuelve al GOP pero la
pantalla ya no la barre el GOP; page-flip no bloqueante con evento (sin esperar en el hilo del
compositor) queda para cuando se mida en hardware.

**Corrección tras la primera prueba en la Portégé (log de Álvaro, 28-oct-2026):** no salió ninguna
línea `kms:` y el compositor siguió en GOP (`[COMP-PERF] GOP: ... exposicion 0-4%`). Causa: i915
compilado sin FBDEV_EMULATION usa un `intel_fbdev_setup()` vacío y nadie llama a `drm_client_setup()`,
que es donde cuelga el cliente KMS. Nuevo `patches/i915_driver.c.patch`: `i915_driver_register()` llama a
`drm_client_setup()` al final. (bochs/virtio-gpu/simpledrm ya la llamaban, por eso QEMU funcionaba.)
Nota del mismo log: la exposicion al barrido del camino GOP medida en la Portégé es baja (0-4 %), asi que
el parpadeo no se explica solo por el volcado; si persiste con KMS activo, el siguiente sospechoso es el
pipe/panel (PSR, backlight) y no el compositor.

**Segundo log de la Portégé (GPF):** con el cliente ya enganchado, el modeset elige 1366x768 en el pipe A,
se crean los dos buffers dumb (`FB:122`, `FB:123`) y al mapear el segundo i915 llama a `drm_clflush_sg()`,
que cae con `#GP` en `clflush` (rip dentro de `drm_clflush_sg+0xca`). Causa: `nth_page(page, n)` de la capa
era `page + n`, pero las `struct page` de la capa no forman un array contiguo por pfn (cada pagina de shmem
se asigna sola) e i915 junta paginas fisicamente contiguas en un mismo sg. Ahora
`nth_page` = `lkpi_pfn_to_page(page->pfn + n)`. Ademas, al activarse el KMS se cierra la ventana
«AuroraOS DRM» que abrio el commit inicial del driver. Con `make linuxkpi-clean && make linuxkpi` por
cambiar una cabecera de la capa.

**Tercer log de la Portégé: el KMS funciona** (`vblank avanza`, `modeset propio`, `[COMP-PERF] KMS: 120 flips, 0 fallidos`,
commit ≈ 11-15 ms con picos de 24 ms; 0-10 vblanks perdidos por cada 120 flips, a vigilar). Fallo visual restante: rayas en el Dock al
mover una ventana o pasar el cursor. Causa: el daño del frame anterior se repasaba desde el
`backbuffer`, pero `compose_scene(e)` repinta fuera del clip (margen del desenfoque del Dock) sin Dock ni
menus, asi que esas zonas del backbuffer no son una imagen final. Ahora el repaso sale de `front_shadow`
(ultima imagen final presentada, solo recibe rectangulos terminados).

## 13. Regresion final del hito 2 (29-oct-2026, codigo de 4d7183b0)

QEMU sin KVM, un solo QEMU a la vez (`scripts/lkpi-regress.sh`): suite del kernel con `-smp 2` y `-smp 4`,
`lkpi_test`, `drm_test` (nativo, bochs, simpledrm, virtio-gpu 2 salidas), `sig_test`, `rseq_test`,
`timer_test`, `thread_sig_test`, `clipboard-x11.sh` y `primary-icons.sh`: **todo OK**. Prueba de KMS con
`lkpi.kms=1` en bochs: modeset propio hecho, 114 flips, 0 fallidos. Pendiente solo lo que exige hardware:
confirmar en la Portégé que las rayas del Dock han desaparecido y bajar los vblanks perdidos.

**Pendiente del bloque (hito 2):** (1) confirmar el Dock en hardware; (2) commit no bloqueante con evento de
page-flip si los vblanks perdidos persisten; (3) virtio-gpu/bochs con `lkpi.kms=1` en la regresion
automatica (ya en `scripts/lkpi-regress.sh kms`); (4) hito 3 de L7.

## 14. Hito 3: Mesa `crocus` y `linux-dmabuf` (29-oct-2026, rama `claude/l7-hito3-crocus-dmabuf`)

Decisiones y razones en Plan §11 (29-oct-2026). Resumen de lo hecho:

- **Mesa**: no hay que compilarlo; el de Ubuntu (`fetch-mesa.sh`, 25.2.8) trae `crocus`. `linux-dmabuf` ya estaba en `aurora-wayland`.
- **`vm_mmap`/`vm_munmap`** en la capa (antes `-ENODEV`): `vfs_mmap_node_current`/`vfs_munmap_current` en el kernel (fd temporal + `sys_mmap`) y `->mmap` con fallo de pagina en los ficheros shmem. Desbloquea `I915_GEM_MMAP` legacy. `I915_MMAP_WC` sigue sin soporte (sin `find_vma`).
- **`tests/wayland/aw_gbm_test.c` + `guest/gbm.sh`**: GBM → EGL → GLES2 → dma-buf → toplevel de `aurora-wayland`. `swrast` usa `card0` (los dumb buffers no se permiten en nodos render de Linux); `crocus`/`iris` usan `renderD128`.
- **Pruebas en QEMU**: `lkpi_test` 145/145 (4 nuevas de `vm_mmap`); `aw_gbm_test` ALL PASS con el backend nativo y con virtio-gpu de Linux (llvmpipe, `GL_RENDERER=llvmpipe`, pixel de la barra verde `33c759`).
- **Sin verificar (necesita la Portégé)**: todo lo que toque la GPU Intel con Mesa. Receta en Plan §11. Si falla, el primer dato es la linea `FAIL ...` de `aw_gbm_test` (indica el paso: `gbm_create_device`, `eglInitialize`, `gbm_bo_create`, `eglCreateImage`, `framebuffer incompleto`, `el pixel leido no es el pintado`, `gbm_bo_get_fd`, linux-dmabuf) junto con `drm.debug=0x14` y `lkpi.i915_diag=1`.
- **Limites**: `on_each_cpu`/IPI sigue ejecutando solo en la CPU local (afecta a `wbinvd` en SMP); sin ECAM; el mapeo del dma-buf de un bo de i915 desde `aurora-wayland` (copia al almacen de la superficie) depende de la coherencia de cache del mapeo: si se ven restos o lineas viejas, es el primer sospechoso.
- **Regresion (29-oct-2026)**: suite del kernel 208/208 con `-smp 2` y `-smp 4`, `lkpi_test` 145, `drm_test` (nativo, bochs, simpledrm, virtio-gpu) y `kms-bochs`: OK. Aviso de entorno: con Mesa/LLVM (`libgallium`, `libLLVM`, ~500 MB) dentro del initrd la suite `-smp 2` dio 2 fallos de memoria (`test_k9.c:260` «la RAM del respaldo se libera» y el estres SMP de ttm); sin Mesa en el initrd, 208/208. Hacer la regresion del kernel sin Mesa y las pruebas de `gbm.sh` con ella. `userland` (sig_test, etc.) no se repitio: este hito no toca senales ni temporizadores.

