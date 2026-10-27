# LinuxKPI: sesión del 27-oct-2026 (fase L4: K1-DMA, K6 básico, K8)

Registro de la fase L4 de la capa de compatibilidad con drivers de Linux: DMA, `sg_table`, `dma_buf`, firmware, I²C, debugfs, backlight y E/S de usuario. El plan es `Plan_LinuxKPI.md` (decisiones con fecha en su §11); las sesiones anteriores, `docs/LinuxKPI_Sesion_L0_L1.md`, `docs/LinuxKPI_Sesion_L2.md` y `docs/LinuxKPI_Sesion_L3.md`.

Estado: **L4 terminada**. **L5 (DRM/KMS sobre la capa) no empezada**: espera confirmación del autor.

Resumen:
1. **Aurora** (§2): `vfs_ops` con operaciones por fd (`read_fd`/`write_fd`/`llseek`/`setfl`/`mmap_dev`), `F_SETOWN`/`O_ASYNC`, VMA de dispositivo (`VMA_DEV`) con `fault`/`open`/`close`, región `vmap` del kernel, `dmabuf` sobre páginas ajenas, identidad de tareas y señales para la capa. Una corrección de paso: `mprotect` perdía `PTE_SPECIAL` (§3.1).
2. **Capa** (§2): K8 (`task_struct` validado por tid, `current->mm`, `signal_pending` real, `struct file`/`file_operations` sobre el VFS, `cdev`/`misc`/`/dev`, `poll`/`epoll`/`fasync`, `mmap` con `vm_area_struct`), K1-DMA definitivo (bounce, WC, `sg_table`, `dma_pool`, `vmap`, `dma_buf` ↔ PRIME) y K6 básico (firmware real, I²C con `i2c-algo-bit`, `seq_file`, debugfs, backlight).
3. **Hito** (§4): `/dev/lkpi_test` (un `miscdevice` de Linux en `.kur`) ejercitado desde el espacio de usuario por `user/musl/apps/lkpi_test`: 101 comprobaciones, todas en verde.
4. Rama `claude/beautiful-newton-1neyw4`, **sin PR ni merge a `main`** (a petición del autor).

---

## 1. Decisiones tomadas (antes de escribir código)

Preguntas de diseño en una sola ronda, con recomendación en cada una; el autor eligió todas las recomendaciones («1 A … 9 A»). Detalle en `Plan_LinuxKPI.md` §11 («27-oct-2026 — L4»).

| Tema | Decisión |
|---|---|
| Puente `file_operations` ↔ VFS | Operaciones opcionales con el fd en `vfs_ops` (buffer de usuario, offset por puntero), `llseek`, `setfl` (fasync), `F_SETOWN`/`O_ASYNC`; `-ERESTART*` → `-EINTR`. |
| `mmap` | `VMA_DEV` con referencia al fd y `fault`/`open`/`close`; `fork` y divisiones llaman a `open`. |
| K8 | Tabla `task_t* → task_struct` validada por tid (sin campo en `task_t`), `mm_struct` por proceso, señales de Aurora. |
| `dma_buf` | Enlazado con el PRIME de Aurora en los dos sentidos; `dma_resv` mínimo hasta K7. |
| DMA streaming | Bounce reservado al momento bajo la máscara, sin pool fijo. |
| `vmap` | Región virtual propia del kernel de Aurora. |
| Firmware | `kernel/linuxkpi/firmware.list` → initrd, con `Link:` de `WHENCE` y SHA-256. |
| debugfs | Árbol en memoria montado en `/sys/kernel/debug`. |
| Hito | `lkpi_test_k6` (suite) + `lkpi_test_dev.kur` cargado por `user/musl/apps/lkpi_test`. |

Cambios de diseño durante la sesión (también en §11):
- **`SIGIO` por pid, no por fd**: la primera versión guardaba el `file_descriptor_t` en el `struct file` para `kill_fasync`; un fd puede liberarse mientras el nodo sigue vivo (un `get_file` del driver). Ahora `F_SETOWN` avisa al fichero (`setfl`) y la capa guarda el pid; Aurora envía con `vfs_send_sigio(owner, sig)`.
- **`llseek` recibe el offset actual** del fd y el VFS guarda el resultado (la capa no ve `file_descriptor_t`).
- **`dma_buf` sin cachés de identidad**: cada `dma_buf_fd()` crea su dmabuf de Aurora (con una referencia al `dma_buf`) y cada `dma_buf_get()` de un dmabuf nativo crea un envoltorio. Una caché por nodo tenía carreras de vida útil sin un «get si no es cero» en los nodos de Aurora.

