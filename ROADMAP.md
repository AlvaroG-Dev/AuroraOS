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
- [x] FAT32 persistente (mount, read, write, create, mkdir, unlink, truncate, readdir)
- [x] Ring 3 + ELF loader + syscalls + libc propia parcial
- [x] ABI Linux x86_64 dual (Linux + Aurora)
- [x] Primeros binarios estáticos compilados con musl
- [x] `execve` in-place + `wait4/rusage` + variables de entorno
- [x] Pipes + `dup2` + redirecciones + pipelines
- [x] TTY real + line discipline + `poll/select`
- [x] `busybox sh` interactivo
- [x] IPC por mailboxes
- [x] Framebuffer compositor + window server + terminal gráfico
- [x] Framework de tests del kernel + regresiones userland
- [x] CI de build y boot con QEMU, incluyendo 1/2/4 CPU

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

### 1.3 Testing y CI
- [x] Mantener regresiones para cada bug crítico corregido.
- [x] Ampliar la matriz de QEMU/CPU y configuraciones relevantes (1/2/4 CPU, KVM, TCG, VirtualBox; AHCI y PIIX3).
- [x] Añadir tests de stress de scheduler, memoria, IPC y VFS (AHCI stress 1MB, multi-LBA, buffer no contiguo).
- [x] Añadir pruebas de errores y recursos agotados. (doble free, remap, late free)
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
- [ ] Añadir cache/buffer cache si resulta necesario.

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
- [ ] Tests de corrupción, límites y apagado durante escritura (parcial: tests de límite 8.3, ENOTEMPTY, non-FAT rechazo).
- [x] LFN (nombres largos) en FAT32: crear, listar, borrar, preservar tras remount.
- [ ] Colisión de alias 8.3: crear `DOCUME~1` después de `Documentos` puede pisar el alias autogenerado. Preexistente, no urgente.

### 2.3 Userland sobre disco
- [ ] Acceso real a /dev desde userland.
- [x] `ls` (lee directorios vía readdir, respeta cwd).
- [x] `cat` (abre y lee archivos).
- [x] `mkdir`, `rm`, `cp`, `mv`, `pwd` como apps userland con argv.
- [x] Persistir configuración y programas. (FAT32 montado en /; apps del sistema en /initrd, datos del usuario en /).
- [ ] Boot desde un filesystem persistente: el bootloader sigue leyendo `/kernel.elf` desde la ruta fija de FAT32; sería necesario un cargador que lea un `/etc/aurora.conf` con la ruta del kernel o migrar a un formato de arranque configurable.

**Deuda técnica de 2.3 — Loader.**
Resuelto parcialmente: `elf_load_streaming` lee el ELF en chunks vía callback (`read(ctx, offset, size, buf)`), así que no necesita buffer contiguo de 16 MB. `process_load_with_ppid` ya lo usa. Pendiente:
- **mmap file-backed**: en vez de copiar las páginas del ELF al address space del hijo, mapearlas directamente al fichero con un VMA de tipo `VMA_FILE`. Modelo Linux; desbloquea `dlopen`, `mmap(PROT_EXEC)` de ficheros y carga perezosa. Siguiente PR de Fase 3.5+.

La opción 2 es la correcta a largo plazo. La 1 es un parche si hace falta cargar ELFs grandes pronto.

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
- [x] Utilidades adicionales compiladas con musl: `wc`, `head`, `grep`, `tee`, `sort`, `cp`, `mv`, `rm`, `mkdir`, `kill`, `calc`.
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
- [ ] PTYs.
- [ ] Job control completo.
- [ ] Señales POSIX completas.
- [ ] Background/jobs del shell.
- [ ] Historial y autocompletado.

### 3.3 VFS y /dev
- [ ] /proc para observabilidad.
- [x] /dev/kmsg.
- [x] /dev/null, /dev/zero y dispositivos básicos.
- [ ] Mejorar permisos y metadatos de archivos.
- [x] (Pivot root) FAT32 en `/`, tarfs en `/initrd`. Punto de montaje `/initrd` se crea como dir vacío en FAT32 al arrancar para que `ls /` lo liste (mismo modelo que Linux).
- [ ] `pivot_root`/`switch_root` genéricos: hoy el layout es fijo (FAT32 raíz + tarfs en /initrd); sería útil soportar cambiar la raíz en runtime tras montar otro FS.

