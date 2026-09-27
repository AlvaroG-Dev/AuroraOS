# Aurora OS

Aurora OS es un sistema operativo **bare-metal x86_64** desarrollado desde cero, con arranque UEFI, kernel monolítico, multitarea preventiva, SMP, memoria virtual, procesos aislados en Ring 3, almacenamiento persistente FAT32 y un entorno gráfico basado en framebuffer.

El proyecto está orientado a aprender y construir un sistema operativo real a bajo nivel: boot, memoria, scheduler, interrupciones, drivers, VFS, userland, IPC y gráficos forman parte del mismo sistema.

> **Estado actual:** Aurora OS ya supera la fase de kernel experimental. El núcleo, SMP, memoria virtual, almacenamiento FAT32 y entorno gráfico están integrados. El userland ha avanzado hasta una **ABI Linux x86_64 compatible en progreso**, ejecución de binarios estáticos compilados con **musl**, TTY real, `poll/select`, pipes, redirecciones y `busybox sh` interactivo. Networking, USB y partes avanzadas de POSIX/seguridad siguen pendientes.

## Características implementadas

### Boot y plataforma

- UEFI + carga de ejecutables **ELF64**.
- Framebuffer UEFI/GOP.
- GDT e IDT.
- ISR/IRQ y manejo de excepciones.
- TSS por CPU.
- ACPI.
- MADT para descubrimiento de CPUs.
- AP startup mediante mecanismos SMP modernos de ACPI cuando están disponibles.
- Soporte para LAPIC, IOAPIC e IPI.
- Per-CPU state mediante `%gs`.
- PIT, RTC y TSC.
- CPUID y detección de capacidades SIMD.
- SSE, AVX, AVX2, FMA y AVX-512 cuando la CPU lo soporta.

### Memoria y aislamiento

- Physical Memory Manager (PMM).
- Bitmap allocator.
- Buddy allocator.
- Kernel heap.
- SLAB allocator.
- Paging x86_64 de 4 niveles.
- Address spaces independientes para procesos.
- Demand paging.
- `mmap()` / `munmap()`.
- VMA management.
- Page faults de usuario.
- Protección de permisos de páginas.
- NX.
- SMEP.
- SMAP.
- `copy_to_user()` / `copy_from_user()` y validación de accesos user/kernel.
- TLB shootdown entre CPUs.
- Separación Ring 0 / Ring 3.

### Scheduler y multitarea

- Scheduler preventivo Round-Robin.
- Procesos y threads.
- Context switching en assembly.
- `current_task` per-CPU.
- Runqueue global.
- Wait queues.
- Blocking/wakeup.
- Timeouts.
- `waitpid()`.
- Task lifecycle y refcounting.
- CPU affinity.
- Migración de tareas entre CPUs.
- Idle tasks.
- Preempción mediante timer.
- IPI de reschedule.
- Protección frente a carreras de publicación de tareas SMP.
- Generación `wait_seq` para evitar wakeups producidos por timeouts obsoletos.

La auditoría SMP realizada durante el desarrollo cubre scheduler, migración, wake/block, timeouts, IPI, TLB shootdown, lifecycle y arranque de APs.

### Procesos y userland

- Ejecución de aplicaciones en **Ring 3**.
- ELF loader.
- ELF loader streaming desde VFS, evitando depender de un buffer contiguo gigante.
- `spawn()` / `execve()` in-place.
- Argumentos `argc/argv` y bloque de argumentos compatible con el ABI Linux.
- Variables de entorno (`envp`, `environ`, `getenv()` / `setenv()`).
- `waitpid()` / `wait4()` con `struct rusage` y contabilidad de CPU.
- `getpid()`, `exit()`, `kill()`.
- Soporte inicial de señales y `rt_sigaction`/`rt_sigprocmask`.
- `sbrk()`, `brk()`, `mmap()` / `munmap()`, incluido `MAP_FIXED`.
- CWD por proceso: `chdir()` / `getcwd()`.
- ABI Linux x86_64 en progreso, con tabla dual Linux/Aurora.
- `arch_prctl(SET_FS/GET_FS)` y `fs_base` per-task.
- `stat` Linux x86_64 y traducción desde los metadatos VFS.
- `readv()` / `writev()`, `getdents64()`, `fcntl()` y syscalls POSIX/Linux adicionales.
- Pipes anónimos, `dup2()`, redirecciones y pipelines.
- TTY real con line discipline, modo canonical/raw, echo, señales de terminal, `poll/select/pselect6/ppoll` e ioctls básicos.
- Libc propia parcial + soporte para binarios estáticos compilados con **musl**.
- `malloc()` / `free()` / `calloc()` / `realloc()` mediante el userland soportado.
- IPC mediante mailboxes.
- Identificación de tareas y servicios.
- Syscalls gráficas para crear/destruir ventanas, blit y eventos.