## 2. Qué se ha hecho

### Lado Aurora (`kernel/`)
| Cambio | Dónde | Prueba |
|---|---|---|
| `vfs_ops`: `read_fd`/`write_fd` (buffer de usuario, offset del fd por puntero; `read`/`write`/`pread`/`pwrite`/`readv`), `llseek` (con el offset actual), `setfl`, `mmap_dev` | `vfs.c`, `vfs.h` | hito |
| `fcntl`: `F_SETOWN`/`F_GETOWN`, `O_ASYNC` en `F_SETFL` (solo en ficheros con `setfl`), `vfs_send_sigio` | `syscall.c`, `vfs.c` | hito (SIGIO) |
| Nodos y fds para la capa: `vfs_node_alloc`, accesores, `vfs_fd_reserve`/`install_node`/`unreserve` (huecos de `get_unused_fd_flags`) | `vfs.c` | hito |
| `VMA_DEV`: referencia al fd, `dev_ops` (`fault` desde el `#PF`, `open` en `fork`/división, `close` al desaparecer), `VMA_DEV_DONTCOPY`, `vma_dev_map_page`/`vma_dev_zap`; `mremap` no lo hace crecer; swap y `/proc/<pid>/maps` | `pf.c`, `pf.h`, `process.c`, `swap.c`, `procfs.c` | hito (fork, mprotect, zap) |
| **`mprotect` conserva `PTE_SPECIAL`, `PTE_SHARED_OBJ` y la caché** (§3.1) | `pf.c` | hito (mprotect sobre `VMA_DEV`) |
| Región `vmap` (PML4 416, 64 GiB, PDPT reservado al arrancar, página de guarda, shootdown) | `vmap.c`, `vmap.h`, `main.c` | prueba del kernel nueva |
| `drm_gem_new_ext`, `dmabuf_node_new_ext`/`_pages`/`_ext_priv`: un `dma_buf` de Linux es un dmabuf de Aurora | `drm/drm_dumb.c`, `drm/drm_prime.c` | hito (PRIME en los dos sentidos) |
| `task_info_get`, `signal_unblocked_of`, export de `task_tid` | `process.c`, `signal.c` | K6, hito |
| Exports con `_Static_assert` de `vfs_ops_t`, `vfs_dirent_t`, `vfs_fs_ops_t`, `vma_t`, `vma_dev_ops_t`, `task_info_t` | `ksymtab_exports.c` | carga de la capa |

`task_t` no cambia (sigue en 776 bytes); `vma_t` crece 32 bytes y `file_descriptor_t` 4.

### Cabeceras (`include/linuxkpi/`, propias)
`fs.h` (`struct file`, `inode`, `file_operations`, `address_space`, chrdev, fasync, `*_llseek`, `simple_*`), `file.h`, `poll.h`, `cdev.h`, `miscdevice.h`, `major.h`, `anon_inodes.h`, `fcntl.h`, `ioctl.h` (+ `asm`/`uapi`), `uapi/linux/eventpoll.h`, `signal.h`, `capability.h`, `cred.h`, `sched/mm.h`, `mmap_lock.h`; `mm_types.h` (`mm_struct`, `vm_area_struct`, `vm_operations_struct`, `vm_fault`, `pgprot_t` con la PAT de Aurora) y `mm.h` (`VM_*`, `remap_pfn_range`, `vmf_insert_*`, `zap_vma_ptes`, `pgprot_writecombine`…); `sched.h` (señales reales). DMA: `dma-mapping.h` completo, `scatterlist.h`, `dmapool.h`, `vmalloc.h` (`vmap`), `dma-buf.h`, `dma-resv.h` (mínimo), `iosys-map.h`. K6: `firmware.h`, `i2c.h`, `i2c-algo-bit.h`, `mod_devicetable.h` (`i2c_device_id`), `seq_file.h`, `debugfs.h`, `backlight.h`.