### 3.4 Shell
- [x] Parser ANSI CSI en la consola gráfica: SGR (colores + bold), `K`/`J` con sus tres modos, `H`/`D`/`C`/`G`/`P`. Distingue `\033[J` local (post-backspace) de `\033[H\033[J` global (patrón de `clear` de busybox con `TERM=linux`).
- [ ] Historial.
- [ ] Autocompletado.
- [ ] Variables de entorno.
- [ ] Pipes/redirecciones.
- [ ] Jobs/background.
- [ ] Mejor manejo de errores.

### 3.5 BusyBox completo

Objetivo: convertir BusyBox en una base de userland mucho más completa.

Estado actual:
- [x] `busybox sh` interactivo.
- [x] Ejecución de applets externos mediante `fork` + `execve`.
- [x] Apps/utilidades equivalentes ya portadas a musl: `echo`, `cat`, `ls`, `touch`, `pwd`, `wc`, `head`, `grep`, `tee`, `sort`, `cp`, `mv`, `rm`, `mkdir`, `kill`, `calc`.
- [x] Pipes y redirecciones necesarios para el shell básico.
- [ ] Completar superficie syscall/libc para un build BusyBox amplio.
- [ ] `stat/fstat/lstat` y metadatos completos.
- [ ] `dup/dup2/pipe/fcntl` con semántica suficiente para más applets.
- [ ] `ioctl` de terminal más completo.
- [ ] `chmod/chown/umask` coherentes con el futuro modelo de permisos.
- [ ] `symlink/readlink` o política explícita de no soporte en FAT32.
- [ ] `sync`, timestamps y APIs restantes.
- [ ] `/proc` mínimo para applets como `ps`, `top`, `free`.
- [ ] Job control y PTYs.

### 3.6 Ejecutar binarios Linux (ELF) — "personalidad Linux"
Objetivo: ejecutar binarios ELF reales de Linux (BusyBox oficial, coreutils, bash, etc.) sin recompilarlos contra la ABI de Aurora, mediante una capa de traducción de syscalls — el mismo enfoque que WSL1 (Linux sobre un kernel no-Linux) en vez de un hipervisor o VM.

**Alcance recomendado para empezar: solo binarios estáticos musl.** Excluye enlazado dinámico (`ld.so`, `dlopen`, glibc/NSS) del alcance inicial y reduce el problema a "traducir syscalls", que es abarcable. Enlazado dinámico y glibc pueden ser una fase posterior si hace falta.

- [ ] Detectar un ELF "Linux" (vs. un ELF nativo de Aurora) por `e_ident`/`.note.ABI-tag` o por el `PT_INTERP` (p. ej. `/lib/ld-musl-x86_64.so.1`).
- [ ] Convención de syscall de Linux x86_64: `rax`=número, args en `rdi,rsi,rdx,r10,r8,r9`. Implica un segundo punto de entrada de `syscall`/`int 0x80` con esa convención, separado de la ABI propia de Aurora.
- [ ] Tabla de traducción `syscall_linux_nr → función interna de Aurora`, empezando por el ~5-10% de syscalls que cubren la mayoría de programas reales: `read/write/openat/close/mmap/munmap/brk/exit/exit_group/fork/clone/execve/wait4/getpid/rt_sigaction/rt_sigprocmask/fstat/lseek/ioctl/pipe2/dup2/getcwd/chdir`.
- [ ] `struct stat` con el layout exacto de Linux x86_64 (distinto del de Aurora) para las syscalls `*stat`.
- [ ] Mapeo de `errno` (mayormente ya coincide con POSIX, pero hay que verificar caso a caso).
- [ ] `futex()`: aunque se restrinja a binarios estáticos de un solo hilo al principio, cualquier musl mínimamente reciente lo usa hasta para mutex internos — probablemente sea la primera syscall "rara" que haga falta.
- [ ] `clone()` con las flags que usa musl para `pthread_create` (para cuando se quiera soportar multihilo).
- [ ] Suite de smoke tests: correr binarios estáticos reales (BusyBox oficial, `busybox-w32`-style, algún coreutils estático) y comparar comportamiento con el mismo binario en Linux.

