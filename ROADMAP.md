# Aurora OS — Roadmap

Roadmap de alto nivel. El estado se basa en la implementación actual del repositorio, no en objetivos antiguos.

## Estado actual

Aurora OS ya dispone de una base de kernel x86_64 bare-metal bastante completa:

- [x] UEFI + ELF64 + framebuffer/GOP
- [x] GDT/IDT/ISR/IRQ + manejo de excepciones
- [x] PMM + buddy + heap + SLAB
- [x] Paging de 4 niveles + espacios de usuario + demand paging
- [x] NX + SMEP + SMAP + uaccess
- [x] Scheduler preemptivo + wait queues + procesos/hilos
- [x] SMP con ACPI/MADT, LAPIC/IOAPIC, AP startup, IPIs y per-CPU
- [x] PS/2, PCI, AHCI/ATA, block layer
- [x] Partition layer (MBR + GPT + EBR chain)
- [x] VFS con mount points
- [x] FAT32 persistente (mount, read, write, create, mkdir, unlink, truncate, readdir, rename, LFN)
- [x] FAT32 buffer cache LRU
- [x] Ring 3 + ELF loader + syscalls + libc propia parcial
- [x] ABI Linux x86_64 dual (Linux + Aurora)
- [x] **Enlazado dinámico** (PT_INTERP + ld-musl-x86_64.so.1 cargado en runtime)
- [x] Binarios dinámicos compilados con musl ejecutándose en Aurora
- [x] `execve` in-place + `wait4/rusage` + variables de entorno
- [x] Pipes + `dup2` + redirecciones + pipelines
- [x] TTY real + line discipline + `poll/select`
- [x] PTYs (master/slave completo + /dev/ptmx + /dev/pts/N)
- [x] **Job control completo** (Ctrl+Z, SIGTSTP/SIGCONT, fg, bg, jobs, WUNTRACED/WCONTINUED)
- [x] `busybox sh` interactivo (build dinámico con ~200 applets)
- [x] Permisos POSIX reales (mode/uid/gid por nodo, umask, chmod/chown, setuid/setgid en execve)
- [x] `/etc/passwd`, `/etc/group`, `/etc/shadow`
- [x] rlimits por proceso
- [x] IPC por mailboxes
- [x] Framebuffer compositor + window server + terminal gráfico
- [x] Framework de tests del kernel + regresiones userland (141 tests)
- [x] CI de build y boot con QEMU, incluyendo 1/2/4 CPU
- [x] Syscalls Linux: `futex`, `clone(CLONE_THREAD)`, `flock`, `klogctl`, `waitid`, `setdomainname`
- [x] `pthread_create` / `pthread_join` funcional (stress 50 iteraciones, 60 clone events sin PF)
- [x] Deadlock del TLB shootdown SMP resuelto (bitmask `tlb_pending` + polling en vez de IPI)
- [x] **Port glibc maduro**: `/proc/vmstat`, `/proc/<pid>/stat` 52
      campos, `/proc/<pid>/status` completo, `/proc/<pid>/{cgroup,ctty}`,
      `statx`, `exe_path`+`exe_file`+`vma->file_node`, VMA-per-PT_LOAD,
      `/etc/ld.so.cache`, bind mount `/tmp` → `/data/tmp`.
- [x] **Interactivas**: `less`, `nano`, `ed` funcionan end-to-end
      (incluido Ctrl+O en nano y heredoc en ed).
- [x] **FAT32 estable bajo tests de truncate**: corregido el bug de
      `vfs_node_free` que filtraba nodos y hacía que el SLAB reciclara
      `fat32_fs_t` ya liberados (hang silencioso).

La prioridad ahora es completar userland y filesystem, después networking/USB, y seguir validando Aurora OS sobre hardware real. En paralelo, el objetivo a más largo plazo del proyecto es que Aurora sea tan abierto y eficiente como Linux pero tan "todo hecho" como Windows: eso implica un BusyBox completo como base de userland (3.5), una capa de compatibilidad para ejecutar binarios Linux/ELF reales sin recompilar (3.6), y — de forma más experimental y acotada — un loader de ejecutables Windows/PE de consola (3.7). Ver el resumen de prioridad relativa al final de la Fase 3.

---

# Fase 1 — Robustez del kernel

Objetivo: eliminar clases de bugs antes de seguir añadiendo grandes subsistemas.

