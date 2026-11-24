# Plan: mini-VM de AuroraOS (aurora-vm)

Estado: F0, F1 y F2a hechos y validados por Alvaro en QEMU+KVM (`aurora-vm --autotest`
da 3 OK). F3a (SeaBIOS + virtio-blk) escrito, solo probado a trozos: la BIOS arranca el
sector de prueba en QEMU normal y los dispositivos del VMM tienen una prueba unitaria
(PCI, virtio-blk, i8042, CMOS), pero `aurora-vm --bios-test` aun no se ha ejecutado
sobre VT-x. Rama `claude/mini-vm-teclado`. **Nada de esto se ha podido ejecutar
todavia**: los hilos no tienen KVM, asi que F1 solo esta compilado. La primera
prueba real es en el PC de Alvaro (i7-12700KF, con virtualizacion activada en la
BIOS) o en QEMU con `-enable-kvm -cpu host` (VMX anidado, `kvm_intel nested=1`).

## 1. Para que sirve

`aurora-run` clasifica cada programa como `aurora`, `linux`, `windows` o `vm`.
Wine 11 cubre la mayoria de los `.exe`. La mini-VM es para lo que Wine no abre:
programas que necesitan un kernel Windows de verdad (drivers, anti-cheat en modo
usuario que comprueba el kernel, instaladores raros, software muy viejo con
dependencias de 16 bits/DOS, etc.). Se elige con una regla en `run.conf`:

    Juego.exe   vm

Un `.exe` no se puede ejecutar "dentro de un hipervisor" sin un sistema operativo
huesped que lo entienda. La mini-VM ejecuta por tanto **un huesped completo**
(ReactOS, o un Windows con licencia del usuario en una imagen de disco) y dentro
lanza el programa. Esto condiciona el diseno:

- Windows no se redistribuye: la imagen base la pone el usuario
  (`~/.local/share/aurora/vm/windows.img`) o se descarga ReactOS (GPL).
- Muchos anti-cheat de kernel (EAC, BattlEye, Vanguard) **detectan VMs y se
  niegan a arrancar**. La mini-VM no lo arregla; hay que decirlo claro al
  usuario. Lo que si arregla: software que Wine no emula bien.
- Rendimiento: CPU casi nativa (VT-x + EPT); graficos al principio solo
  framebuffer por software. 3D acelerado en el huesped es F7 y es lo mas dificil.

## 2. Arquitectura

```
 aurora-run --vm Juego.exe
      |
 /usr/libexec/aurora/aurora-vm  (VMM en espacio de usuario, glibc/musl, hilos)
      |  ioctl                   dispositivos: serie, PIT, PIC, LAPIC, PS/2,
      v                          virtio-blk/net/input, framebuffer -> ventana Aurora
 /dev/aurora-vm   (kernel/vmx.c: VMXON, VMCS, EPT, bucle de ejecucion)
      |
   VT-x + EPT (+ unrestricted guest)
```

Reparto como KVM, pero mucho mas pequeno:

- **Kernel** (`kernel/vmx.c`, `kernel/vmx.h`, `include/aurora_vm.h`): activa VMX
  por CPU, crea el VMCS y las tablas EPT, ejecuta el vCPU y devuelve al VMM solo
  las salidas que necesitan un dispositivo (E/S, MMIO, HLT, apagado). Resuelve
  en el kernel lo trivial (CPUID, MSRs conocidos, CR, fallos EPT de RAM).
- **VMM** (`user/musl/apps/aurora-vm`): carga el firmware/imagen, emula los
  dispositivos y traduce entrada/pantalla a Aurora.
- **RAM del huesped** = un `memfd` del VMM. El VMM lo mapea (`mmap`) para
  cargar y para virtio; el kernel lo registra con `AVM_SET_MEMORY` y rellena el
  EPT por demanda (fallo EPT -> `get_page` del memfd). Sin copias.

### API del kernel (`include/aurora_vm.h`)

Un `open("/dev/aurora-vm")` = una VM (como `KVM_CREATE_VM`). Un solo vCPU en F1.

| ioctl | Hace |
|---|---|
| `AVM_GET_CAPS` | VMX/EPT/unrestricted guest disponibles, VPID, etc. |
| `AVM_SET_MEMORY` | `{memfd, gpa, size, offset}`: mapea RAM del huesped |
| `AVM_SET_REGS` / `AVM_GET_REGS` | GPR, RIP, RFLAGS, CR0/3/4, EFER, segmentos, GDTR/IDTR |
| `AVM_RUN` | Ejecuta hasta una salida; devuelve `struct avm_exit` |
| `AVM_INJECT_IRQ` | Inyecta una interrupcion externa (vector) |