### 3.7 Ejecutar ejecutables Windows (PE/COFF) — expectativas realistas
Objetivo real y acotado: cargar y ejecutar **ejecutables PE de consola, sin GUI**, que dependan de un subconjunto pequeño y bien definido de `kernel32.dll`. Esto **no** es "Windows funcionando" — reimplementar Win32 completo (kernel32+user32+gdi32+ntdll+COM+...) es un proyecto del tamaño de Wine (décadas de trabajo de un equipo grande); no debe tratarse como objetivo alcanzable a medio plazo ni bloquear el resto del roadmap.

- [ ] Parser de PE/COFF: cabeceras DOS/PE, secciones, tabla de importación, relocaciones base.
- [ ] Loader que mapea las secciones al address space del proceso con los permisos (`.text` RX, `.data` RW, etc.) usando la infraestructura de paging/VMA ya existente.
- [ ] "DLL shim" de `kernel32.dll`: en vez de cargar la DLL real de Windows, resolver los imports contra un conjunto pequeño de funciones nativas de Aurora con la misma firma (`CreateFileA`, `ReadFile`, `WriteFile`, `ExitProcess`, `GetStdHandle`, `HeapAlloc`/`HeapFree`, `GetCommandLineA`, poco más al principio).
- [ ] Convención de llamada Win64 (`RCX,RDX,R8,R9` + shadow space de 32 bytes) — distinta de System V; hace falta un trampolín de entrada/salida por cada función shimmeada.
- [ ] Alcance inicial: solo PE/x64, subsistema `CONSOLE`, sin CRT dinámica (o CRT estática) y sin tocar `user32`/`gdi32`/COM. Cualquier `.exe` con interfaz gráfica queda fuera de alcance por ahora.
- [ ] Tratar esto como experimental y de baja prioridad (ver Fase 12) hasta que 3.5/3.6 estén maduros — es la pieza más grande y con menor retorno inmediato de las tres.

**Prioridad relativa de 3.5/3.6/3.7:** BusyBox completo (3.5) es la base y ya está en marcha. La personalidad Linux (3.6) da más beneficio por esfuerzo invertido que el loader de PE (3.7): reutiliza binarios reales del ecosistema Linux con "solo" una capa de traducción de syscalls, mientras que PE exige reimplementar una API entera. Recomendación: 3.5 → 3.6 → 3.7, y no empezar 3.7 en serio hasta tener 3.6 corriendo binarios reales.

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
- [ ] Mejorar composición y rendimiento.
- [ ] Separar claramente primitivas de dibujo, compositor y window server.

### 4.4 Aceleración
- [ ] Definir una abstracción gráfica/GPU.
- [ ] Investigar framebuffer double buffering.
- [ ] Driver GPU/DRM-KMS cuando exista una base estable para ello.
- [ ] Aceleración 2D/3D solo después de estabilizar el escritorio.

---

# Fase 5 — Networking

Objetivo: dotar al sistema de conectividad real.

**Nota:** la auditoría SMP y sus correcciones críticas ya están integradas. Networking queda pendiente por falta de drivers y pila de red, no por el antiguo race de `current_task`.

### 5.1 Hardware
- [ ] Abstracción de NIC.
- [ ] Driver e1000 inicialmente.
- [ ] RX/TX queues.
- [ ] Interrupts/MSI/MSI-X cuando proceda.
- [ ] Loopback.

### 5.2 Pila de red
- [ ] Ethernet.
- [ ] ARP.
- [ ] IPv4.
- [ ] ICMP.
- [ ] UDP.
- [ ] TCP.
- [ ] Checksums y timeouts.
- [ ] DHCP.
- [ ] DNS.

### 5.3 API userland
- [ ] Sockets.
- [ ] bind, listen, accept, connect, send, recv.
- [ ] Cliente HTTP mínimo.
- [ ] Herramientas tipo ping y ifconfig/ip.

IPv6 puede llegar después de estabilizar IPv4.

---

# Fase 6 — USB y hardware moderno

Objetivo: que Aurora OS deje de depender de periféricos PS/2 y pueda manejar hardware moderno.

### 6.1 USB host
- [ ] xHCI.
- [ ] Enumeración de dispositivos.
- [ ] Descriptores.
- [ ] Control/bulk/interrupt transfers.
- [ ] Gestión de endpoints.

