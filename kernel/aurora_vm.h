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
#define AVM_INJECT_IRQ (AVM_IOC_BASE + 6)  // uint32_t vector (por valor); -EBUSY si ya hay una pendiente
#define AVM_SET_TIMEOUT (AVM_IOC_BASE + 7) // ms (por valor): AVM_RUN devuelve AVM_EXIT_SIGNAL tras ese tiempo; 0 = sin limite

#define AVM_SET_DEBUG (AVM_IOC_BASE + 8)   // flags (por valor): bit0 = interceptar #UD/#DF/#GP/#PF y registrarlos en el log del kernel

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

// Mapea [gpa, gpa+size) del huesped sobre un memfd del VMM. El kernel rellena el
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
      // asize: 0 = direcciones de 16 bits, 1 = 32, 2 = 64.
      uint8_t string;
      uint8_t asize;
      uint8_t rep;
      uint8_t pad2;
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
};

#endif