### 1.1 Auditoría profunda de memoria y paging
- [x] Revisar todas las APIs de paging para overflow, canonicalidad, permisos y rollback.
- [x] Auditar creación/destrucción de tablas intermedias. (C1: paging_lock global)
- [x] Revisar correctamente los flags USER, WRITABLE, NX y páginas huge durante errores parciales. (H2: split_huge_page conserva NX)
- [x] Auditar PMM frente a mapas EFI malformados: tamaños, strides, límites y overflow.
- [x] Revisar fugas y dobles liberaciones en todas las rutas de error. (H5: detección de doble free)
- [x] Añadir tests de fault injection donde sea útil.

### 1.2 Auditoría SMP y concurrencia
- [x] Revisar todos los locks y su orden global.
- [x] Buscar deadlocks entre scheduler, VFS, winsrv, PMM y paging.
- [x] Auditar accesos a estado compartido sin lock. (C3: need_resched con xchg; C4: state+deadline atómicos)
- [x] Revisar IRQ/preemption contexts.
- [x] Revisar TLB shootdown y cambios de address space concurrentes. (H3: ipi_tlb_shootdown)
- [x] **Deadlock de `ipi_tlb_shootdown` con dos CPUs concurrentes**. Cazado con `dmesg | wc -c` (ld.so hace `mprotect(RELRO)` en dos procesos a la vez). Fix: bitmask `tlb_pending` (un bit por CPU) + `tlb_service` por polling; cada CPU que gira en `tlb_shootdown_lock` atiende su propia invalidación sin depender de recibir la IPI con IRQs on. `preempt_disable` durante el protocolo.
- [x] Revisar lifecycle de task_t y process_t.
- [x] Revisar wait queues bajo carreras de wake/block/timeout. (C4)
- [x] Resolver race de migración de tasks entre CPUs en `wait_common` + `task_switch` (corregido con publicación atómica de `current_task`/`need_resched` y transición atómica de `on_cpu`; regresión con migración y stack canary).
- [ ] Documentar invariantes importantes del scheduler y memoria.

**Deuda técnica restante de 1.2:**
- [x] `sched_kick_idle_cpu`: leer `per_cpu(current_task, cpu)` con `__atomic_load_n(ACQUIRE)`.
- [x] `sched_tick`: publicar `this_cpu(current_task)` con `__atomic_store_n(RELEASE)`.
- [x] `need_resched`: publicación/consumo atómico con ACQUIRE/RELEASE + exchange.
- [x] `on_cpu`: transición de context switch publicada atómicamente con `xchg`.
- [x] Test de regresión: task migrando con `cpu_affinity=-1` y stack canary verificado tras la migración.
- [x] `isr_handler` usaba un `registers_t` static compartido entre CPUs. Reemplazado por el frame propio de cada invocación.
- [x] Ventana física de RAM en Write-Back (antes NOCACHE causaba aliasing UC/WB sobre los mismos frames cuando el proceso mapeaba RAM con WB).

### 1.3 Testing y CI
- [x] Mantener regresiones para cada bug crítico corregido.
- [x] Ampliar la matriz de QEMU/CPU y configuraciones relevantes (1/2/4 CPU, KVM, TCG, VirtualBox; AHCI y PIIX3).
- [x] Añadir tests de stress de scheduler, memoria, IPC y VFS (AHCI stress 1MB, multi-LBA, buffer no contiguo).
- [x] Añadir pruebas de errores y recursos agotados. (doble free, remap, late free)
- [x] Añadir tests de corrupción FAT32 con mock block device (BPB fuzz, ciclos, out-of-range, huérfanos, E/S durante create).
- [x] Regresión de concurrencia SMP: lanzar `dmesg | wc -c` en bucle (15 iteraciones × 3 paralelos) con 2 y 4 CPUs.
- [ ] Evitar que los tests dependan accidentalmente del orden de ejecución.
- [ ] Mejorar diagnósticos de CI cuando QEMU falle.

### 1.4 Debugging
- [ ] Mejorar dumps de scheduler/procesos.
- [ ] Añadir nombres legibles a tareas/procesos.
- [ ] Revisar el comportamiento de panic cuando existen locks tomados.
- [ ] Hacer los dumps de panic seguros en SMP. (panic_handler actual solo detiene el CPU que falla)
- [ ] Mejorar backtraces y diagnóstico de page faults.

---

# Fase 2 — Storage persistente

Objetivo: pasar de un sistema que carga un initrd a un sistema operativo que puede conservar datos.