### 6.2 USB HID
- [ ] Teclados USB.
- [ ] Ratones USB.
- [ ] Hotplug básico.

### 6.3 USB storage
- [ ] USB mass storage.
- [ ] Integración con block layer.
- [ ] Boot/test desde almacenamiento USB si resulta práctico.

---

# Fase 7 — Seguridad

Objetivo: aumentar el aislamiento y reducir la superficie de ataque.

### 7.1 Protección del kernel
- [x] NX.
- [x] SMEP.
- [x] SMAP.
- [x] Validación de accesos user/kernel.
- [ ] Stack canaries.
- [ ] CFI o mecanismo equivalente.
- [ ] ASLR/KASLR.
- [ ] Hardening adicional de syscalls.
- [ ] Parcheo quirúrgico de `stac`/`clac` en `uaccess_init` (deuda de 1.1: hoy escanea `.text` buscando bytes `0F 01 CB/CA` y podría tener falsos positivos).

### 7.2 Modelo de seguridad
- [ ] Usuarios y grupos.
- [ ] Permisos de archivos.
- [ ] ACLs si son necesarias.
- [ ] Capabilities/sandboxing.
- [ ] Aislamiento más fuerte de servicios.
- [ ] Definir modelo de privilegios para procesos del sistema.

### 7.3 Criptografía
- [ ] Primitivas criptográficas básicas.
- [ ] RNG de calidad.
- [ ] TLS en userland cuando exista red estable.
- [ ] Evaluar filesystem cifrado solo después de disponer de almacenamiento persistente.

Secure Boot puede evaluarse como integración posterior; no debería bloquear el desarrollo del kernel.

---

# Fase 8 — Drivers y plataforma de hardware

Objetivo: ampliar hardware soportado sin convertir el kernel en una colección de drivers aislados.

- [ ] Arquitectura común de drivers.
- [ ] Mejorar abstracción PCI.
- [ ] MSI/MSI-X (hay MSI puntual para AHCI, falta generalizar).
- [ ] HDA/Audio.
- [ ] RTC/clocksource más completo.
- [ ] NVMe.
- [ ] VirtIO donde aporte valor para QEMU.
- [ ] ACPI más completo.
- [ ] Gestión de energía.
- [ ] Suspensión/reanudación si el objetivo del proyecto lo requiere.
- [ ] CPU/memory hotplug si se decide soportarlo.

---

# Fase 9 — Performance y escalabilidad

Objetivo: que el sistema siga siendo razonablemente eficiente al crecer.

- [ ] Medir scheduler y latencias.
- [ ] Benchmarks de allocators.
- [ ] Benchmarks de IPC.
- [ ] Benchmarks de VFS/storage.
- [ ] Mejorar afinidad CPU (hoy kmain_task forzada a CPU 0 como mitigación).
- [ ] Per-CPU allocators donde sean útiles.
- [ ] Reducir contención global (paging_lock es global; posibles per-PML4 o per-PDPT).
- [ ] RCU u otros mecanismos solo donde exista una necesidad medida.
- [ ] NUMA si el hardware objetivo lo justifica.
- [ ] Huge pages correctamente integradas.
- [ ] vmalloc/virtual allocations para objetos grandes si el límite actual del heap sigue siendo una restricción.
- [ ] Buddy allocator: free lists por orden en vez de scan lineal sobre bitmap.
- [ ] Migrar LAPIC MMIO → x2APIC MSR para reducir latencia de IPIs.

---

# Fase 10 — Toolchain y SDK

Objetivo: que desarrollar aplicaciones para Aurora sea cómodo.

- [ ] SDK propio.
- [ ] Headers completos.
- [ ] libc más completa.
- [ ] Toolchain reproducible.
- [ ] Linker scripts/documentación estable.
- [ ] Debugging userland con GDB.
- [ ] Symbolication y mejores backtraces.
- [ ] Build system más sencillo para aplicaciones.
- [ ] Plantilla para crear aplicaciones.
- [ ] Package manager/repository solo cuando exista un filesystem y red suficientemente maduros.

No es necesario crear un compilador propio: inicialmente debe usarse LLVM/GCC/binutils existentes.

