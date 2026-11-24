// kernel/aurora_vm.h — API de /dev/aurora-vm (mini-VM de AuroraOS).
//
// Compartida entre el kernel (kernel/vmx.c) y el VMM de espacio de usuario
// (user/musl/apps/aurora-vm). Ver Plan_MiniVM.md. Un open() del dispositivo es
// una VM con un vCPU.
#ifndef AURORA_VM_H
#define AURORA_VM_H

#include <stdint.h>

#define AVM_IOC_BASE 0xA700UL
#define AVM_GET_CAPS (AVM_IOC_BASE + 1)    // struct avm_caps *
#define AVM_SET_MEMORY (AVM_IOC_BASE + 2)  // const struct avm_memory *
#define AVM_SET_REGS (AVM_IOC_BASE + 3)    // const struct avm_regs *
#define AVM_GET_REGS (AVM_IOC_BASE + 4)    // struct avm_regs *
#define AVM_RUN (AVM_IOC_BASE + 5)         // struct avm_exit *
#define AVM_INJECT_IRQ (AVM_IOC_BASE + 6)  // vector | AVM_IRQ_* (por valor); -EBUSY si ya hay una pendiente.
// La interrupcion queda pendiente en el kernel hasta que el huesped puede recibirla
// (IF=1, sin sombra de STI/MOV SS y, si lleva AVM_IRQ_TPR, vector por encima del TPR).
// Cada salida dice en avm_exit.irq_pending si sigue pendiente (-1 = ya entregada):
// el VMM solo debe pasarla al ISR de su PIC/LAPIC cuando el kernel la entrego.
#define AVM_IRQ_TPR (1u << 8)              // viene del LAPIC: respeta el TPR (CR8)
#define AVM_SET_TIMEOUT (AVM_IOC_BASE + 7) // ms (por valor): AVM_RUN devuelve AVM_EXIT_SIGNAL tras ese tiempo; 0 = sin limite

#define AVM_SET_DEBUG (AVM_IOC_BASE + 8)   // flags (por valor): bit0 = interceptar #UD/#DF/#GP/#PF y registrarlos en el log del kernel

#define AVM_SET_FLAGS (AVM_IOC_BASE + 9)   // AVM_VMF_* (por valor)
#define AVM_VMF_APIC (1u << 0)             // CPUID anuncia el APIC local y IA32_APIC_BASE = 0xFEE00900 (el VMM emula el LAPIC por MMIO)
#define AVM_VMF_ALL (AVM_VMF_APIC)

// TPR del LAPIC (registro 0x80; CR8 = TPR >> 4). El kernel lo mantiene porque
// MOV CR8 (Windows de 64 bits, en cada cambio de IRQL) se resuelve sin salir al VMM.
#define AVM_SET_TPR (AVM_IOC_BASE + 10)      // valor del registro TPR (por valor): escrituras MMIO del huesped
// Clase (1..15) del vector que el LAPIC del VMM tiene bloqueado por el TPR; 0 = ninguno.
// Cuando el huesped baja CR8 por debajo, AVM_RUN sale con AVM_EXIT_TPR.
#define AVM_SET_TPR_WAIT (AVM_IOC_BASE + 11)
// Retira la interrupcion pendiente sin entregar. Devuelve su valor (vector | flags) o -1.
#define AVM_WITHDRAW_IRQ (AVM_IOC_BASE + 12)

// Estadisticas de rendimiento acumuladas (struct avm_stats *). Los tiempos van
// en ciclos de TSC: el VMM los pasa a microsegundos comparando con su reloj.
// Como GET/SET_REGS pero sin leer todo el VMCS (con KVM anidado cuesta ~130 us):
// GET_REGS_LITE garantiza al dia los registros generales, RIP, RSP, RFLAGS,
// CR0/CR3/CR4, EFER, CS/SS (AR y base) y las bases de ES/DS/FS/GS; el resto puede
// estar viejo. SET_REGS_LITE solo aplica los generales, RSP, RIP y RFLAGS. Para
// emular MMIO e INS/OUTS. Un kernel sin ellas devuelve error: usar GET/SET_REGS.
#define AVM_GET_REGS_LITE (AVM_IOC_BASE + 14)
#define AVM_SET_REGS_LITE (AVM_IOC_BASE + 15)
#define AVM_GET_STATS (AVM_IOC_BASE + 13)
struct avm_stats {
  uint64_t runs;      // llamadas a AVM_RUN
  uint64_t windows;   // ventanas (VMCLEAR/VMPTRLD + estado completo + VMLAUNCH)
  uint64_t exits;     // salidas de VM (todas)
  uint64_t ext_exits; // por IRQ del host
  uint64_t ept_maps;  // violaciones EPT resueltas en el kernel
  uint64_t tsc_run;   // dentro de AVM_RUN
  uint64_t tsc_setup; // preparando cada ventana (hasta la primera entrada)
  uint64_t tsc_guest; // entre VMLAUNCH/VMRESUME y la salida
  uint64_t tsc_leave; // al salir de la ventana (leer estado, VMCLEAR)
  uint64_t bp_skipped; // INT3 del huesped en anillo 0 saltados (sin depurador)
  uint64_t bp_rip;     // RIP del ultimo
};