El kernel mantiene una ABI Aurora y una compatibilidad Linux x86_64 en paralelo. La compatibilidad Linux está orientada inicialmente a binarios estáticos y se amplía progresivamente según las necesidades de musl/BusyBox.

### Storage

Aurora OS dispone actualmente de una pila de almacenamiento persistente:

- Block layer.
- BIO/read/write/completions.
- ATA PIO.
- ATA DMA.
- ATAPI básico.
- AHCI/SATA.
- Detección mediante PCI.
- MSI para AHCI.
- Particiones:
  - MBR
  - GPT
  - EBR / particiones extendidas
- VFS con mount points.
- FAT32 persistente.
- Lectura y escritura.
- Creación y eliminación de archivos.
- Directorios.
- `mkdir`.
- `unlink`.
- `truncate`.
- `rename`, incluyendo movimientos soportados entre directorios.
- Long File Names (LFN).
- Caché de FAT.
- Montaje de FAT32 como raíz `/`.
- TarFS/initrd conservado en `/initrd`.

El loader puede ejecutar ELFs directamente desde VFS/FAT32 mediante lectura streaming.

### Aplicaciones y shell

El userland actual incluye aplicaciones nativas y una colección creciente de programas compilados con musl:

**Nativas / sistema**
- `shell`
- `ls`
- `cat`
- `mkdir`
- `rm`
- `cp`
- `mv`
- `pwd`
- `calc`
- aplicaciones de prueba de memoria, ELF, VMA, IPC, procesos y filesystem.

**Userland sobre musl**
- `hello_musl`
- `echo`
- `cat`
- `ls`
- `touch`
- `pwd`
- `wc`
- `head`
- `grep`
- `tee`
- `sort`
- `cp`
- `mv`
- `rm`
- `mkdir`
- `kill`
- `calc`

### Shell y TTY

Aurora dispone de dos capas de terminal:

- consola gráfica/nativa;
- TTY real compatible con las necesidades de un shell tipo Linux.

El shell actual soporta:

- argumentos y cwd;
- ejecución de programas mediante `fork` + `execve`;
- `cd`, `pwd`, `help`, `echo`, `clear`, `exit`;
- pipes `|`;
- redirecciones `<`, `>`, `>>`;
- pipelines de varias etapas;
- `Ctrl+C` mediante SIGINT;
- espera y terminación de hijos;
- parser ANSI CSI;
- fuente JetBrains Mono antialiased.

El TTY implementa line discipline con modos canonical/raw, echo, `VINTR`, `VERASE`, `VEOF`, `VSUSP`, `ICRNL`, `ONLCR`, wait queues, `poll/select` y varios ioctls de terminal. `/dev/tty` se expone mediante devfs.

### ABI Linux x86_64

Aurora incorpora una capa de compatibilidad Linux en progreso:

- números de syscall Linux x86_64 en el rango correspondiente;
- syscalls Aurora adicionales en un espacio separado;
- `arch_prctl` y TLS mediante `FS_BASE`;
- `struct stat` Linux;
- `writev/readv`;
- `getdents64`;
- `open/openat/stat/fstat/lstat/fstatat/access`;
- `mkdirat/unlinkat/renameat`;
- `brk/mmap/munmap`;
- `clock_gettime/getrandom`;
- `wait4`;
- `rt_sigaction/rt_sigprocmask`;
- ABI probado con un binario de regresión que usa directamente números de syscall Linux.

Esto permite ejecutar binarios estáticos compilados con musl sin recompilarlos contra la libc propia de Aurora.

### Gráficos y escritorio

Aurora OS dispone de un entorno gráfico basado en framebuffer:

- Compositor.
- Window server.
- Ventanas.
- Terminal gráfica.
- Taskbar.
- Temas.
- Fuentes bitmap y fuentes con antialiasing.
- Iconos.
- Sombras.
- Animaciones.
- Damage tracking / renderizado por regiones.
- Eventos de teclado y ventana.
- Registro de consola gráfica.
- Blitting desde userland mediante `WIN_BLIT`.
- Protección mediante `copy_from_user()` para buffers gráficos de procesos.