---

# Fase 11 — Calidad del proyecto

Objetivo: mantener el proyecto mantenible mientras crece.

- [ ] Documentar arquitectura.
- [ ] Documentar ABI/syscalls.
- [ ] Documentar formato de estructuras internas importantes.
- [ ] Documentar invariantes de concurrencia (ver deuda 1.2).
- [ ] Documentar proceso de boot.
- [ ] Documentar cómo escribir drivers.
- [ ] Documentar cómo escribir aplicaciones.
- [ ] Ampliar CI con tests de regresión.
- [ ] Sanitización/fuzzing de parsers cuando sea posible (candidatos: BPB parser, GPT parser, ELF loader).
- [ ] Revisar warnings del compilador.
- [ ] Reducir deuda técnica periódicamente.
- [ ] Mantener commits pequeños y funcionalmente aislados.
- [ ] Añadir dependencias de headers al Makefile del kernel (`$(wildcard *.o): $(wildcard *.h)`) para evitar builds incrementales desincronizados.

---

# Orden recomendado de alto nivel

1. **Robustez del kernel + auditoría SMP** — ✅ completada (solo quedan tareas de documentación/debugging, no bugs SMP conocidos)
2. **Storage persistente + filesystem** — ✅ en gran parte (FAT32 persistente con LFN, rename, mount en `/`; pendiente: buffer cache, tests de corrupción, boot configurable).
3. **Userland/libc/shell** — ⏳ en progreso avanzado (ABI Linux, musl, TTY, pipes, redirecciones y `busybox sh` ya funcionan; quedan libc/POSIX, PTY, job control y superficie BusyBox).
4. **Escritorio y window manager**
5. **Networking**
6. **USB**
7. **Seguridad avanzada**
8. **Drivers/hardware adicional**
9. **Performance/NUMA/escalabilidad**
10. **SDK/toolchain/ecosistema**
11. **BusyBox completo + personalidad Linux (ELF)** — depende de libc/syscalls de la Fase 3, pero puede avanzar en paralelo a redes/USB una vez esté estable el userland.
12. **Compatibilidad PE/Windows (experimental)** — solo tiene sentido una vez 11 esté maduro; alcance permanentemente acotado a consola/x64 sin GUI.

La idea es evitar implementar muchas funciones superficiales a la vez. Primero hay que conseguir que el núcleo sea difícil de romper; después darle almacenamiento persistente; a partir de ahí, construir userland, red y escritorio sobre APIs estables.

## Objetivos de largo plazo

- [x] Aurora OS arranca de forma fiable en QEMU con 1/2/4 CPU.
- [~] Aurora OS puede instalarse/arrancar desde almacenamiento persistente. (FAT32 es la raíz del VFS y el usuario escribe ahí; el bootloader sigue cargando el kernel desde una ruta fija. Falta instalador + boot configurable).
- [x] Aurora OS puede crear, modificar y conservar archivos. (FAT32 persistente, LFN, rename y apps userland completas: `mkdir/rm/cp/mv/pwd`).
- [x] Aurora OS puede ejecutar múltiples aplicaciones aisladas.
- [x] Aurora OS dispone de terminal y escritorio utilizables.
- [ ] Aurora OS puede comunicarse por red.
- [ ] Aurora OS puede utilizar teclado/ratón USB.
- [ ] Aurora OS dispone de un modelo de seguridad coherente.
- [ ] Aurora OS tiene SDK/documentación suficientes para desarrollar aplicaciones de terceros.
- [~] Aurora OS ejecuta un userland tipo BusyBox completo. (`busybox sh` interactivo y una colección creciente de applets sobre musl ya funcionan; falta ampliar la superficie hasta un build completo, ver 3.5)
- [ ] Aurora OS puede ejecutar binarios Linux (ELF) reales sin recompilar, al menos estáticos/musl (ver 3.6).
- [ ] Aurora OS puede ejecutar un subconjunto acotado de ejecutables Windows de consola (PE/x64), sin pretender compatibilidad Win32 completa (ver 3.7).

> **Principio:** priorizar primero corrección, aislamiento y observabilidad; después funcionalidad; y finalmente optimización. Cada bug importante corregido debería, cuando sea posible, quedar acompañado de una regresión automatizada.