Salidas: `AVM_EXIT_IO` (puerto, tamano, direccion, dato), `AVM_EXIT_MMIO`,
`AVM_EXIT_HLT`, `AVM_EXIT_SHUTDOWN` (triple fallo), `AVM_EXIT_INTR_WINDOW`,
`AVM_EXIT_SIGNAL` (el VMM debe mirar su cola: senal o plazo), `AVM_EXIT_FAIL`.

## 3. Decisiones de F1 y por que

- **Requiere EPT + unrestricted guest.** Todos los Intel desde Westmere los tienen;
  evita emular modo real y paginacion con sombras. Sin ellos, `AVM_GET_CAPS`
  dice que no y `aurora-vm` avisa. Solo Intel VMX en F1; AMD SVM (NPT) es F8.
- **VMCS sin estado entre `AVM_RUN`** (se hace VMCLEAR/VMPTRLD en cada entrada
  con preempcion desactivada): el vCPU puede migrar de CPU sin IPIs. Las salidas
  rapidas (CPUID, MSR, CR, fallo EPT) se resuelven sin salir de la ventana de
  preempcion. Optimizar mas (mantener el VMCS cargado) en F6.
- **Interrupciones del host**: salida por interrupcion externa activada; el kernel
  hace `sti` un instante en cada salida para que el host sirva la IRQ. Asi el
  planificador sigue mandando aunque el huesped no haga nada.
- **Dispositivos en espacio de usuario**, no en el kernel: un fallo de un
  dispositivo no tira el kernel, y se depura con las herramientas normales.
- **Sin `vmxoff` en suspension todavia**: S3 real es de `Plan_Power.md`; cuando
  se haga hay que cerrar VMX antes de dormir.
- **Coexistencia con LinuxKPI/VT-d (L9)**: F7 (pasar la GPU al huesped) usa el
  IOMMU de L9. No se toca en F1.

## 4. Fases