### 2.1 Block layer
- [x] Abstracción de dispositivos de bloque.
- [x] BIO/read/write/completions.
- [x] Integración AHCI/ATA.
- [ ] Revisar exhaustivamente errores, timeouts y recuperación de dispositivos.
- [x] Añadir cache/buffer cache si resulta necesario. (FAT32 buffer cache LRU de 256 sectores × 512 B, integrado en fat32.c)

### 2.2 Sistema de archivos persistente
- [x] Implementar FAT32.
- [x] Lectura de particiones/volúmenes (part.c: MBR + GPT + EBR chain).
- [x] Directorios reales.
- [x] Crear/eliminar archivos y directorios (mkdir, unlink).
- [x] Rename (mismo FS, mismo dir y entre dirs; con LFN).
- [x] Escritura y truncado.
- [x] Montaje/desmontaje.
- [x] Integrarlo con VFS (mount points con longest-match).
- [x] Caché de FAT en memoria (rendimiento read/write).
- [x] Tests de corrupción, límites y apagado durante escritura.
- [x] LFN (nombres largos) en FAT32: crear, listar, borrar, preservar tras remount.
- [x] Colisión de alias 8.3: crear `DOCUME~1` después de `Documentos` no pisa el alias autogenerado.
- [x] fsck mínimo (`/apps/aurora-fsck`) + validación pasiva al montar + detección de huérfanos/ciclos/cadenas rotas + actualización de FSInfo en sync.

### 2.3 Userland sobre disco
- [ ] Acceso real a /dev desde userland.
- [x] `ls` (lee directorios vía readdir, respeta cwd).
- [x] `cat` (abre y lee archivos).
- [x] `mkdir`, `rm`, `cp`, `mv`, `pwd` como apps userland con argv.
- [x] Persistir configuración y programas.
- [ ] Boot desde un filesystem persistente: el bootloader sigue leyendo `/kernel.elf` desde la ruta fija de FAT32; sería necesario un cargador que lea un `/etc/aurora.conf` con la ruta del kernel o migrar a un formato de arranque configurable.

**Deuda técnica de 2.3 — Loader.**
Resuelto: `elf_load_streaming` lee el ELF en chunks vía callback. `mmap` file-backed y `VMA_FILE` operativos. Enlazado dinámico completo (PT_INTERP).

---

# Fase 3 — Userland y modelo de sistema

Objetivo: convertir el kernel en una plataforma para aplicaciones.

### 3.1 libc y ABI Linux

- [x] Definir ABI Linux x86_64 en paralelo a la ABI Aurora.
- [x] Dispatcher dual de syscalls.
- [x] `arch_prctl(SET_FS/GET_FS)` + `fs_base` per-task.
- [x] Layout Linux de `struct stat`.
- [x] `writev/readv`, `fcntl`, `getdents64` y primeras syscalls POSIX/Linux.
- [x] Primer binario estático compilado con musl ejecutándose en Aurora.
- [x] Apps básicas compiladas con musl.
- [x] Utilidades adicionales compiladas con musl.
- [x] Enlazado dinámico: PT_INTERP + `ld-musl-x86_64.so.1` + `AT_BASE` real + ejecución de binarios PIE.
- [ ] Ampliar la libc propia.
- [ ] Completar `errno`, tiempo, procesos, archivos y memoria.
- [ ] Headers y ABI estable documentados.
- [ ] Compatibilidad POSIX más amplia.

### 3.2 Procesos, ejecución y shell

- [x] Argumentos en `spawn`/`exec`.
- [x] cwd por proceso y herencia.
- [x] `execve` in-place con nuevo address space.
- [x] `wait4` con `struct rusage` y contabilidad de CPU por proceso.
- [x] Variables de entorno: `envp`, `environ`, `getenv/setenv`.
- [x] `MAP_FIXED` y `next_mmap_addr` por proceso.
- [x] Pipes anónimos, `dup2` y asignación de FDs al hijo.
- [x] Redirecciones `<`, `>`, `>>` y pipelines.
- [x] TTY real con line discipline canonical/raw.
- [x] `poll`, `select`, `pselect6`, `ppoll` y `nanosleep`.
- [x] ioctls básicos de TTY y `/dev/tty`.
- [x] `busybox sh` interactivo mediante fork/exec.
- [x] Fuente antialiased y mejoras ANSI de la consola.
- [x] PTYs (`/dev/ptmx`, `/dev/pts/N`, TIOCGPTN, TIOCSPTLCK, EOF del master tras cierre del slave).
- [x] Job control completo (Ctrl+Z, SIGTSTP/SIGCONT dentro del wait, WUNTRACED/WCONTINUED, SIGCHLD al padre en exit, shell interactivo `jobs`/`fg`/`bg`/`kill %N`).
- [x] Background/jobs del shell.
- [ ] Señales POSIX completas.
- [x] Historial y autocompletado (vía busybox `FEATURE_EDITING` + `FEATURE_TAB_COMPLETION`).

