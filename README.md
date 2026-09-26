# Aurora OS

Aurora OS es un sistema operativo **bare-metal x86_64** desarrollado desde cero, con arranque UEFI, kernel monolítico, multitarea preventiva, SMP, memoria virtual, procesos aislados en Ring 3, almacenamiento persistente FAT32 y un entorno gráfico basado en framebuffer.

El proyecto está orientado a aprender y construir un sistema operativo real a bajo nivel: boot, memoria, scheduler, interrupciones, drivers, VFS, userland, IPC y gráficos forman parte del mismo sistema.

> **Estado actual:** Aurora OS ya supera la fase de kernel experimental. El núcleo, SMP, memoria virtual, almacenamiento FAT32, userland básico y entorno gráfico están funcionalmente integrados y cuentan con regresiones automatizadas. Networking, USB y varias partes de POSIX/seguridad avanzada siguen pendientes.

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
- `spawn()` / `exec` básico.
- Argumentos `argc/argv`.
- `waitpid()`.
- `getpid()`.
- `exit()`.
- `kill()` y soporte inicial de señales.
- `sbrk()`.
- `mmap()` / `munmap()`.
- CWD por proceso.
- `chdir()` / `getcwd()`.
- Libc propia parcial.
- `malloc()` / `free()` / `calloc()`.
- Syscalls de archivos.
- IPC mediante mailboxes.
- Identificación de tareas y servicios.
- Syscalls gráficas para crear/destruir ventanas, blit y eventos.

Syscalls actualmente definidas incluyen operaciones de procesos, memoria, archivos, directorios, IPC, ventanas, servicios, cwd y señales.

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

El sistema incluye aplicaciones userland como:

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

El shell gráfico soporta actualmente:

- comandos built-in;
- argumentos;
- cwd;
- ejecución de programas;
- `cd`;
- `pwd`;
- `spawn`;
- `help`;
- `echo`;
- `clear`;
- `exit`;
- `Ctrl+C` mediante SIGINT;
- terminación de hijos;
- salida de procesos integrada en la consola.

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
| FAT32 persistente | ✅ |
| LFN / rename | ✅ |
| ELF streaming desde VFS | ✅ |
| Syscalls | ✅ |
| IPC | ✅ |
| libc propia | 🟡 Parcial |
| Shell y aplicaciones básicas | 🟡 Funcional, en expansión |
| Escritorio framebuffer | 🟡 Funcional, sin GPU |
| Networking | ⬜ Pendiente |
| USB | ⬜ Pendiente |
| GPU acceleration | ⬜ Pendiente |
| POSIX completo | ⬜ No es el objetivo actual |
| ASLR/KASLR | ⬜ Pendiente |
| Seguridad avanzada | ⬜ En desarrollo |

## Próximos grandes bloques

El desarrollo posterior se centra en:

1. **Memoria virtual y paging:** continuar la auditoría profunda de page faults, VMA, `mmap/munmap`, uaccess y liberación de memoria.
2. **Userland/libc:** ampliar la libc, errno, APIs de procesos/tiempo/memoria y compatibilidad.
3. **Shell:** historial, autocompletado, variables de entorno, pipes/redirecciones y job control.
4. **Filesystem:** robustez ante corrupción y apagados durante escritura, alias 8.3 y boot configurable desde filesystem.
5. **Desktop:** resize, focus/input routing, ventanas, clipboard y workspaces.
6. **Networking:** NIC, Ethernet, ARP, IPv4, ICMP, UDP, TCP, DHCP y DNS.
7. **USB:** xHCI, HID y almacenamiento USB.
8. **Seguridad avanzada y hardware moderno.**

Para conocer el estado detallado de cada tarea, consultar **[ROADMAP.md](ROADMAP.md)**.

## Filosofía

Aurora OS prioriza:

1. **Corrección**
2. **Aislamiento**
3. **Observabilidad**
4. **Funcionalidad**
5. **Rendimiento**

Las correcciones de bugs importantes deben ir acompañadas, cuando sea posible, de una regresión automatizada para evitar que vuelvan a aparecer.