| Fase | Contenido | Se prueba con |
|---|---|---|
| F0 | Este plan | - |
| F1 | VMX + EPT + `/dev/aurora-vm`; VMM que ejecuta un binario plano en modo real con serie en 0x3F8 | `aurora-vm --plano tests/vm/hola.bin` imprime "Hola desde la VM" |
| F2 | (a, hecha) serie, PIC x2, PIT, CMOS, decodificador MMIO (MOV/MOVZX/MOVSX) y temporizador `AVM_SET_TIMEOUT`; `aurora-vm --autotest` corre 3 guests (real, modo largo+MMIO, PIT+IRQ). (b, falta) LAPIC/IOAPIC, XSAVE. Dispositivos basicos: PIT, PIC 8259, LAPIC/IOAPIC virtual, reloj CMOS, PS/2, serie 16550 | SeaBIOS llega al menu de arranque |
| F3 | (a, escrita, **sin probar**) SeaBIOS 1.16.3 sin parches en `third_party/seabios` (128 KiB, `build.sh` la reconstruye), plataforma minima en `user/musl/apps/aurora-vm/platform.h`: PCI mecanismo 1 con puente i440FX (00:00.0, subsistema 1af4:1100 para que SeaBIOS lo trate como "QEMU") y virtio-blk heredado (00:03.0, E/S en BAR0), CMOS/RTC con el tamano de RAM, i8042, PIT canal 2 (calibracion del TSC), puertos 0x92/0xCF9, depuracion de la BIOS por 0x402. `aurora-vm --bios [--disco IMG] [--mem MiB] [--tiempo S] [--log-bios]` y `aurora-vm --bios-test` (sector de arranque incorporado, tests/vm/mbr.asm). (b, escrita, sin probar sobre VT-x) IDE PIIX3 (00:01.1 + puente ISA 00:01.0): ATA PIO, MULTIPLE, LBA48 y DMA de bus maestro (BAR4 + PRD), canal 0 con un maestro (`--ide IMG`), IRQ14/15; INS/OUTS: el kernel solo describe la instruccion (`io.string`, `io.linear`, `io.count`) y el VMM mueve los datos y ajusta RSI/RDI/RCX (ReactOS/Windows no traen driver virtio-blk, necesitan IDE). (c, escrita, sin probar sobre VT-x) ACPI + LAPIC/IOAPIC virtuales: el kernel solo expone el bit APIC de CPUID/MSR 0x1B si el VMM lo pide (`AVM_SET_FLAGS`/`AVM_VMF_APIC`, asi `--bios-test` no cambia); `apic.h` = LAPIC (IRR/ISR/TPR, EOI, temporizador, IPI propia) + IOAPIC de 24 pines (flanco/nivel, IRR remoto); dispositivo PM PIIX4 (00:01.3: PM1, temporizador de 3,58 MHz, GPE0, SMI_CMD 0xB2, SCI en IRQ9, apagado por SLP_EN); SeaBIOS reconstruido con ACPI y genera FADT/MADT/DSDT (`--bios` lo activa; `--sin-acpi` lo quita; `--bios-test-acpi`). Un solo vCPU, sin fw_cfg (sin override de IRQ0), sin HPET. (d, escrita, sin probar sobre VT-x) VGA stdvga/Bochs (`vga.h`, PCI 00:02.0 1234:1111): SeaVGABIOS (`third_party/seabios/vgabios.bin`) se carga desde la ROM de expansion (BAR de ROM de 64 KiB, memfd en el slot 3) y da int 10h/VBE; 0xA0000-0xBFFFF sigue siendo RAM normal (texto en 0xB8000, fuente en 0xA0000 como plano 2, modo 13h); modos VBE lineales 8/15/16/24/32 bits por el BAR0 de 16 MiB (memfd en el slot 2, sin salidas); registros VGA/CRTC/DAC y Bochs dispi (0x1CE/0x1CF). `vgawin.h`: ventana nativa de Aurora (~30 fps) + teclado crudo (`ASYS_WIN_SET_RAW_INPUT` con 2: el kernel manda tambien las teclas al soltar, `WIN_FLAGS_RAW_KEYS`) hacia el i8042 (IRQ1). `--bios` abre la ventana (`--sin-ventana`, `--sin-vga`, `--vgarom`), `--bios-test-vga` comprueba que el texto de SeaBIOS aparece en pantalla. NO soportado aun: modos graficos planares de 16 colores (ReactOS/Windows los usan al arrancar), modo X, raton, teclas extendidas (E0). Falta: planar, raton PS/2, arrancar ReactOS | `aurora-vm --bios-test` imprime `[OK]`; luego ReactOS llega al escritorio |
| F4 | Pantalla: VGA/VBE y luego virtio-gpu 2D en una ventana Aurora; teclado/raton (virtio-input o tableta USB) | Se puede usar el huesped con el raton |
| F5 | Red (virtio-net sobre la pila de Aurora, NAT en el VMM) y carpeta compartida (virtio-fs o 9p) para pasar el `.exe` | `aurora-vm Juego.exe` lo ejecuta en el huesped |
| F6 | Multi-vCPU (hilos), VPID, VMCS persistente, rendimiento, integracion con `aurora-run` y Aplicaciones | Windows con 4 vCPU fluido |
| F7 | GPU: virtio-gpu con Venus/3D o paso de la RTX 4060 con VT-d | Un juego DX11 a algo mas que 30 fps |
| F8 | AMD SVM/NPT | Ryzen |

## 4b. Limites conocidos de F1 (lo que NO hace todavia)

- CPUID oculta XSAVE/AVX/x2APIC/TSC-deadline/PCID: el huesped no usa AVX hasta F2.
  Windows moderno arranca sin ello, pero pierde rendimiento.
- Sin APIC virtual ni temporizadores: el VMM inyecta IRQ con `AVM_INJECT_IRQ`.
- Salida MMIO sin decodificar la instruccion (solo gpa y lectura/escritura): el
  decodificador x86 minimo es de F2. INS/OUTS tambien.
- MSR: SYSCALL/KERNEL_GS_BASE se cargan por hardware; MTRR/PAT/MISC_ENABLE se
  responden con ceros o se recuerdan sin aplicar; los MSR desconocidos dan #GP.
- Un vCPU; FPU del huesped con FXSAVE (no XSAVE); sin VPID.

## 5. Riesgos conocidos

- Sin KVM en los hilos, **todo el codigo VMX se escribe contra el manual de Intel
  (SDM vol. 3C) sin ejecutarlo**. Un error en el VMCS da "VM-entry failure"; el
  kernel registra el campo `VM-instruction error` y el motivo de salida para
  poder corregirlo rapido en el PC de Alvaro. Primera prueba recomendada: QEMU con
  KVM anidado, no el PC real (un VMXON mal hecho no deberia colgar, pero asi se
  evita el miedo).
- Alder Lake con nucleos P/E: `MSR_IA32_VMX_*` pueden diferir entre nucleos; se
  leen en la CPU que ejecuta VMXON (ya es asi: los MSR se leen por CPU).
- Licencias: SeaBIOS (LGPLv3), OVMF (BSD-2), ReactOS (GPL), virtio (BSD). Windows,
  la imagen del usuario.

## 5b. Rendimiento (medido con KVM anidado, 2026-11-23/24)