### 3.3 VFS y /dev
- [x] /proc para observabilidad (`/proc/{uptime,version,meminfo,stat,mounts,loadavg,self}` + `/proc/<pid>/{stat,status,cmdline,comm,statm}`).
- [x] /dev/kmsg.
- [x] /dev/null, /dev/zero y dispositivos básicos con `rdev` correcto.
- [x] Permisos y metadatos de archivos (mode/uid/gid por nodo, umask, chmod/chown, setuid/setgid en execve).
- [x] Layout final: tarfs RO en `/`, FAT32 RW en `/data`, devfs en `/dev`, procfs en `/proc`.
- [ ] `pivot_root`/`switch_root` genéricos: hoy el layout es fijo; sería útil soportar cambiar la raíz en runtime tras montar otro FS. (`vfs_pivot_root` está implementado pero no se usa porque el modelo LiveUSB no lo necesita.)
- [x] /proc para observabilidad: `/proc/{uptime,version,meminfo,stat,
      mounts,loadavg,vmstat,filesystems,partitions,swaps,self}` y
      `/proc/<pid>/{stat,status,cmdline,comm,statm,maps,smaps,mountinfo,
      cgroup,ctty}`.
- [x] **`/proc/<pid>/stat` con 52 campos** al layout Linux x86_64
      (necesario para libproc2 / top / htop).
- [x] **`/proc/<pid>/status` extendido**: VmPeak/HWM/Size/RSS/Data/
      Stk/Exe/Lib, SigQ/Pnd/Blk/Ign/Cgt, Cpus_allowed, Mems_allowed,
      Groups, FDSize.
- [x] **`/proc/<pid>/cgroup`** (`0::/`) y **`/proc/<pid>/ctty`**
      (symlink al tty de control).

### 3.4 Shell
- [x] Parser ANSI CSI en la consola gráfica: SGR (colores + bold), `K`/`J` con sus tres modos, `H`/`D`/`C`/`G`/`P`. Distingue `\033[J` local (post-backspace) de `\033[H\033[J` global (patrón de `clear` de busybox con `TERM=linux`).
- [x] Historial (busybox `FEATURE_EDITING_HISTORY`, `FEATURE_REVERSE_SEARCH`).
- [x] Autocompletado (busybox `FEATURE_TAB_COMPLETION`, `FEATURE_USERNAME_COMPLETION`).
- [x] Variables de entorno.
- [x] Pipes/redirecciones.
- [x] Jobs/background.
- [ ] Mejor manejo de errores.
- [x] Bind mount `/tmp` → `/data/tmp` para que `tmpfile()`/`mkstemp()`
      de glibc tenga un directorio RW real.
- [x] `TMPDIR` exportado en el env de init.

### 3.5 BusyBox completo

Objetivo: convertir BusyBox en una base de userland mucho más completa.

Estado actual:
- [x] `busybox sh` interactivo (binario dinámico, ~200 applets).
- [x] Ejecución de applets externos mediante `fork` + `execve`.
- [x] Build dinámico (`CONFIG_STATIC=n`, `CONFIG_PIE=y`).
- [x] `FEATURE_DEVPTS`, `CTTYHACK`.
- [x] `USE_BB_PWD_GRP`, `USE_BB_SHADOW`.
- [x] Editors y procesado de texto: `vi`, `ed`, `sed`, `awk`, `grep`, `diff`, `patch`, `cmp`.
- [x] Pagers: `less`, `more` (con scroll, search, marcas).
- [x] Grabación de sesión: `script`, `scriptreplay`.
- [x] Gestión de procesos: `pgrep`, `pkill`, `pidof`, `fuser`, `ps -l`, `top`.
- [x] TTY: `stty`, `setsid`, `hostname`, `watch`.
- [x] Binario: `hexedit`, `xxd`, `hexdump`, `od`, `strings`.
- [x] Pipes y redirecciones necesarios para el shell.
- [x] `futex` (WAIT/WAKE), `clone(CLONE_THREAD)`, `flock`, `klogctl` (dmesg), `waitid`, `setdomainname`.
- [ ] Completar superficie syscall/libc para un build BusyBox completo (queda `chroot`, `futex` robusto, `clone` con más flags).
- [ ] `stat/fstat/lstat` y metadatos completos.
- [ ] `dup/dup2/pipe/fcntl` con semántica suficiente para más applets.
- [ ] `ioctl` de terminal más completo.
- [ ] `chmod/chown/umask` coherentes con el modelo de permisos (hecho a nivel de kernel; falta exponer más a userland).
- [x] `symlink/readlink`.
- [x] `sync`, timestamps.
- [x] `/proc` mínimo para applets como `ps`, `top`, `free`.
- [x] Job control y PTYs.

