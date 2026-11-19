# Plan: mini-VM de AuroraOS (aurora-vm)

Estado: F0 (este plan) y F1 (VMX en el kernel + `/dev/aurora-vm` + VMM minimo)
escritos en la rama `claude/mini-vm-teclado`. **Nada de esto se ha podido ejecutar
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
| F2 | Dispositivos basicos: PIT, PIC 8259, LAPIC/IOAPIC virtual, reloj CMOS, PS/2, serie 16550 | SeaBIOS llega al menu de arranque |
| F3 | Firmware (SeaBIOS/OVMF en `third_party`, licencias aparte) + virtio-blk con imagen en archivo; arranque de ReactOS | ReactOS llega al escritorio |
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

## 6. Pruebas

- `tests/vm/hola.asm` / `hola.bin`: 16 bits, escribe una cadena por el puerto
  0x3F8 y hace `hlt`.
- `kernel/tests/kernel_tests.c`: `test_vmx_caps` (solo comprueba que
  `AVM_GET_CAPS` no cuelga; se salta sin VMX, es el caso de las baterias QEMU sin
  KVM).
- Bateria QEMU: sigue necesitando `lkpi.auto=off` (`auroraos-bateria-qemu.md`).