### Implementación (`kernel/linuxkpi/`)
| Fichero | Contenido |
|---|---|
| `kernel/sched.c` | Identidad de Aurora (pid = tid, tgid, comm, uid), validación por tid con entrada nueva si un driver guarda la vieja, `mm_struct` por proceso, `signal_pending`/`fatal_signal_pending`, `capable` (root o hilo del kernel). |
| `fs/file.c` | Puente `vfs_ops` ↔ `file_operations`, `poll` con entradas en las colas del driver que despiertan la cola de Aurora (y epoll), `fdget`/`fget`/`get_unused_fd_flags`/`fd_install`, `anon_inode_getfile`, fasync (SIGIO desde un work), `*_llseek`, `simple_*_buffer`. |
| `fs/char_dev.c` | Regiones de `dev_t` (dinámicas 254→234, 511→384), `cdev`, nodo de `/dev` en `device_add()` con devt (`devnode` de clase/tipo, modo), `misc_register` (major 10, clase `misc`, `private_data` = miscdevice). |
| `fs/debugfs.c` | Árbol en memoria, montado en `/sys/kernel/debug`; `create_{dir,file,u8..x64,bool,blob,regset32,…}`, `DEFINE_DEBUGFS_ATTRIBUTE`. |
| `mm/mmap.c` | `vm_area_struct` por `VMA_DEV`; `remap_pfn_range`, `vm_insert_page`, `vmf_insert_pfn(_prot)`, `vm_iomap_memory`, `zap_vma_ptes`, `unmap_mapping_range`; páginas con `struct page` con referencia hasta quitarlas. |
| `mm/vmalloc.c` | `vmap`/`vmap_pfn`/`vunmap` sobre la región de Aurora; `is_vmalloc_addr` la incluye. |
| `dma/mapping.c` | Coherente (WC, `NO_KERNEL_MAPPING`), streaming con bounce, `dma_mmap_*`, `dma_get_sgtable`, `dma_alloc_pages`, contadores. |
| `dma/scatterlist.c`, `dma/pool.c` | `sg_table` (desde páginas, iteradores de página y de DMA, copias), `dma_pool` (sin cruzar `boundary`). |
| `dma/dma-buf.c` | `dma_buf` (export, fd, get, attach, map, vmap, mmap) enlazado con PRIME; `reservation_ww_class`. |
| `misc/firmware.c` | `request_firmware(_nowait/_into_buf/_direct)`, `firmware_request_nowarn`. |
| `misc/i2c-core.c`, `misc/i2c-algo-bit.c` | Bus `i2c`, adaptadores y clientes, `i2c_transfer`, SMBus emulado; bit-banging propio (START/STOP, ACK, estirado de reloj, 7 y 10 bits). |
| `misc/backlight.c`, `lib/seq_file.c` | Clase `backlight`; `seq_file` (`single_open`, `DEFINE_SHOW_ATTRIBUTE`…). |

`linuxkpi.kur`: ≈ 703 KiB (antes ≈ 494), 723 exports (antes 543).

### Firmware real
`scripts/fetch-linux-drivers.sh --firmware --no-nvidia` descargó linux-firmware 20261019 (641 MiB, no se versiona). `kernel/linuxkpi/firmware.list` lista `i915/kbl_dmc_ver1.bin` (un `Link:` de `WHENCE` a `kbl_dmc_ver1_01.bin`, 8616 bytes); `scripts/install-linux-firmware.sh` (objetivo `linuxkpi-firmware`, dentro de `make linuxkpi`) lo copia al initrd comprobando su SHA-256. Sin el árbol no copia nada y las pruebas lo omiten.

## 3. Fallos y problemas encontrados

1. **`mprotect` perdía los bits que no son de permiso (Aurora)**. Reescribía las PTE con `new_flags` (presente, usuario, escritura, NX): un memfd `MAP_SHARED` con `mprotect` perdía `PTE_SPECIAL`/`PTE_SHARED_OBJ` (al salir el proceso, `paging_free_user_space` devolvía al PMM una página del memfd; en `fork` se copiaba en vez de compartirse) y un MMIO perdía `PTE_NOCACHE`. Ahora se conservan. Lo encontró la revisión al añadir `VMA_DEV`.
2. **`poll` de Aurora no filtra `revents`** por los eventos pedidos (Linux sí: `events | POLLERR | POLLHUP`); cada `vfs_ops.poll` de Aurora filtra por su cuenta. El puente lo hace; no se ha cambiado el comportamiento general de Aurora.
3. **`/dev/kmsg` se lee por índice de línea sobre un anillo de 16 KiB que rota**: una lectura incremental puede saltarse líneas, y **cada `read` deja una línea `DEBUG [KMSG-READ]` en el mismo anillo**, así que releerlo entero lo hace rotar todavía más (con `-smp 4` el programa no vio nunca el resumen de la suite: 291 718 lecturas y 15 min de espera, aunque las 101 comprobaciones pasaron después). El programa espera ahora a ver la capa cargada y luego descargada en `/proc/modules`. No se ha tocado Aurora (§7).
4. Errores propios corregidos antes de subir: macros `vmap`/`vunmap` que chocaban con los miembros de `dma_buf_ops` (ahora `static inline`, como Linux), `generic_file_llseek` como macro con argumentos (no valía como puntero), `.o` del driver de prueba con el mismo nombre que el objeto enlazado, y fallos de la propia prueba K6 (un buffer en la pila para `sg_init_one`, que tampoco vale en Linux; semilla mal calculada; ruta y salida en el mismo buffer).