### 3.6 Ejecutar binarios Linux (ELF) — "personalidad Linux"
Objetivo: ejecutar binarios ELF reales de Linux (BusyBox oficial, coreutils, bash, etc.) sin recompilarlos contra la ABI de Aurora, mediante una capa de traducción de syscalls — el mismo enfoque que WSL1 (Linux sobre un kernel no-Linux) en vez de un hipervisor o VM.

**Estado**: la infraestructura de enlazado dinámico (PT_INTERP, AT_BASE, ld.so cargado en runtime) ya está operativa con binarios compilados para musl. Falta la capa de traducción para binarios de distros Linux (glibc).

**Alcance recomendado para empezar: solo binarios estáticos musl.** Excluye enlazado dinámico de distros glibc del alcance inicial y reduce el problema a "traducir syscalls", que es abarcable.

- [ ] Detectar un ELF "Linux" (vs. un ELF nativo de Aurora) por `e_ident`/`.note.ABI-tag` o por el `PT_INTERP`.
- [ ] Convención de syscall de Linux x86_64 (ya implementada en el dispatcher dual).
- [ ] Tabla de traducción `syscall_linux_nr → función interna de Aurora` (parcial: la mayoría de las comunes ya está).
- [x] `struct stat` con el layout exacto de Linux x86_64.
- [x] Mapeo de `errno`.
- [x] `futex()` (WAIT/WAKE, PRIVATE).
- [x] `clone()` con flags de musl para pthread (CLONE_VM|CLONE_THREAD|CLONE_SETTLS|...).
- [x] `pthread_create` / `pthread_join` / `pthread_mutex` / `pthread_cond` operativos.
- [ ] Suite de smoke tests con binarios reales.
- [x] `statx` (332) completo.
- [x] `/etc/ld.so.cache` real, generado en build time.
- [x] `O_TMPFILE` → `-ENOENT` para que glibc caiga a `mkstemp`.
- [x] `exe_path` + `exe_file` + `vma->file_node` en `process_t`.
- [x] VMA-per-PT_LOAD: `maps`/`smaps` con permisos reales por segmento.

### 3.7 Ejecutar ejecutables Windows (PE/COFF) — expectativas realistas
*(sin cambios, sigue pendiente)*

**Prioridad relativa de 3.5/3.6/3.7:** BusyBox completo (3.5) ya está prácticamente cerrado. La personalidad Linux (3.6) da más beneficio por esfuerzo invertido que el loader de PE (3.7). Recomendación: 3.5 → 3.6 → 3.7.

---

# Fase 4 — Gráficos y escritorio

Objetivo: pasar del compositor/terminal actual a un entorno gráfico usable.

### 4.1 Window manager
- [ ] Resize de ventanas.
- [ ] Z-order robusto.
- [ ] Focus/input routing más completo.
- [ ] Minimizar/maximizar.
- [ ] Gestión de ventanas cerradas y procesos muertos.
- [ ] Workspaces/escritorios virtuales.

### 4.2 Input
- [ ] Mejorar cursor.
- [ ] Soporte de mouse más completo.
- [ ] Atajos globales.
- [ ] Selección de texto.
- [ ] Copy/paste y clipboard.
- [ ] Multi-consola/TTY.

### 4.3 Rendering
- [ ] Ring buffer de output en vez de un evento por byte.
- [ ] Fuentes TTF/bitmap más completas.
- [ ] Cursor parpadeante.
- [ ] Terminal 2D: matriz de celdas + parser CSI completo + scroll region + alternate screen. Desbloquea `vi`, `less`, `top` plenamente.
- [ ] Mejorar composición y rendimiento.
- [ ] Separar claramente primitivas de dibujo, compositor y window server.