Todavía no existe aceleración GPU: el rendering se realiza sobre framebuffer.

## Drivers y hardware

Actualmente existen componentes para:

| Componente | Estado |
|---|---|
| PS/2 teclado | Implementado |
| PS/2 mouse | Implementado |
| PCI | Implementado |
| PIT | Implementado |
| RTC | Implementado |
| ATA PIO | Implementado |
| ATA DMA | Implementado |
| ATAPI | Parcial |
| AHCI/SATA | Implementado |
| LAPIC | Implementado |
| IOAPIC | Implementado |
| ACPI | Implementado |
| GOP/framebuffer | Implementado |
| USB host | Pendiente |
| USB HID | Pendiente |
| NVMe | Pendiente |
| Ethernet | Pendiente |
| GPU acceleration | Pendiente |

## Seguridad

Ya están implementadas varias protecciones importantes:

- Ring 0 / Ring 3.
- Aislamiento de address spaces.
- NX.
- SMEP.
- SMAP.
- Validación de accesos user/kernel.
- Protección de páginas según permisos ELF/VMA.
- Manejo de page faults de procesos.
- TLB shootdown SMP.

Pendiente:

- ASLR/KASLR.
- Stack canaries del kernel.
- CFI o equivalente.
- Usuarios/grupos.
- Permisos de archivos.
- Capabilities/sandboxing.
- Hardening adicional de syscalls.
- Criptografía y TLS.

## Testing

El repositorio incluye un framework de tests del kernel y numerosas regresiones de userland.

Se prueban, entre otras cosas:

- PMM y allocators.
- Paging.
- Page faults.
- NX y permisos ELF.
- VMA isolation.
- `mmap()` / `munmap()`.
- `sbrk()`.
- Scheduler.
- SMP.
- CPU affinity.
- Migración de tareas.
- Wait queues.
- Timeouts.
- IPI.
- TLB shootdown.
- IPC.
- procesos y `waitpid()`.
- AHCI y acceso a disco.
- FAT32.
- VFS.
- operaciones de archivos.
- gráficos y accesos de buffers userland.
- validación de ELFs malformados.
- errores de memoria y recursos.

La CI ejecuta builds y boots con diferentes configuraciones de QEMU, incluyendo pruebas SMP y configuraciones con almacenamiento AHCI.

## Arquitectura del repositorio

La organización principal es:

```
AuroraOS/
├── bootloader/       # Aplicación UEFI y carga del kernel
├── kernel/            # Kernel monolítico
│   ├── acpi.*         # ACPI
│   ├── apic.*         # LAPIC/IOAPIC
│   ├── ahci.*         # AHCI/SATA
│   ├── ata_*.*        # ATA/ATAPI/DMA
│   ├── block.*        # Block layer
│   ├── fat32.*        # FAT32
│   ├── vfs.*          # Virtual File System
│   ├── paging.*       # Paging y address spaces
│   ├── pmm.*          # Memoria física
│   ├── buddy.*        # Buddy allocator
│   ├── heap.*         # Heap
│   ├── slab.*         # SLAB allocator
│   ├── pf.*           # Page faults y VMA/mmap
│   ├── sched.*        # Scheduler
│   ├── process.*      # Procesos
│   ├── elf.*          # ELF loader
│   ├── syscall.*      # Syscalls
│   ├── wait.*         # Wait queues
│   ├── ipc.*          # IPC/mailboxes
│   ├── ipi.*          # IPIs y TLB shootdown
│   ├── smp_boot.*     # Arranque SMP
│   ├── uaccess.*      # Acceso seguro a userland
│   ├── driver.*       # Framework de drivers
│   ├── input.*        # Input
│   ├── tty.*          # TTY
│   ├── gfx/            # Compositor, ventanas, taskbar y window server
│   └── tests/          # Tests del kernel
├── user/
│   ├── apps/           # Aplicaciones userland
│   └── lib/            # Librerías userland
├── scripts/             # Scripts de build/boot/test
├── tests/               # Imágenes y recursos de pruebas
├── Makefile
└── ROADMAP.md
```

## Compilar

### Dependencias Debian/Ubuntu

