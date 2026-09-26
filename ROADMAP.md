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
- [x] IPC por mailboxes
- [x] Framebuffer compositor + window server + terminal gráfico
- [x] Framework de tests del kernel + regresiones userland
- [x] CI de build y boot con QEMU, incluyendo 1/2/4 CPU

La prioridad ahora es completar userland y filesystem, después networking/USB, y seguir validando Aurora OS sobre hardware real.

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

### 3.1 libc
- [ ] Ampliar la libc propia.
- [ ] Mejorar manejo de errores y errno.
- [ ] Añadir APIs de tiempo, procesos, archivos y memoria que falten.
- [ ] Headers y ABI más completos.
- [ ] Definir claramente qué parte es propia y qué parte pretende compatibilidad POSIX.

### 3.2 Procesos y ejecución
- [x] Argumentos en `spawn`/`exec`: `SYS_SPAWN_ARGS`, arg block en el stack del hijo (convención System V), `crt0.asm` lee `argc`/`argv`. Entorno (`envp`) todavía vacío.
- [x] cwd por proceso: `SYS_CHDIR`, `SYS_GETCWD`, `proc->cwd`, herencia en `spawn`, resolución de paths relativos y `..` contra cwd.
- [ ] Variables de entorno (`envp`, `getenv`/`setenv`).
- [ ] Señales, si se decide que forman parte del modelo de Aurora.
- [ ] Pipes y redirecciones.
- [ ] PTYs para terminales.
- [ ] Job control del shell.

### 3.3 VFS y /dev
- [ ] /proc para observabilidad.
- [ ] /dev/kmsg.
- [ ] /dev/null, /dev/zero y dispositivos básicos.
- [ ] Mejorar permisos y metadatos de archivos.
- [x] (Pivot root) FAT32 en `/`, tarfs en `/initrd`. Punto de montaje `/initrd` se crea como dir vacío en FAT32 al arrancar para que `ls /` lo liste (mismo modelo que Linux).
- [ ] `pivot_root`/`switch_root` genéricos: hoy el layout es fijo (FAT32 raíz + tarfs en /initrd); sería útil soportar cambiar la raíz en runtime tras montar otro FS.

### 3.4 Shell
- [ ] Historial.
- [ ] Autocompletado.
- [ ] Variables de entorno.
- [ ] Pipes/redirecciones.
- [ ] Jobs/background.
- [ ] Mejor manejo de errores.

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
3. **Userland/libc/shell** — ⏳ en progreso (argv y cwd listos; siguientes: libc, pipes/redirecciones, /dev, shell con historial).
4. **Escritorio y window manager**
5. **Networking**
6. **USB**
7. **Seguridad avanzada**
8. **Drivers/hardware adicional**
9. **Performance/NUMA/escalabilidad**
10. **SDK/toolchain/ecosistema**

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

> **Principio:** priorizar primero corrección, aislamiento y observabilidad; después funcionalidad; y finalmente optimización. Cada bug importante corregido debería, cuando sea posible, quedar acompañado de una regresión automatizada.