#define AVM_CAP_VMX (1u << 0)
#define AVM_CAP_EPT (1u << 1)
#define AVM_CAP_UNRESTRICTED (1u << 2) // modo real / paginacion sin sombras
#define AVM_CAP_VPID (1u << 3)
#define AVM_CAP_USABLE (1u << 31)      // todo lo necesario para correr un huesped

struct avm_caps {
  uint32_t flags;
  uint32_t vmcs_revision;
  uint64_t ept_vpid_cap; // IA32_VMX_EPT_VPID_CAP
  uint32_t max_memory_mb;
  uint32_t reserved;
};

// Mapea [gpa, gpa+size) del huesped sobre un memfd del VMM (size = 0 quita el slot). El kernel rellena el
// EPT por demanda. gpa/size/offset: multiplos de 4 KiB.
struct avm_memory {
  int32_t fd;
  uint32_t slot; // 0..AVM_MAX_SLOTS-1; reusar el numero reemplaza el slot
  uint64_t gpa;
  uint64_t size;
  uint64_t offset; // dentro del memfd
};
#define AVM_MAX_SLOTS 8

struct avm_seg {
  uint64_t base;
  uint32_t limit;
  uint16_t selector;
  uint16_t pad;
  uint32_t ar; // bytes de acceso VT-x (tipo, S, DPL, P, AVL, L, D/B, G); bit 16 = inutilizable
};

struct avm_regs {
  uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
  uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
  uint64_t rip, rflags;
  uint64_t cr0, cr2, cr3, cr4, efer;
  struct avm_seg cs, ds, es, fs, gs, ss, tr, ldtr;
  uint64_t gdt_base, idt_base;
  uint32_t gdt_limit, idt_limit;
};

#define AVM_EXIT_NONE 0
#define AVM_EXIT_IO 1        // io.*
#define AVM_EXIT_MMIO 2      // mmio.*
#define AVM_EXIT_HLT 3
#define AVM_EXIT_SHUTDOWN 4  // triple fallo / apagado
#define AVM_EXIT_INTR_WINDOW 5
#define AVM_EXIT_SIGNAL 6    // el VMM tiene una senal o un plazo
#define AVM_EXIT_FAIL 7      // fail.* (error de VMX; hay detalle en el log del kernel)
#define AVM_EXIT_TPR 8       // el huesped cambio CR8: reevaluar interrupciones (y retirar la pendiente si ya no cabe)

struct avm_exit {
  uint32_t reason; // AVM_EXIT_*
  uint32_t hw_reason; // motivo VMX bruto (para depurar)
  union {
    struct {
      uint16_t port;
      uint8_t size;     // 1, 2 o 4
      uint8_t is_write; // 1 = OUT
      uint32_t count;   // 1 salvo INS/OUTS con REP
      uint32_t data;    // OUT: valor. IN: el VMM lo rellena antes del siguiente AVM_RUN
      // INS/OUTS (string=1): el VMM transfiere `count` elementos de `size` bytes
      // desde/hacia la direccion lineal `linear` (ES:rDI en INS, DS:rSI en OUTS) y
      // luego ajusta RSI/RDI y RCX (si rep) con AVM_SET_REGS. RIP ya avanzo.
      // asize: 0 = direcciones de 16 bits, 1 = 32, 2 = 64. ilen: longitud de la
      // instruccion (RIP - ilen = la propia INS/OUTS). Con KVM anidado asize y
      // linear pueden llegar mal: el VMM decodifica los prefijos y los rehace.
      uint8_t string;
      uint8_t asize;
      uint8_t rep;
      uint8_t ilen;
      uint64_t linear;
    } io;
    struct {
      uint64_t gpa;
      uint8_t size;
      uint8_t is_write;
      uint8_t pad[6];
      uint64_t data;
    } mmio;
    struct {
      uint64_t code;  // VM-instruction error o motivo de fallo de entrada
      uint64_t qual;
    } fail;
    uint64_t raw[4];
  };
  int32_t irq_pending; // AVM_INJECT_IRQ aun sin entregar (vector | flags) o -1
  uint32_t tpr;        // TPR actual del huesped (formato del registro 0x80)
};

#endif