## 4. Pruebas

### Suite del kernel
- Nueva **`vmap: páginas sueltas en la región del kernel`** (Aurora).
- Nueva **`linuxkpi: L4 DMA, dma_buf, firmware, i2c, backlight, debugfs`** (`tests/linuxkpi/lkpi_test_k6`, `TEST_FLAG_BLOCKING`):
  - `sg_table` desde 6 páginas en 3 tramos (fusión de contiguas, iterador, copia que cruza tramos);
  - DMA de streaming con máscara de 64 bits (directo) y por debajo del buffer (bounce) en las cuatro direcciones, `sync_for_cpu/device`, listas sg; **contra hardware**: el motor DMA de `edu` (máscara de 28 bits) lee y escribe un buffer por encima de 256 MiB a través del bounce;
  - `dma_alloc_wc` (mapeo WC en la región vmap), `dma_get_sgtable`, `dma_pool` (alineación, `boundary`, reutilización), `vmap` y `vmap_pfn` WC;
  - `dma_buf` sin fds (attach/map/vmap compartido/referencias/release);
  - firmware: el de Aurora, uno que falta, `_into_buf`, `_nowait` y **el real de linux-firmware** (tamaño y CRC-32);
  - **i2c-algo-bit contra una EEPROM 24C02 simulada** por software (líneas de drenador abierto, START repetido): bloque EDID de 128 bytes con checksum, SMBus byte/word/bloque, escritura, NAK de una dirección vacía con reintentos, atributos en `/sys/bus/i2c`;
  - backlight por `/sys` (brillo, máximo, `actual_brightness`, `bl_power`, tipo), debugfs (crear, buscar, borrar, recrear), `current` y un kthread (sin `mm` ni señales).

### Hito: `/dev/lkpi_test` desde el espacio de usuario
`tests/linuxkpi/lkpi_test_dev` (`lkpi_test_dev.kur`, un `miscdevice` escrito como un driver de Linux) y `user/musl/apps/lkpi_test` (binario en `user/musl/lkpi_test`). El programa espera a que acabe la suite del kernel (también carga y descarga la capa), carga `linuxkpi.kur` y el driver con `finit_module` y prueba:
- `current` visto por el driver (pid, tgid, comm, `mm`, uid); `/dev/lkpi_test` es carácter, major 10;
- `read`/`write`/`pread`/`pwrite`/`lseek` (SEEK_END, fuera de rango → `EINVAL`, EOF), ioctl desconocido → `ENOTTY`, `write` en `O_RDONLY` → `EBADF`;
- `mmap` de la memoria coherente (`dma_mmap_coherent`, dos mapeos ven lo mismo, tamaño excesivo rechazado);
- `mmap` por fallo de página (`vm_operations_struct`): un fallo por página, `unmap_mapping_range` y nuevo fallo, `fork` (`open`/`close` del hijo), `mprotect` en medio (dos `open`), `munmap` (tres `close`);
- `poll` (espera al temporizador del driver), `epoll`, **`SIGIO`** con `F_SETOWN`/`O_ASYNC` (y sin él no llega);
- **una espera del driver cortada por `SIGALRM` devuelve `EINTR`**, y **un hijo bloqueado en el driver muere con `SIGKILL`** (`signal_pending`/`fatal_signal_pending` reales);
- firmware por ioctl (el real de linux-firmware incluido);
- **`dma_buf` en los dos sentidos**: el driver exporta → `mmap` del fd, `dma_buf_get` del propio fd, `PRIME_FD_TO_HANDLE` en `/dev/dri/card0` y `MAP_DUMB` del GEM importado ven el patrón; un dumb buffer de `/dev/dri` exportado por PRIME → el driver lo importa, lo adjunta, lo mapea y lo lee por `vmap`;
- debugfs (`/sys/kernel/debug/lkpi_test/status` y `events`);
- descarga con `delete_module` (el nodo de `/dev` desaparece) y de la capa.