Con KVM anidado cada VMREAD/VMWRITE de un campo que no esta en el VMCS en sombra
de KVM, cada VMPTRLD/VMCLEAR y cada RDMSR de un MSR interceptado es una salida al
hipervisor de fuera (unos us). Lo que se hizo para no pagarlo en cada salida:

- El VMCS queda activo en la CPU entre ventanas (VMRESUME); `vmx_sched_out()`
  (desde `sched_tick`) hace el VMCLEAR al cambiar de tarea.
- Tras MMIO, INS/OUTS y HLT solo se leen RIP/RSP/RFLAGS/CR/EFER + AR y base de
  CS/SS + bases de ES/DS/FS/GS (`AVM_GET/SET_REGS_LITE`); el VMM usa esas ordenes
  para emular y cae a GET/SET_REGS con un kernel viejo. Leer todo el VMCS costaba
  ~130 us por salida MMIO.
- HOST_CR0/CR3 solo si cambian; MSR del host leidos una vez por CPU.
- EPT: cada fallo mapea 64 paginas (256 KiB) alineadas del slot.
- SeaBIOS (`third_party/seabios/parches/`, `config`): CD por INT 13h en bloques
  DRQ de hasta 31 sectores; disco duro por DMA de bus maestro (CONFIG_ATA_DMA).
  Antes ~4 salidas por sector.
- VMM: reloj de dispositivos interpolado con el TSC (`CLOCK_MONOTONIC` de AuroraOS
  va a saltos de 1 ms); `AURORA_VM_RELOJ_TICK=1` vuelve al de ticks.
- `AURORA_VM_TRAZA=1`: lineas `[traza]` y `[rend]` cada 2 s (salidas, puertos,
  tiempo en huesped/kernel/VMM, coste de preparar y salir de cada ventana).

## 6. Pruebas

- `tests/vm/hola.asm` / `hola.bin`: 16 bits, escribe una cadena por el puerto
  0x3F8 y hace `hlt`.
- `kernel/tests/kernel_tests.c`: `test_vmx_caps` (solo comprueba que
  `AVM_GET_CAPS` no cuelga; se salta sin VMX, es el caso de las baterias QEMU sin
  KVM).
- Bateria QEMU: sigue necesitando `lkpi.auto=off` (`auroraos-bateria-qemu.md`).
- `user/musl/apps/aurora-vm/tests/replay.c`: reproduce una traza de QEMU
  (`-trace memory_region_ops_*`) contra los modelos de dispositivo y compara cada
  lectura (IDE, bus maestro, VGA, VBE, i8042 cuadran con el arranque de ReactOS).
- `user/musl/apps/aurora-vm/tests/uc_backend.c` + `uc_shim.h`: `/dev/aurora-vm`
  falso sobre Unicorn 2.1 (CPU emulada) para probar el VMM entero sin VT-x:
  ```
  U=<unicorn: include/ y lib/libunicorn.so.2>; A=user/musl/apps/aurora-vm
  gcc -O1 -include $A/tests/uc_shim.h -c -o m.o $A/main.c
  gcc -I$U/include -c -o b.o $A/tests/uc_backend.c
  gcc -o vm-uc m.o b.o $U/lib/libunicorn.so.2 -Wl,-rpath,$U/lib -lpthread
  ./vm-uc --autotest; AURORA_VM_BIOS=.../bios.bin AURORA_VM_VGABIOS=.../vgabios.bin ./vm-uc --bios-test-vga
  AURORA_VM_CAPTURA=pantalla.ppm ./vm-uc --bios --sin-ventana --cd reactos.iso --ide hd.img --mem 256 --tiempo 3000
  ```
  Pasan autotest 3/3 y los tres `--bios-test*`; el CD de ReactOS 0.4.15 arranca
  (SeaBIOS, freeldr, ntoskrnl/hal, "BOOT DRIVERS LOADED": PCI, ATAPI con DMA,
  i8042, modo usuario/smss). `UC_DBGPRINT=1` muestra los DbgPrint del huesped.
  Se para en una llamada V86 a la BIOS de video (#PF en 0:F31C dentro del banco;
  con VT-x el V86 lo hace la CPU). Es ~1000 veces mas lento que VT-x (~25 min).
  `UC_STRIO=1` hace salir REP INS/OUTS en bloque como VT-x; `UC_STRIO=2` ademas
  manda asize/linear erroneos, como KVM anidado (ahi el VMM decodifica los
  prefijos; asi fallaba el IDENTIFY: disco de 0 MiB). No cubre: EPT, CR8/TPR ni CPUID/MSR del kernel, ni
  #GP/#PF del huesped en modo protegido (Unicorn no da el codigo de error).