### 4.4 Aceleración
- [ ] Definir una abstracción gráfica/GPU.
- [ ] Investigar framebuffer double buffering.
- [ ] Driver GPU/DRM-KMS cuando exista una base estable para ello.
- [ ] Aceleración 2D/3D solo después de estabilizar el escritorio.

---

# Fase 5 — Networking
*(sin cambios)*

---

# Fase 6 — USB y hardware moderno
*(sin cambios)*

---

# Fase 7 — Seguridad

### 7.1 Protección del kernel
- [x] NX.
- [x] SMEP.
- [x] SMAP.
- [x] Validación de accesos user/kernel.
- [ ] Stack canaries.
- [ ] CFI o mecanismo equivalente.
- [ ] ASLR/KASLR.
- [ ] Hardening adicional de syscalls.
- [ ] Parcheo quirúrgico de `stac`/`clac` en `uaccess_init`.

### 7.2 Modelo de seguridad
- [x] Usuarios y grupos (`/etc/passwd`, `/etc/group`, `/etc/shadow`, credenciales POSIX completas por proceso, uid/gid por nodo).
- [x] Permisos de archivos (mode por nodo, umask, chmod/chown, vfs_check_access en open/execve/access/chdir).
- [ ] ACLs si son necesarias.
- [ ] Capabilities/sandboxing.
- [ ] Aislamiento más fuerte de servicios.
- [ ] Definir modelo de privilegios para procesos del sistema.

### 7.3 Criptografía
*(sin cambios)*

---

# Fase 8-11
*(sin cambios)*

---

# Orden recomendado de alto nivel

1. **Robustez del kernel + auditoría SMP** — ✅ completada.
2. **Storage persistente + filesystem** — ✅ cerrado (FAT32 persistente con LFN, rename, buffer cache, tests de corrupción, fsck mínimo).
3. **Userland/libc/shell** — ⏳ en progreso avanzado. Cerrado:
   ABI Linux, glibc dinámico, TTY, pipes, PTYs, job control, permisos
   POSIX, `/etc/passwd`, BusyBox dinámico (~200 applets), futex+clone,
   `flock`, `dmesg`, `statx`, `/proc` extendido, VMA-per-PT_LOAD,
   `/etc/ld.so.cache`, bind `/tmp`, interactivas (`less`/`nano`/`ed`).
   Siguiente: `epoll`+`eventfd`, luego `perl`/`python3`.
4. **Escritorio y window manager** — pendiente; el siguiente salto grande es el terminal 2D.
5. **Networking**
6. **USB**
7. **Seguridad avanzada**
8. **Drivers/hardware adicional**
9. **Performance/NUMA/escalabilidad**
10. **SDK/toolchain/ecosistema**
11. **BusyBox completo + personalidad Linux (ELF)** — la parte BusyBox está hecha; la personalidad Linux (3.6) es el siguiente bloque de peso.
12. **Compatibilidad PE/Windows (experimental)**

---

## Objetivos de largo plazo

- [x] Aurora OS arranca de forma fiable en QEMU con 1/2/4 CPU.
- [~] Aurora OS puede instalarse/arrancar desde almacenamiento persistente. (FAT32 es la raíz del VFS; el bootloader sigue cargando el kernel desde ruta fija. Falta instalador + boot configurable.)
- [x] Aurora OS puede crear, modificar y conservar archivos.
- [x] Aurora OS puede ejecutar múltiples aplicaciones aisladas.
- [x] Aurora OS dispone de terminal y escritorio utilizables.
- [ ] Aurora OS puede comunicarse por red.
- [ ] Aurora OS puede utilizar teclado/ratón USB.
- [~] Aurora OS dispone de un modelo de seguridad coherente. (Usuarios y permisos POSIX funcionando; falta sandboxing/capabilities.)
- [ ] Aurora OS tiene SDK/documentación suficientes para desarrollar aplicaciones de terceros.
- [~] Aurora OS ejecuta un userland tipo BusyBox completo. (`busybox sh` dinámico con ~200 applets, futex+clone, pthreads funcional; falta superficie de syscalls para builds completos.)
- [ ] Aurora OS puede ejecutar binarios Linux (ELF) reales sin recompilar (estáticos/musl primero, ver 3.6). La infraestructura de enlazado dinámico ya existe.
- [ ] Aurora OS puede ejecutar un subconjunto acotado de ejecutables Windows de consola (PE/x64).

> **Principio:** priorizar primero corrección, aislamiento y observabilidad; después funcionalidad; y finalmente optimización. Cada bug importante corregido debería, cuando sea posible, quedar acompañado de una regresión automatizada.