**Resultado: `LKPI-TEST: ALL PASS (101 comprobaciones, 0 omitidas)`**, con `-smp 2` y con `-smp 4`. Historia: la primera ejecución dio 97 de 101 (`poll` sin filtrar, §3.2) y esperó 15 min a la suite (§3.3); la siguiente, 101 de 101.

### Regresión
Todo en QEMU 8.2.2 TCG, `-m 6G`, un solo QEMU cada vez, con `edu` y `virtio-rng-pci`.

| Prueba | Al empezar (sin cambios) | Al terminar |
|---|---|---|
| Suite del kernel (`-smp 2`) | 199/199 (16 omitidas) | **201/201** (16 omitidas; +`vmap`, +K6) en 4 arranques |
| Suite del kernel (`-smp 4`) | — | **201/201** en 2 arranques |
| Hito `lkpi_test` (`-smp 2` / `-smp 4`) | — | **ALL PASS** (101) en los dos |
| `drm_test`, `sig_test`, `rseq_test`, `timer_test`, `thread_sig_test` | ALL PASS | **ALL PASS** |
| `clipboard-x11.sh` | pasa | **pasa** (133 s) |
| `primary-icons.sh` (`-t 600`) | pasa | **pasa** (136 s) |

Sin pánicos ni avisos nuevos en los logs. La prueba K6 no omite nada con linux-firmware descargado (el bounce con `edu` encuentra memoria por encima de 256 MiB reservando bloques de 2 MiB).

## 5. Qué no se ha podido comprobar

- **Alias de tipos de memoria en WC**: `dma_alloc_wc`/`vmap` con `pgprot_writecombine` mapean WC en la región vmap mientras la ventana directa de Aurora sigue WB para esas páginas. x86 lo desaconseja (Linux cambia también la ventana); en QEMU no se nota. Pendiente para cuando haya GPU real (L7) o L9.
- `VM_DONTCOPY`/`VM_WIPEONFORK` y `vm_ops->open` que falla en `fork` (camino escrito, sin prueba).
- `vm_mm` del VMA del hijo tras `fork` queda a `NULL` hasta que el driver vuelva a pasar por la capa.
- I²C de 10 bits, `I2C_M_RECV_LEN` y estirado de reloj real (la EEPROM simulada no estira).
- `request_firmware_nowait` sin `uevent`; ficheros comprimidos de linux-firmware.
- KVM y hardware real.

## 6. Contradicciones encontradas (planes frente al código)

- `Plan_LinuxKPI.md` §1: «`request_firmware` sí (`firmware.h`)». Existía el cargador, pero nada copiaba linux-firmware al initrd (solo `aurora/selftest.bin`); ahora `firmware.list`.
- `Plan_LinuxKPI.md` §1: «ABI Linux hacia el userland (ioctl, mmap, epoll)». `vfs_ops` no tenía `llseek`, `fasync` ni operaciones por fd, `read`/`write` troceaban por un buffer del kernel y `VMA_MMIO` no guardaba referencia al fichero ni avisaba al desmapear (§2).
- `Plan_LinuxKPI.md` §1: «DRM … PRIME»: solo para GEM de `kernel/drm`; ahora también para páginas ajenas.
- `Plan_LinuxKPI.md` §5: «VFS: `cdev` genérico + `file_operations` completo (`fault`, `llseek`, `poll`, `fasync`)»: hecho (L4), con `fault` en `VMA_DEV`.

## 7. Pendiente y decisiones abiertas

