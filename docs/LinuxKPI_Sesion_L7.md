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