```bash
sudo apt update
sudo apt install build-essential nasm qemu-system-x86 ovmf gnu-efi mtools xorriso
```

### Fedora

```bash
sudo dnf install gcc nasm qemu edk2-ovmf gnu-efi-devel mtools xorriso
```

### Arch Linux

```bash
sudo pacman -S base-devel nasm qemu edk2-ovmf gnu-efi mtools xorriso
```

El Makefile detecta el toolchain disponible para el bootloader y el kernel.

Construir la imagen:

```bash
make
```

Esto genera la imagen FAT32 `aurora.img` y el entorno de arranque necesario.

## Ejecutar con QEMU

### 1 CPU

```bash
make run
```

### Debug con GDB

```bash
make run-debug
```

QEMU queda detenido con el servidor GDB disponible en el puerto 1234.

### SMP

Para probar SMP con virtualización por hardware:

```bash
make run-smp-kvm
```

Este target utiliza KVM y expone 8 CPUs virtuales.

Para depuración SMP:

```bash
make run-smp-kvm-debug
```

### AHCI + SMP

```bash
make run-smp-kvm-ahci
```

Este target utiliza Q35/AHCI y permite probar la pila de almacenamiento SATA junto con SMP.

### Arranque desde ISO

```bash
make iso
make run-iso
```

También se puede utilizar `aurora.iso` con otros hipervisores compatibles con UEFI.

## Estado del proyecto

| Subsistema | Estado |
|---|---|
| Boot UEFI + ELF64 | ✅ |
| GDT / IDT / IRQ / excepciones | ✅ |
| Memoria física y allocators | ✅ |
| Paging / address spaces | ✅ |
| Demand paging / VMA / mmap | ✅ |
| NX / SMEP / SMAP / uaccess | ✅ |
| Scheduler preventivo | ✅ |
| Procesos / threads / Ring 3 | ✅ |
| SMP / AP startup / IPI | ✅ |
| CPU affinity / migración | ✅ |
| TLB shootdown | ✅ |
| ATA / DMA / AHCI | ✅ |
| MBR / GPT / EBR | ✅ |
| VFS | ✅ |
| FAT32 persistente + LFN + rename | ✅ |
| ELF streaming desde VFS | ✅ |
| Syscalls Aurora | ✅ |
| ABI Linux x86_64 | 🟡 En expansión |
| musl estático | 🟡 Funcional |
| TTY / poll / select | 🟡 Funcional |
| Pipes / redirecciones / pipelines | 🟡 Funcional |
| BusyBox sh interactivo | 🟡 Funcional |
| libc propia | 🟡 Parcial |
| Shell y aplicaciones | 🟡 En expansión |
| Escritorio framebuffer | 🟡 Funcional, sin GPU |
| Networking | ⬜ Pendiente |
| USB | ⬜ Pendiente |
| GPU acceleration | ⬜ Pendiente |
| ASLR/KASLR | ⬜ Pendiente |
| Seguridad avanzada | ⬜ En desarrollo |

## Próximos grandes bloques

El desarrollo posterior se centra en:

1. **Compatibilidad Linux / BusyBox:** ampliar la superficie syscall/libc para ejecutar más applets y binarios estáticos reales.
2. **Userland:** completar entorno, señales, job control, PTY y APIs POSIX/Linux necesarias.
3. **Filesystem:** robustez ante corrupción/apagados durante escritura, alias 8.3 y boot configurable desde filesystem.
4. **Desktop:** resize, focus/input routing, ventanas, clipboard y workspaces.
5. **Networking:** NIC, Ethernet, ARP, IPv4, ICMP, UDP, TCP, DHCP y DNS.
6. **USB:** xHCI, HID y almacenamiento USB.
7. **Seguridad avanzada:** ASLR/KASLR, permisos, usuarios/grupos y sandboxing.
8. **Drivers y hardware moderno.**
9. **Performance y escalabilidad.**
10. **SDK/toolchain y documentación.**

Para conocer el estado detallado de cada tarea, consultar **[ROADMAP.md](ROADMAP.md)**.

## Filosofía

Aurora OS prioriza:

1. **Corrección**
2. **Aislamiento**
3. **Observabilidad**
4. **Funcionalidad**
5. **Rendimiento**

Las correcciones de bugs importantes deben ir acompañadas, cuando sea posible, de una regresión automatizada para evitar que vuelvan a aparecer.