1. **L5 (DRM/KMS sobre la capa)**: espera confirmación.
2. WC sin alias (cambiar la ventana directa al reservar memoria WC) — L7/L9.
3. `dma_resv` con fences (K7, L5); el fence implícito del dmabuf de Aurora no se conecta todavía con el de un `dma_buf` de Linux.
4. `poll` de Aurora sin filtrar `revents`, y `/dev/kmsg` (lectura incremental que se salta líneas; un `DEBUG` por cada `read` que alimenta el propio anillo) (§3.2, §3.3): fallos de Aurora fuera del alcance de L4; se pueden arreglar aparte si el autor quiere.
5. `/dev/i2c-N` (i2c-dev), uevents hacia el userland (L5), atributos binarios de sysfs.
6. Las entradas de debugfs, los nodos de `/dev` borrados y los `task_struct`/`mm_struct` de tareas muertas se liberan al descargar la capa (acotado; evita carreras con lookups de Aurora en curso).
7. `seq_file` genera la salida completa en cada lectura desde el offset 0 (vale para debugfs; no para ficheros grandes en streaming).
8. De sesiones anteriores: `on_each_cpu` (L7), ECAM (L7/L8), vectores MSI (L7/L8), módulos > 64 MiB (L8), decisión NVIDIA.

## 8. Commits de la sesión

| Commit | Contenido |
|---|---|
| `04d085a0` | Plan §11: decisiones de diseño de L4 |
| `a2342d9d` | Aurora: operaciones con fd en `vfs_ops`, `F_SETOWN` y `O_ASYNC` |
| `d6d3b624` | Aurora: `VMA_DEV`, `mprotect` conserva `PTE_SPECIAL`, `task_info_get`, `signal_unblocked_of` |
| `ba614a25` | Aurora: región `vmap`, dmabuf sobre páginas ajenas, exports de L4 |
| `c6385383` | Aurora: `llseek` con el offset actual, `vfs_node_ops`, SIGIO por dueño |
| `355ea121` | Capa: K8 (task_struct, señales, ficheros, `/dev`, mmap) |
| `01b95c5c` | Capa: DMA definitivo, scatterlist, `dma_pool`, `vmap`, `dma_buf` |
| `a0f18775` | Capa: K6 básico (firmware, i2c con algo-bit, debugfs, backlight) |
| `10d5486c` | Capa: números de señal y `poll` filtrado |
| `42791d7b` | Prueba K6 en la suite del kernel |
| `5fd8644f` | Hito: `/dev/lkpi_test` y `user/musl/apps/lkpi_test` |
| `166f697e` | `lkpi_test` espera a la suite por `/proc/modules` (no por `/dev/kmsg`) |
| (este commit) | Este registro, Plan (§1, §5, §10, §11, estado) y D5 |

## 9. Notas para la siguiente sesión (entorno)

- Lo de `docs/LinuxKPI_Sesion_L3.md` §11 sigue valiendo (`musl-tools`, `make iso`, un QEMU cada vez, `pgrep -f`). En un contenedor nuevo hacen falta además `rsync` (para `python-stdlib` de `make iso`) y `musl-tools` (a veces exige `apt-get update` antes).
- **linux-firmware**: `scripts/fetch-linux-drivers.sh --firmware --no-nvidia` (≈ 641 MiB de descarga, 2 GiB extraído en `build/linuxkpi_src/`). Sin él, la prueba del fichero real se omite.
- **El hito** se lanza con `scripts/qemu-drive.py -t 1200 -m 6G --boot-test /usr/bin/lkpi_test --done "LKPI-TEST:"`: espera a que acabe la suite del kernel (que también carga y descarga la capa; la ve en `/proc/modules`), así que en la misma ejecución salen las dos (≈ 95 s).
- La suite del kernel tarda ahora ≈ 50 s de reloj en este entorno (antes ≈ 6 min).
- **Entorno automático** (añadido al cerrar la sesión): `scripts/claude-env-setup.sh` instala los paquetes del host (incluidos `rsync` y `musl-tools`, que ahora también pide `scripts/setup-env.sh`), prepara la pila Wayland y descarga Linux 6.18.59 y linux-firmware 20261019. Sirve como «Setup script» del entorno de Claude Code (si el repositorio aún no está clonado, solo instala paquetes) o a mano dentro del repositorio; variables `AURORA_NO_FIRMWARE=1`, `AURORA_NO_LINUX=1`, `AURORA_NO_WAYLAND=1`.
- **Siguiente sesión**: el prompt de L5 está en `docs/Prompt_L5.md`.
- **Merge con `main`** (al cerrar la sesión, a petición del autor): primero `main` en la rama (dos commits de documentación: alcance de la capa y portátil Haswell; sin conflictos) y después la rama en `main` por avance rápido.

