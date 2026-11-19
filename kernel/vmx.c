// kernel/vmx.c — mini-VM de AuroraOS: Intel VT-x + EPT.
//
// /dev/aurora-vm: un open() = una VM con un vCPU (kernel/aurora_vm.h). Diseno y
// fases en Plan_MiniVM.md. ESTA FASE (F1) NO SE HA EJECUTADO NUNCA: los hilos
// de desarrollo no tienen KVM. Escrita contra el SDM de Intel vol. 3C. Si algo
// falla, el kernel registra "[VMX] ..." con el campo VM-instruction error y el
// motivo de salida.
//
// Reglas:
//  * El estado del huesped vive en `vm->regs` (software). Cada entrada lo
//    escribe entero en el VMCS y al salir de la "ventana" se lee de vuelta.
//  * Ventana = preempt_disable + IRQs del host apagadas, con el VMCS cargado en
//    ESTA cpu. Al salir se hace VMCLEAR, asi el vCPU puede migrar entre CPUs.
//  * Salidas rapidas (CPUID, MSR, CR, fallos EPT de RAM, HLT sin pendientes no)
//    se resuelven dentro de la ventana; las que necesitan un dispositivo salen
//    a espacio de usuario.
#include "aurora_vm.h"
#include "vmx.h"
#include "cpu.h"
#include "devtmpfs.h"
#include "heap.h"
#include "klog.h"
#include "memfd.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"
#include "signal.h"
#include "string.h"
#include "uaccess.h"
#include "vfs.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// MSRs y bits
// ---------------------------------------------------------------------------
#define MSR_FEATURE_CONTROL 0x3A
#define MSR_VMX_BASIC 0x480
#define MSR_VMX_PINBASED 0x481
#define MSR_VMX_PROCBASED 0x482
#define MSR_VMX_EXIT 0x483
#define MSR_VMX_ENTRY 0x484
#define MSR_VMX_CR0_FIXED0 0x486
#define MSR_VMX_CR0_FIXED1 0x487
#define MSR_VMX_CR4_FIXED0 0x488
#define MSR_VMX_CR4_FIXED1 0x489
#define MSR_VMX_PROCBASED2 0x48B
#define MSR_VMX_EPT_VPID 0x48C
#define MSR_VMX_TRUE_PIN 0x48D
#define MSR_VMX_TRUE_PROC 0x48E
#define MSR_VMX_TRUE_EXIT 0x48F
#define MSR_VMX_TRUE_ENTRY 0x490

#define CR4_VMXE (1ULL << 13)

// Controles
#define PIN_EXT_INTR (1u << 0)
#define PROC_INTR_WINDOW (1u << 2)
#define PROC_HLT (1u << 7)
#define PROC_UNCOND_IO (1u << 24)
#define PROC_SECONDARY (1u << 31)
#define PROC2_EPT (1u << 1)
#define PROC2_RDTSCP (1u << 3)
#define PROC2_UNRESTRICTED (1u << 7)
#define PROC2_INVPCID (1u << 12)
#define EXIT_HOST_64 (1u << 9)
#define EXIT_SAVE_EFER (1u << 20)
#define EXIT_LOAD_EFER (1u << 21)
#define ENTRY_IA32E (1u << 9)
#define ENTRY_LOAD_EFER (1u << 15)

// Campos del VMCS (SDM vol. 3, apendice B)
enum {
  F_GUEST_ES_SEL = 0x800, F_GUEST_CS_SEL = 0x802, F_GUEST_SS_SEL = 0x804,
  F_GUEST_DS_SEL = 0x806, F_GUEST_FS_SEL = 0x808, F_GUEST_GS_SEL = 0x80A,
  F_GUEST_LDTR_SEL = 0x80C, F_GUEST_TR_SEL = 0x80E,
  F_HOST_ES_SEL = 0xC00, F_HOST_CS_SEL = 0xC02, F_HOST_SS_SEL = 0xC04,
  F_HOST_DS_SEL = 0xC06, F_HOST_FS_SEL = 0xC08, F_HOST_GS_SEL = 0xC0A,
  F_HOST_TR_SEL = 0xC0C,
  F_EXIT_MSR_STORE_ADDR = 0x2006, F_EXIT_MSR_LOAD_ADDR = 0x2008,
  F_ENTRY_MSR_LOAD_ADDR = 0x200A, F_EPT_POINTER = 0x201A,
  F_GUEST_PHYS_ADDR = 0x2400,
  F_VMCS_LINK = 0x2800, F_GUEST_DEBUGCTL = 0x2802, F_GUEST_EFER = 0x2806,
  F_HOST_EFER = 0x2C02,
  F_PIN = 0x4000, F_PROC = 0x4002, F_EXC_BITMAP = 0x4004, F_PF_MASK = 0x4006,
  F_PF_MATCH = 0x4008, F_CR3_COUNT = 0x400A, F_EXIT_CTLS = 0x400C,
  F_EXIT_MSR_STORE_CNT = 0x400E, F_EXIT_MSR_LOAD_CNT = 0x4010,
  F_ENTRY_CTLS = 0x4012, F_ENTRY_MSR_LOAD_CNT = 0x4014,
  F_ENTRY_INTR_INFO = 0x4016, F_ENTRY_ERRCODE = 0x4018,
  F_ENTRY_INSTR_LEN = 0x401A, F_PROC2 = 0x401E,
  F_VM_INSTR_ERROR = 0x4400, F_EXIT_REASON = 0x4402, F_EXIT_INTR_INFO = 0x4404,
  F_IDTV_INFO = 0x4408, F_IDTV_ERR = 0x440A, F_EXIT_INSTR_LEN = 0x440C,
  F_GUEST_ES_LIM = 0x4800, F_GUEST_CS_LIM = 0x4802, F_GUEST_SS_LIM = 0x4804,
  F_GUEST_DS_LIM = 0x4806, F_GUEST_FS_LIM = 0x4808, F_GUEST_GS_LIM = 0x480A,
  F_GUEST_LDTR_LIM = 0x480C, F_GUEST_TR_LIM = 0x480E, F_GUEST_GDTR_LIM = 0x4810,
  F_GUEST_IDTR_LIM = 0x4812,
  F_GUEST_ES_AR = 0x4814, F_GUEST_CS_AR = 0x4816, F_GUEST_SS_AR = 0x4818,
  F_GUEST_DS_AR = 0x481A, F_GUEST_FS_AR = 0x481C, F_GUEST_GS_AR = 0x481E,
  F_GUEST_LDTR_AR = 0x4820, F_GUEST_TR_AR = 0x4822,
  F_GUEST_INTERRUPTIBILITY = 0x4824, F_GUEST_ACTIVITY = 0x4826,
  F_GUEST_SYSENTER_CS = 0x482A, F_HOST_SYSENTER_CS = 0x4C00,
  F_CR0_MASK = 0x6000, F_CR4_MASK = 0x6002, F_CR0_SHADOW = 0x6004,
  F_CR4_SHADOW = 0x6006, F_EXIT_QUAL = 0x6400,
  F_GUEST_CR0 = 0x6800, F_GUEST_CR3 = 0x6802, F_GUEST_CR4 = 0x6804,
  F_GUEST_ES_BASE = 0x6806, F_GUEST_CS_BASE = 0x6808, F_GUEST_SS_BASE = 0x680A,
  F_GUEST_DS_BASE = 0x680C, F_GUEST_FS_BASE = 0x680E, F_GUEST_GS_BASE = 0x6810,
  F_GUEST_LDTR_BASE = 0x6812, F_GUEST_TR_BASE = 0x6814,
  F_GUEST_GDTR_BASE = 0x6816, F_GUEST_IDTR_BASE = 0x6818, F_GUEST_DR7 = 0x681A,
  F_GUEST_RSP = 0x681C, F_GUEST_RIP = 0x681E, F_GUEST_RFLAGS = 0x6820,
  F_GUEST_PENDING_DBG = 0x6822, F_GUEST_SYSENTER_ESP = 0x6824,
  F_GUEST_SYSENTER_EIP = 0x6826,
  F_HOST_CR0 = 0x6C00, F_HOST_CR3 = 0x6C02, F_HOST_CR4 = 0x6C04,
  F_HOST_FS_BASE = 0x6C06, F_HOST_GS_BASE = 0x6C08, F_HOST_TR_BASE = 0x6C0A,
  F_HOST_GDTR_BASE = 0x6C0C, F_HOST_IDTR_BASE = 0x6C0E,
  F_HOST_SYSENTER_ESP = 0x6C10, F_HOST_SYSENTER_EIP = 0x6C12,
};

// Motivos de salida (basic exit reason)
enum {
  XR_EXC_NMI = 0, XR_EXT_INTR = 1, XR_TRIPLE_FAULT = 2, XR_INTR_WINDOW = 7,
  XR_CPUID = 10, XR_HLT = 12, XR_CR_ACCESS = 28, XR_IO = 30, XR_RDMSR = 31,
  XR_WRMSR = 32, XR_INVALID_GUEST = 33, XR_MSR_LOAD_FAIL = 34,
  XR_EPT_VIOLATION = 48, XR_EPT_MISCONFIG = 49,
};

// ---------------------------------------------------------------------------
// Estructuras
// ---------------------------------------------------------------------------
struct gregs { // orden fijado por vmx_enter (asm)
  uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, r8, r9, r10, r11, r12, r13, r14,
      r15;
};

struct msr_ent {
  uint32_t index, reserved;
  uint64_t value;
};
// MSRs del huesped que el VMCS no guarda (SYSCALL y KERNEL_GS_BASE): se cargan
// y se guardan con las listas de MSR de VM-entry/VM-exit.
#define NGMSR 5
static const uint32_t guest_msr_idx[NGMSR] = {
    MSR_STAR, MSR_LSTAR, MSR_CSTAR, MSR_SFMASK, MSR_KERNEL_GS_BASE};

struct slot {
  bool used;
  uint64_t gpa, size, offset;
  file_descriptor_t *fd; // referencia al memfd mientras el slot viva
  vfs_node_t *node;
};

struct avm_vm {
  uint32_t magic;
  struct avm_regs regs; // estado del huesped (verdad)
  uint64_t cr0_shadow, cr4_shadow;
  uint64_t sysenter_cs, sysenter_esp, sysenter_eip;
  uint64_t pat, tsc_aux_unused;
  uint32_t interruptibility, activity;
  int32_t pending_irq; // -1 = ninguna
  // IN pendiente de completar en el siguiente AVM_RUN
  bool pending_in;
  uint8_t pending_in_size;
  uint8_t pending_in_len;
  struct slot slots[AVM_MAX_SLOTS];
  // EPT
  uint64_t ept_pml4; // fisica
  uint64_t *ept_tables;
  size_t ept_ntables, ept_cap;
  // VMCS + listas de MSR (una pagina)
  uint64_t vmcs_phys;
  uint64_t msr_page_phys;
  struct msr_ent *gmsr; // NGMSR: carga al entrar / guarda al salir
  struct msr_ent *hmsr; // NGMSR: carga al salir (valores del host)
  uint64_t fx_page_phys; // 2 x 512 B: fxsave host (0) y huesped (512)
  bool launched;
  volatile int busy;
  uint32_t procbased, procbased2, pinbased, exitctl, entryctl;
};
#define VM_MAGIC 0x41564D31u

// ---------------------------------------------------------------------------
// Estado global
// ---------------------------------------------------------------------------
static struct {
  bool probed, usable;
  struct avm_caps caps;
  uint64_t basic;
  bool true_ctls;
} g_vmx;

static uint64_t g_vmxon_phys[MAX_CPUS];
static bool g_vmxon_active[MAX_CPUS];

static inline uint64_t vmread(uint64_t field) {
  uint64_t v;
  __asm__ volatile("vmread %1, %0" : "=r"(v) : "r"(field) : "cc");
  return v;
}
static inline void vmwrite(uint64_t field, uint64_t v) {
  __asm__ volatile("vmwrite %1, %0" : : "r"(field), "rm"(v) : "cc");
}
static inline bool vmptrld(uint64_t phys) {
  uint8_t err;
  __asm__ volatile("vmptrld %1; setna %0" : "=q"(err) : "m"(phys) : "cc", "memory");
  return !err;
}
static inline bool vmclear(uint64_t phys) {
  uint8_t err;
  __asm__ volatile("vmclear %1; setna %0" : "=q"(err) : "m"(phys) : "cc", "memory");
  return !err;
}
static inline bool vmxon_insn(uint64_t phys) {
  uint8_t err;
  __asm__ volatile("vmxon %1; setna %0" : "=q"(err) : "m"(phys) : "cc", "memory");
  return !err;
}

static inline unsigned long irq_save(void) {
  unsigned long f;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) : : "memory");
  return f;
}
static inline void irq_restore(unsigned long f) {
  __asm__ volatile("pushq %0; popfq" : : "r"(f) : "memory", "cc");
}

// vmx_enter(gregs, launched): 0 = hubo salida de VM; 1 = VMLAUNCH/VMRESUME fallo.
extern int vmx_enter(struct gregs *g, int launched);
__asm__(
    ".text\n"
    ".globl vmx_enter\n"
    "vmx_enter:\n"
    "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n"
    "  pushq %r15\n"
    "  pushq %rdi\n"                  // [rsp] = gregs
    "  movl $0x6C14, %eax\n"          // HOST_RSP
    "  vmwrite %rsp, %rax\n"
    "  movl $0x6C16, %eax\n"          // HOST_RIP
    "  leaq vmx_exit_stub(%rip), %rdx\n"
    "  vmwrite %rdx, %rax\n"
    "  testl %esi, %esi\n"            // ZF=1 -> VMLAUNCH
    "  movq %rdi, %rax\n"
    "  movq 8(%rax), %rbx\n  movq 16(%rax), %rcx\n  movq 24(%rax), %rdx\n"
    "  movq 32(%rax), %rsi\n  movq 48(%rax), %rbp\n"
    "  movq 56(%rax), %r8\n  movq 64(%rax), %r9\n  movq 72(%rax), %r10\n"
    "  movq 80(%rax), %r11\n  movq 88(%rax), %r12\n  movq 96(%rax), %r13\n"
    "  movq 104(%rax), %r14\n  movq 112(%rax), %r15\n"
    "  movq 40(%rax), %rdi\n  movq 0(%rax), %rax\n"
    "  jz 2f\n"
    "  vmresume\n"
    "  jmp 3f\n"
    "2: vmlaunch\n"
    "3:\n"                            // solo se llega aqui si la entrada fallo
    "  addq $8, %rsp\n"
    "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n"
    "  popq %rbp\n"
    "  movl $1, %eax\n"
    "  ret\n"
    ".globl vmx_exit_stub\n"
    "vmx_exit_stub:\n"                // rsp = HOST_RSP; [rsp] = gregs
    "  pushq %rdi\n"
    "  movq 8(%rsp), %rdi\n"
    "  movq %rax, 0(%rdi)\n  movq %rbx, 8(%rdi)\n  movq %rcx, 16(%rdi)\n"
    "  movq %rdx, 24(%rdi)\n  movq %rsi, 32(%rdi)\n  movq %rbp, 48(%rdi)\n"
    "  movq %r8, 56(%rdi)\n  movq %r9, 64(%rdi)\n  movq %r10, 72(%rdi)\n"
    "  movq %r11, 80(%rdi)\n  movq %r12, 88(%rdi)\n  movq %r13, 96(%rdi)\n"
    "  movq %r14, 104(%rdi)\n  movq %r15, 112(%rdi)\n"
    "  popq %rax\n  movq %rax, 40(%rdi)\n"
    "  addq $8, %rsp\n"
    "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n"
    "  popq %rbp\n"
    "  xorl %eax, %eax\n"
    "  ret\n");

// ---------------------------------------------------------------------------
// Capacidades
// ---------------------------------------------------------------------------
static uint32_t adjust_ctl(uint32_t want, uint32_t msr) {
  uint64_t v = rdmsr(msr);
  uint32_t must1 = (uint32_t)v, may1 = (uint32_t)(v >> 32);
  return (want | must1) & may1;
}
static bool ctl_supported(uint32_t bits, uint32_t msr) {
  return (((uint32_t)(rdmsr(msr) >> 32)) & bits) == bits;
}

bool vmx_probe(struct avm_caps *out) {
  struct avm_caps c;
  memset(&c, 0, sizeof(c));
  uint32_t a, b, cx, d;
  cpuid(1, 0, &a, &b, &cx, &d);
  if (cx & (1u << 5)) {
    c.flags |= AVM_CAP_VMX;
    uint64_t basic = rdmsr(MSR_VMX_BASIC);
    c.vmcs_revision = (uint32_t)(basic & 0x7FFFFFFF);
    bool tc = (basic >> 55) & 1;
    uint32_t procmsr = tc ? MSR_VMX_TRUE_PROC : MSR_VMX_PROCBASED;
    if (ctl_supported(PROC_SECONDARY, procmsr)) {
      uint64_t p2 = rdmsr(MSR_VMX_PROCBASED2) >> 32;
      if (p2 & PROC2_EPT)
        c.flags |= AVM_CAP_EPT;
      if (p2 & PROC2_UNRESTRICTED)
        c.flags |= AVM_CAP_UNRESTRICTED;
      if (p2 & (1u << 5))
        c.flags |= AVM_CAP_VPID;
      c.ept_vpid_cap = rdmsr(MSR_VMX_EPT_VPID);
    }
    // 4 niveles, escritura-atras, paginas de 4 KiB
    if (!(c.ept_vpid_cap & (1ULL << 6)) || !(c.ept_vpid_cap & (1ULL << 14)))
      c.flags &= ~AVM_CAP_EPT;
    if ((c.flags & (AVM_CAP_EPT | AVM_CAP_UNRESTRICTED)) ==
        (AVM_CAP_EPT | AVM_CAP_UNRESTRICTED))
      c.flags |= AVM_CAP_USABLE;
  }
  cpuid(0x80000008, 0, &a, &b, &cx, &d);
  c.max_memory_mb = 1u << 20; // 1 TiB
  if (out)
    *out = c;
  return (c.flags & AVM_CAP_USABLE) != 0;
}

// Activa VMX en esta CPU (preempt desactivado). 0 o -errno.
static int vmx_enable_this_cpu(void) {
  int cpu = smp_processor_id();
  if (g_vmxon_active[cpu])
    return 0;
  uint64_t fc = rdmsr(MSR_FEATURE_CONTROL);
  if (!(fc & 1)) { // sin bloquear: la BIOS lo dejo libre; lo activamos
    wrmsr(MSR_FEATURE_CONTROL, fc | 1 | (1u << 2));
  } else if (!(fc & (1u << 2))) {
    LOG_WARN("[VMX] BIOS: VMX apagado en IA32_FEATURE_CONTROL (activa "
             "Intel Virtualization Technology)");
    return -ENODEV;
  }
  if (!g_vmxon_phys[cpu]) {
    uint64_t p = pmm_alloc_page();
    if (!p)
      return -ENOMEM;
    memset(phys_to_virt(p), 0, PAGE_SIZE);
    *(uint32_t *)phys_to_virt(p) = g_vmx.caps.vmcs_revision;
    g_vmxon_phys[cpu] = p;
  }
  uint64_t cr0 = read_cr0(), cr4 = read_cr4() | CR4_VMXE;
  uint64_t c0f0 = rdmsr(MSR_VMX_CR0_FIXED0), c0f1 = rdmsr(MSR_VMX_CR0_FIXED1);
  uint64_t c4f0 = rdmsr(MSR_VMX_CR4_FIXED0), c4f1 = rdmsr(MSR_VMX_CR4_FIXED1);
  if ((cr0 & c0f0) != c0f0 || (cr0 & ~c0f1) || (cr4 & c4f0) != c4f0 ||
      (cr4 & ~c4f1)) {
    LOG_ERR("[VMX] CR0/CR4 no cumplen los bits fijos (cr0=%lx cr4=%lx)", cr0,
            cr4);
    return -ENODEV;
  }
  write_cr4(cr4);
  if (!vmxon_insn(g_vmxon_phys[cpu])) {
    LOG_ERR("[VMX] VMXON fallo en cpu %d", cpu);
    write_cr4(cr4 & ~CR4_VMXE);
    return -ENODEV;
  }
  g_vmxon_active[cpu] = true;
  LOG_INFO("[VMX] VMXON en cpu %d", cpu);
  return 0;
}

// ---------------------------------------------------------------------------
// EPT
// ---------------------------------------------------------------------------
#define EPT_RWX 0x7ULL
#define EPT_WB (6ULL << 3)

static uint64_t ept_new_table(struct avm_vm *vm) {
  uint64_t p = pmm_alloc_page();
  if (!p)
    return 0;
  memset(phys_to_virt(p), 0, PAGE_SIZE);
  if (vm->ept_ntables == vm->ept_cap) {
    size_t nc = vm->ept_cap ? vm->ept_cap * 2 : 64;
    uint64_t *n = (uint64_t *)krealloc(vm->ept_tables, nc * sizeof(uint64_t));
    if (!n) {
      pmm_free_page(p);
      return 0;
    }
    vm->ept_tables = n;
    vm->ept_cap = nc;
  }
  vm->ept_tables[vm->ept_ntables++] = p;
  return p;
}

// Mapea una pagina de 4 KiB gpa -> hpa (RWX, WB).
static int ept_map_4k(struct avm_vm *vm, uint64_t gpa, uint64_t hpa) {
  uint64_t table = vm->ept_pml4;
  for (int level = 3; level >= 1; level--) {
    uint64_t *t = (uint64_t *)phys_to_virt(table);
    unsigned idx = (unsigned)((gpa >> (12 + 9 * level)) & 0x1FF);
    if (!(t[idx] & EPT_RWX)) {
      uint64_t n = ept_new_table(vm);
      if (!n)
        return -ENOMEM;
      t[idx] = n | EPT_RWX;
    }
    table = t[idx] & PTE_FRAME;
  }
  uint64_t *pt = (uint64_t *)phys_to_virt(table);
  pt[(gpa >> 12) & 0x1FF] = (hpa & PTE_FRAME) | EPT_RWX | EPT_WB;
  return 0;
}

static void invept_all(void) {
  if (!(g_vmx.caps.ept_vpid_cap & (1ULL << 20)))
    return;
  struct { uint64_t eptp, rsvd; } desc = {0, 0};
  uint64_t type = (g_vmx.caps.ept_vpid_cap & (1ULL << 26)) ? 2 : 1;
  if (type == 1) // sin global: no sabemos el EPTP aqui; los slots nuevos no lo necesitan
    return;
  __asm__ volatile("invept %0, %1" : : "m"(desc), "r"(type) : "cc", "memory");
}

static struct slot *find_slot(struct avm_vm *vm, uint64_t gpa) {
  for (int i = 0; i < AVM_MAX_SLOTS; i++) {
    struct slot *s = &vm->slots[i];
    if (s->used && gpa >= s->gpa && gpa - s->gpa < s->size)
      return s;
  }
  return NULL;
}

// Resuelve un fallo EPT contra los slots. 0 = mapeado, 1 = no es RAM (MMIO).
static int ept_fault(struct avm_vm *vm, uint64_t gpa) {
  struct slot *s = find_slot(vm, gpa);
  if (!s)
    return 1;
  gpa &= ~(uint64_t)(PAGE_SIZE - 1);
  uint64_t pgoff = (s->offset + (gpa - s->gpa)) / PAGE_SIZE;
  uint64_t phys = 0;
  if (!s->node->ops || !s->node->ops->get_page)
    return -EINVAL;
  int rc = s->node->ops->get_page(s->node, pgoff, &phys);
  if (rc < 0)
    return rc;
  return ept_map_4k(vm, gpa, phys);
}

static void ept_free_all(struct avm_vm *vm) {
  for (size_t i = 0; i < vm->ept_ntables; i++)
    pmm_free_page(vm->ept_tables[i]);
  kfree(vm->ept_tables);
  vm->ept_tables = NULL;
  vm->ept_ntables = vm->ept_cap = 0;
}

// ---------------------------------------------------------------------------
// Estado inicial: reset de un x86 (modo real, CS:IP = F000:FFF0)
// ---------------------------------------------------------------------------
static void vm_reset_regs(struct avm_vm *vm) {
  struct avm_regs *r = &vm->regs;
  memset(r, 0, sizeof(*r));
  r->rip = 0xFFF0;
  r->rflags = 2;
  r->cr0 = 0x60000010;
  r->cs = (struct avm_seg){.base = 0xFFFF0000, .limit = 0xFFFF,
                           .selector = 0xF000, .ar = 0x9B};
  struct avm_seg ds = {.base = 0, .limit = 0xFFFF, .selector = 0, .ar = 0x93};
  r->ds = r->es = r->fs = r->gs = r->ss = ds;
  r->tr = (struct avm_seg){.limit = 0xFFFF, .ar = 0x8B};
  r->ldtr = (struct avm_seg){.limit = 0xFFFF, .ar = 0x82};
  r->gdt_limit = r->idt_limit = 0xFFFF;
  vm->cr0_shadow = r->cr0;
  vm->cr4_shadow = 0;
  vm->pat = 0x0007040600070406ULL;
  vm->pending_irq = -1;
  vm->interruptibility = 0;
}

// ---------------------------------------------------------------------------
// Estado -> VMCS (con el VMCS cargado en esta CPU)
// ---------------------------------------------------------------------------
static uint64_t fixed_cr0(struct avm_vm *vm, uint64_t want) {
  uint64_t f0 = rdmsr(MSR_VMX_CR0_FIXED0), f1 = rdmsr(MSR_VMX_CR0_FIXED1);
  (void)vm;
  f0 &= ~((1ULL << 0) | (1ULL << 31)); // unrestricted guest: PE y PG libres
  return (want | f0) & f1;
}
static uint64_t fixed_cr4(uint64_t want) {
  return (want | rdmsr(MSR_VMX_CR4_FIXED0)) & rdmsr(MSR_VMX_CR4_FIXED1);
}
// Bits que el huesped no controla (los fuerza VMX): el huesped ve la sombra.
static uint64_t cr0_owned(void) {
  uint64_t f0 = rdmsr(MSR_VMX_CR0_FIXED0), f1 = rdmsr(MSR_VMX_CR0_FIXED1);
  f0 &= ~((1ULL << 0) | (1ULL << 31));
  return f0 | ~f1;
}
static uint64_t cr4_owned(void) {
  return rdmsr(MSR_VMX_CR4_FIXED0) | ~rdmsr(MSR_VMX_CR4_FIXED1);
}

static void write_seg(uint64_t sel_f, uint64_t base_f, uint64_t lim_f,
                      uint64_t ar_f, const struct avm_seg *s) {
  vmwrite(sel_f, s->selector);
  vmwrite(base_f, s->base);
  vmwrite(lim_f, s->limit);
  vmwrite(ar_f, s->ar);
}
static void read_seg(uint64_t sel_f, uint64_t base_f, uint64_t lim_f,
                     uint64_t ar_f, struct avm_seg *s) {
  s->selector = (uint16_t)vmread(sel_f);
  s->base = vmread(base_f);
  s->limit = (uint32_t)vmread(lim_f);
  s->ar = (uint32_t)vmread(ar_f);
}

static void gdt_sidt(uint64_t *gbase, uint16_t *glim, uint64_t *ibase,
                     uint16_t *ilim) {
  struct { uint16_t lim; uint64_t base; } __attribute__((packed)) g, i;
  __asm__ volatile("sgdt %0" : "=m"(g));
  __asm__ volatile("sidt %0" : "=m"(i));
  *gbase = g.base; *glim = g.lim; *ibase = i.base; *ilim = i.lim;
}

static void vmcs_setup_static(struct avm_vm *vm) {
  vm->pinbased = adjust_ctl(PIN_EXT_INTR, g_vmx.true_ctls ? MSR_VMX_TRUE_PIN
                                                          : MSR_VMX_PINBASED);
  vm->procbased = adjust_ctl(PROC_HLT | PROC_UNCOND_IO | PROC_SECONDARY,
                             g_vmx.true_ctls ? MSR_VMX_TRUE_PROC
                                             : MSR_VMX_PROCBASED);
  vm->procbased &= ~PROC_INTR_WINDOW;
  vm->procbased2 = adjust_ctl(PROC2_EPT | PROC2_UNRESTRICTED | PROC2_RDTSCP |
                                  PROC2_INVPCID,
                              MSR_VMX_PROCBASED2);
  vm->exitctl = adjust_ctl(EXIT_HOST_64 | EXIT_SAVE_EFER | EXIT_LOAD_EFER,
                           g_vmx.true_ctls ? MSR_VMX_TRUE_EXIT : MSR_VMX_EXIT);
  vm->entryctl = adjust_ctl(ENTRY_LOAD_EFER, g_vmx.true_ctls
                                                 ? MSR_VMX_TRUE_ENTRY
                                                 : MSR_VMX_ENTRY);
}

static void vmcs_write_controls_and_host(struct avm_vm *vm) {
  vmwrite(F_PIN, vm->pinbased);
  vmwrite(F_PROC, vm->procbased);
  vmwrite(F_PROC2, vm->procbased2);
  vmwrite(F_EXIT_CTLS, vm->exitctl);
  vmwrite(F_EXC_BITMAP, 0);
  vmwrite(F_PF_MASK, 0);
  vmwrite(F_PF_MATCH, 0);
  vmwrite(F_CR3_COUNT, 0);
  vmwrite(F_EPT_POINTER, vm->ept_pml4 | (3ULL << 3) | 6ULL);
  vmwrite(F_VMCS_LINK, ~0ULL);
  vmwrite(F_CR0_MASK, cr0_owned());
  vmwrite(F_CR4_MASK, cr4_owned());
  // Listas de MSR
  vmwrite(F_ENTRY_MSR_LOAD_ADDR, vm->msr_page_phys);
  vmwrite(F_EXIT_MSR_STORE_ADDR, vm->msr_page_phys);
  vmwrite(F_EXIT_MSR_LOAD_ADDR, vm->msr_page_phys + 256);
  vmwrite(F_ENTRY_MSR_LOAD_CNT, NGMSR);
  vmwrite(F_EXIT_MSR_STORE_CNT, NGMSR);
  vmwrite(F_EXIT_MSR_LOAD_CNT, NGMSR);
  for (int i = 0; i < NGMSR; i++)
    vm->hmsr[i].value = rdmsr(guest_msr_idx[i]);

  // Estado del host (la CPU puede haber cambiado desde la ultima entrada).
  vmwrite(F_HOST_CR0, read_cr0());
  vmwrite(F_HOST_CR3, read_cr3());
  vmwrite(F_HOST_CR4, read_cr4());
  uint16_t cs, ss, tr;
  __asm__ volatile("mov %%cs, %0" : "=r"(cs));
  __asm__ volatile("mov %%ss, %0" : "=r"(ss));
  __asm__ volatile("str %0" : "=r"(tr));
  vmwrite(F_HOST_CS_SEL, cs & ~7);
  vmwrite(F_HOST_SS_SEL, ss & ~7);
  vmwrite(F_HOST_DS_SEL, 0);
  vmwrite(F_HOST_ES_SEL, 0);
  vmwrite(F_HOST_FS_SEL, 0);
  vmwrite(F_HOST_GS_SEL, 0);
  vmwrite(F_HOST_TR_SEL, tr & ~7);
  uint64_t gb, ib;
  uint16_t gl, il;
  gdt_sidt(&gb, &gl, &ib, &il);
  vmwrite(F_HOST_GDTR_BASE, gb);
  vmwrite(F_HOST_IDTR_BASE, ib);
  // Base del TSS: descriptor de sistema de 16 bytes en la GDT.
  const uint8_t *d = (const uint8_t *)(gb + (tr & ~7));
  uint64_t tb = (uint64_t)d[2] | ((uint64_t)d[3] << 8) | ((uint64_t)d[4] << 16) |
                ((uint64_t)d[7] << 24) |
                ((uint64_t)*(const uint32_t *)(d + 8) << 32);
  vmwrite(F_HOST_TR_BASE, tb);
  vmwrite(F_HOST_FS_BASE, rdmsr(MSR_FS_BASE));
  vmwrite(F_HOST_GS_BASE, rdmsr(MSR_GS_BASE));
  vmwrite(F_HOST_SYSENTER_CS, rdmsr(0x174));
  vmwrite(F_HOST_SYSENTER_ESP, rdmsr(0x175));
  vmwrite(F_HOST_SYSENTER_EIP, rdmsr(0x176));
  vmwrite(F_HOST_EFER, rdmsr(MSR_EFER));
}

static void vmcs_write_guest(struct avm_vm *vm) {
  struct avm_regs *r = &vm->regs;
  vmwrite(F_GUEST_CR0, fixed_cr0(vm, r->cr0));
  vmwrite(F_CR0_SHADOW, vm->cr0_shadow);
  vmwrite(F_GUEST_CR3, r->cr3);
  vmwrite(F_GUEST_CR4, fixed_cr4(r->cr4));
  vmwrite(F_CR4_SHADOW, vm->cr4_shadow);
  vmwrite(F_GUEST_RSP, r->rsp);
  vmwrite(F_GUEST_RIP, r->rip);
  vmwrite(F_GUEST_RFLAGS, r->rflags | 2);
  vmwrite(F_GUEST_DR7, 0x400);
  vmwrite(F_GUEST_DEBUGCTL, 0);
  vmwrite(F_GUEST_EFER, r->efer);
  write_seg(F_GUEST_ES_SEL, F_GUEST_ES_BASE, F_GUEST_ES_LIM, F_GUEST_ES_AR, &r->es);
  write_seg(F_GUEST_CS_SEL, F_GUEST_CS_BASE, F_GUEST_CS_LIM, F_GUEST_CS_AR, &r->cs);
  write_seg(F_GUEST_SS_SEL, F_GUEST_SS_BASE, F_GUEST_SS_LIM, F_GUEST_SS_AR, &r->ss);
  write_seg(F_GUEST_DS_SEL, F_GUEST_DS_BASE, F_GUEST_DS_LIM, F_GUEST_DS_AR, &r->ds);
  write_seg(F_GUEST_FS_SEL, F_GUEST_FS_BASE, F_GUEST_FS_LIM, F_GUEST_FS_AR, &r->fs);
  write_seg(F_GUEST_GS_SEL, F_GUEST_GS_BASE, F_GUEST_GS_LIM, F_GUEST_GS_AR, &r->gs);
  write_seg(F_GUEST_TR_SEL, F_GUEST_TR_BASE, F_GUEST_TR_LIM, F_GUEST_TR_AR, &r->tr);
  write_seg(F_GUEST_LDTR_SEL, F_GUEST_LDTR_BASE, F_GUEST_LDTR_LIM, F_GUEST_LDTR_AR, &r->ldtr);
  vmwrite(F_GUEST_GDTR_BASE, r->gdt_base);
  vmwrite(F_GUEST_GDTR_LIM, r->gdt_limit);
  vmwrite(F_GUEST_IDTR_BASE, r->idt_base);
  vmwrite(F_GUEST_IDTR_LIM, r->idt_limit);
  vmwrite(F_GUEST_SYSENTER_CS, vm->sysenter_cs);
  vmwrite(F_GUEST_SYSENTER_ESP, vm->sysenter_esp);
  vmwrite(F_GUEST_SYSENTER_EIP, vm->sysenter_eip);
  vmwrite(F_GUEST_INTERRUPTIBILITY, vm->interruptibility);
  vmwrite(F_GUEST_ACTIVITY, 0);
  vmwrite(F_GUEST_PENDING_DBG, 0);
  vmwrite(F_ENTRY_INTR_INFO, 0);
  bool lma = (r->efer >> 10) & 1;
  vmwrite(F_ENTRY_CTLS, lma ? (vm->entryctl | ENTRY_IA32E)
                            : (vm->entryctl & ~ENTRY_IA32E));
}

static void vmcs_read_guest(struct avm_vm *vm) {
  struct avm_regs *r = &vm->regs;
  r->rsp = vmread(F_GUEST_RSP);
  r->rip = vmread(F_GUEST_RIP);
  r->rflags = vmread(F_GUEST_RFLAGS);
  r->cr3 = vmread(F_GUEST_CR3);
  r->efer = vmread(F_GUEST_EFER);
  // CR0/CR4 los lleva el software (sombra + valor real); no se leen de vuelta.
  read_seg(F_GUEST_ES_SEL, F_GUEST_ES_BASE, F_GUEST_ES_LIM, F_GUEST_ES_AR, &r->es);
  read_seg(F_GUEST_CS_SEL, F_GUEST_CS_BASE, F_GUEST_CS_LIM, F_GUEST_CS_AR, &r->cs);
  read_seg(F_GUEST_SS_SEL, F_GUEST_SS_BASE, F_GUEST_SS_LIM, F_GUEST_SS_AR, &r->ss);
  read_seg(F_GUEST_DS_SEL, F_GUEST_DS_BASE, F_GUEST_DS_LIM, F_GUEST_DS_AR, &r->ds);
  read_seg(F_GUEST_FS_SEL, F_GUEST_FS_BASE, F_GUEST_FS_LIM, F_GUEST_FS_AR, &r->fs);
  read_seg(F_GUEST_GS_SEL, F_GUEST_GS_BASE, F_GUEST_GS_LIM, F_GUEST_GS_AR, &r->gs);
  read_seg(F_GUEST_TR_SEL, F_GUEST_TR_BASE, F_GUEST_TR_LIM, F_GUEST_TR_AR, &r->tr);
  read_seg(F_GUEST_LDTR_SEL, F_GUEST_LDTR_BASE, F_GUEST_LDTR_LIM, F_GUEST_LDTR_AR, &r->ldtr);
  r->gdt_base = vmread(F_GUEST_GDTR_BASE);
  r->gdt_limit = (uint32_t)vmread(F_GUEST_GDTR_LIM);
  r->idt_base = vmread(F_GUEST_IDTR_BASE);
  r->idt_limit = (uint32_t)vmread(F_GUEST_IDTR_LIM);
  vm->sysenter_cs = vmread(F_GUEST_SYSENTER_CS);
  vm->sysenter_esp = vmread(F_GUEST_SYSENTER_ESP);
  vm->sysenter_eip = vmread(F_GUEST_SYSENTER_EIP);
  vm->interruptibility = (uint32_t)vmread(F_GUEST_INTERRUPTIBILITY);
}

// ---------------------------------------------------------------------------
// Inyeccion y emulacion rapida
// ---------------------------------------------------------------------------
static void inject_gp(void) {
  vmwrite(F_ENTRY_ERRCODE, 0);
  vmwrite(F_ENTRY_INTR_INFO, 13 | (3u << 8) | (1u << 11) | (1u << 31));
}

static void skip_insn(struct avm_vm *vm) {
  vm->regs.rip += vmread(F_EXIT_INSTR_LEN);
  vmwrite(F_GUEST_RIP, vm->regs.rip);
  vmwrite(F_GUEST_INTERRUPTIBILITY,
          vmread(F_GUEST_INTERRUPTIBILITY) & ~3ULL); // fin de STI/MOV SS shadow
}

static uint64_t *gpr(struct gregs *g, struct avm_vm *vm, int n) {
  switch (n) {
  case 0: return &g->rax; case 1: return &g->rcx; case 2: return &g->rdx;
  case 3: return &g->rbx; case 4: return &vm->regs.rsp; case 5: return &g->rbp;
  case 6: return &g->rsi; case 7: return &g->rdi; case 8: return &g->r8;
  case 9: return &g->r9; case 10: return &g->r10; case 11: return &g->r11;
  case 12: return &g->r12; case 13: return &g->r13; case 14: return &g->r14;
  default: return &g->r15;
  }
}

static void emu_cpuid(struct avm_vm *vm, struct gregs *g) {
  uint32_t leaf = (uint32_t)g->rax, sub = (uint32_t)g->rcx;
  uint32_t a, b, c, d;
  cpuid(leaf, sub, &a, &b, &c, &d);
  switch (leaf) {
  case 1:
    c &= ~((1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) |
           (1u << 7) | (1u << 8) | (1u << 12) | (1u << 15) | (1u << 17) |
           (1u << 21) | (1u << 24) | (1u << 26) | (1u << 27) | (1u << 28) |
           (1u << 29) | (1u << 31));
    d &= ~((1u << 9) | (1u << 21) | (1u << 22) | (1u << 28) | (1u << 29) |
           (1u << 31));
    b = (b & 0x0000FFFFu) | (1u << 16); // sin APIC id, 1 logico
    break;
  case 4:
    a &= 0x3FFF; // un solo hilo/nucleo
    break;
  case 7:
    if (sub == 0) {
      uint32_t invpcid = b & (1u << 10);
      b &= (1u << 0) | (1u << 3) | (1u << 7) | (1u << 8) | (1u << 9) |
           (1u << 18) | (1u << 19);
      if (vm->procbased2 & PROC2_INVPCID)
        b |= invpcid; // solo si el host lo tiene
      c = 0;
      d = 0;
    } else {
      a = b = c = d = 0;
    }
    break;
  case 0xB: case 0xD: case 0x12: case 0x14: case 0x1F:
    a = b = c = d = 0;
    break;
  case 0x80000001:
    if (!(vm->procbased2 & PROC2_RDTSCP))
      d &= ~(1u << 27);
    break;
  default:
    if (leaf >= 0x40000000 && leaf < 0x50000000)
      a = b = c = d = 0;
    break;
  }
  g->rax = a; g->rbx = b; g->rcx = c; g->rdx = d;
}

// RDMSR/WRMSR. Devuelve true si se emulo, false si hay que inyectar #GP.
static bool emu_msr(struct avm_vm *vm, struct gregs *g, bool write) {
  uint32_t idx = (uint32_t)g->rcx;
  uint64_t val = ((g->rdx & 0xFFFFFFFFULL) << 32) | (g->rax & 0xFFFFFFFFULL);
  uint64_t out = 0;
  bool ok = true;
  for (int i = 0; i < NGMSR; i++)
    if (idx == guest_msr_idx[i]) {
      if (write)
        vm->gmsr[i].value = val;
      else
        out = vm->gmsr[i].value;
      goto done;
    }
  switch (idx) {
  case MSR_EFER:
    if (write) {
      uint64_t keep = vm->regs.efer & (1ULL << 10); // LMA lo decide CR0.PG
      vm->regs.efer = (val & ~(1ULL << 10)) | keep;
      vmwrite(F_GUEST_EFER, vm->regs.efer);
    } else {
      out = vmread(F_GUEST_EFER);
    }
    break;
  case MSR_FS_BASE:
    if (write) vmwrite(F_GUEST_FS_BASE, val); else out = vmread(F_GUEST_FS_BASE);
    break;
  case MSR_GS_BASE:
    if (write) vmwrite(F_GUEST_GS_BASE, val); else out = vmread(F_GUEST_GS_BASE);
    break;
  case 0x174:
    if (write) vmwrite(F_GUEST_SYSENTER_CS, val); else out = vmread(F_GUEST_SYSENTER_CS);
    break;
  case 0x175:
    if (write) vmwrite(F_GUEST_SYSENTER_ESP, val); else out = vmread(F_GUEST_SYSENTER_ESP);
    break;
  case 0x176:
    if (write) vmwrite(F_GUEST_SYSENTER_EIP, val); else out = vmread(F_GUEST_SYSENTER_EIP);
    break;
  case 0x10: // TSC (sin offset en F1)
    if (write) break;
    { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); out = ((uint64_t)hi << 32) | lo; }
    break;
  case 0x277: // PAT: se recuerda pero no se aplica (F2: PAT del huesped)
    if (write) vm->pat = val; else out = vm->pat;
    break;
  case 0x1B:  // APIC_BASE: sin APIC en F1
  case 0x3A:  // FEATURE_CONTROL
  case 0x8B:  // microcode
  case 0x1A0: // MISC_ENABLE
  case 0xFE: case 0x2FF:               // MTRR cap / def type
  case 0x200 ... 0x20F: case 0x250: case 0x258: case 0x259:
  case 0x268 ... 0x26F:                // MTRR
    out = 0;
    break;
  default:
    ok = false;
  }
done:
  if (ok && !write) {
    g->rax = out & 0xFFFFFFFFULL;
    g->rdx = out >> 32;
  }
  return ok;
}

// Recalcula EFER.LMA y el control IA-32e tras tocar CR0.PG / EFER.LME.
static void update_long_mode(struct avm_vm *vm) {
  bool lme = (vm->regs.efer >> 8) & 1;
  bool pg = (vm->cr0_shadow >> 31) & 1;
  bool lma = lme && pg;
  vm->regs.efer = lma ? (vm->regs.efer | (1ULL << 10))
                      : (vm->regs.efer & ~(1ULL << 10));
  vmwrite(F_GUEST_EFER, vm->regs.efer);
  uint32_t e = (uint32_t)vmread(F_ENTRY_CTLS);
  vmwrite(F_ENTRY_CTLS, lma ? (e | ENTRY_IA32E) : (e & ~ENTRY_IA32E));
}

#define CR4_ALLOWED 0x1007FFULL

static bool emu_cr_access(struct avm_vm *vm, struct gregs *g) {
  uint64_t q = vmread(F_EXIT_QUAL);
  int cr = (int)(q & 0xF), type = (int)((q >> 4) & 3), reg = (int)((q >> 8) & 0xF);
  if (type == 2) { // CLTS
    vm->cr0_shadow &= ~(1ULL << 3);
    vm->regs.cr0 = vm->cr0_shadow;
    vmwrite(F_CR0_SHADOW, vm->cr0_shadow);
    vmwrite(F_GUEST_CR0, fixed_cr0(vm, vm->regs.cr0));
    skip_insn(vm);
    return true;
  }
  if (type == 3) { // LMSW: solo los 4 bits bajos
    uint64_t v = (q >> 16) & 0xF;
    vm->cr0_shadow = (vm->cr0_shadow & ~0xFULL) | v | (vm->cr0_shadow & 1);
    vm->regs.cr0 = vm->cr0_shadow;
    vmwrite(F_CR0_SHADOW, vm->cr0_shadow);
    vmwrite(F_GUEST_CR0, fixed_cr0(vm, vm->regs.cr0));
    skip_insn(vm);
    return true;
  }
  uint64_t *rp = gpr(g, vm, reg);
  if (reg == 4)
    vm->regs.rsp = vmread(F_GUEST_RSP);
  if (type == 1) { // MOV desde CR
    switch (cr) {
    case 0: *rp = vm->cr0_shadow; break;
    case 3: *rp = vmread(F_GUEST_CR3); break;
    case 4: *rp = vm->cr4_shadow; break;
    default: return false;
    }
    skip_insn(vm);
    return true;
  }
  uint64_t v = *rp;
  switch (cr) {
  case 0:
    if (((v >> 31) & 1) && !((v >> 0) & 1)) { inject_gp(); return true; }
    vm->cr0_shadow = v | (1ULL << 4); // ET siempre
    vm->regs.cr0 = vm->cr0_shadow;
    vmwrite(F_CR0_SHADOW, vm->cr0_shadow);
    vmwrite(F_GUEST_CR0, fixed_cr0(vm, vm->regs.cr0));
    update_long_mode(vm);
    break;
  case 3:
    vmwrite(F_GUEST_CR3, v);
    break;
  case 4:
    if (v & ~CR4_ALLOWED) { inject_gp(); return true; }
    vm->cr4_shadow = v;
    vm->regs.cr4 = v;
    vmwrite(F_CR4_SHADOW, v);
    vmwrite(F_GUEST_CR4, fixed_cr4(v));
    break;
  default:
    return false;
  }
  skip_insn(vm);
  return true;
}

// ---------------------------------------------------------------------------
// Un AVM_RUN
// ---------------------------------------------------------------------------
static void fail_exit(struct avm_exit *x, uint32_t reason, uint64_t code,
                      uint64_t qual) {
  x->reason = AVM_EXIT_FAIL;
  x->hw_reason = reason;
  x->fail.code = code;
  x->fail.qual = qual;
}

// Ejecuta una ventana. Devuelve 1 si hay que volver a espacio de usuario
// (x relleno), 0 si hay que repetir (tras ceder la CPU), <0 error.
static int run_window(struct avm_vm *vm, struct avm_exit *x) {
  struct gregs g;
  uint8_t *fx = (uint8_t *)phys_to_virt(vm->fx_page_phys);
  int ret = 0;

  preempt_disable();
  int rc = vmx_enable_this_cpu();
  if (rc < 0) {
    preempt_enable();
    return rc;
  }
  unsigned long fl = irq_save();
  if (!vmclear(vm->vmcs_phys) || !vmptrld(vm->vmcs_phys)) {
    irq_restore(fl);
    preempt_enable();
    fail_exit(x, 0, 0xFFFF, 0);
    return 1;
  }
  vm->launched = false; // tras VMCLEAR hay que usar VMLAUNCH
  vmcs_write_controls_and_host(vm);
  vmcs_write_guest(vm);
  memset(&g, 0, sizeof(g));
  g.rax = vm->regs.rax; g.rbx = vm->regs.rbx; g.rcx = vm->regs.rcx;
  g.rdx = vm->regs.rdx; g.rsi = vm->regs.rsi; g.rdi = vm->regs.rdi;
  g.rbp = vm->regs.rbp; g.r8 = vm->regs.r8; g.r9 = vm->regs.r9;
  g.r10 = vm->regs.r10; g.r11 = vm->regs.r11; g.r12 = vm->regs.r12;
  g.r13 = vm->regs.r13; g.r14 = vm->regs.r14; g.r15 = vm->regs.r15;
  __asm__ volatile("fxsave %0" : "=m"(*(uint8_t(*)[512])fx) : : "memory");
  __asm__ volatile("fxrstor %0" : : "m"(*(uint8_t(*)[512])(fx + 512)) : "memory");
  // Los MSR del huesped (SYSCALL...) los carga y guarda el hardware.
  bool need_intr_window = false;

  for (;;) {
    // Interrupcion pendiente del VMM: inyectar si el huesped puede recibirla.
    uint32_t proc = vm->procbased;
    if (vm->pending_irq >= 0) {
      uint64_t rfl = vmread(F_GUEST_RFLAGS);
      uint64_t intr = vmread(F_GUEST_INTERRUPTIBILITY);
      uint64_t info = vmread(F_ENTRY_INTR_INFO);
      if ((rfl & (1u << 9)) && !(intr & 3) && !(info & (1u << 31))) {
        vmwrite(F_ENTRY_INTR_INFO, (uint32_t)vm->pending_irq | (1u << 31));
        vm->pending_irq = -1;
      } else {
        proc |= PROC_INTR_WINDOW;
      }
    }
    (void)need_intr_window;
    vmwrite(F_PROC, proc);

    __asm__ volatile("mov %0, %%cr2" : : "r"(vm->regs.cr2) : "memory");
    int fail = vmx_enter(&g, vm->launched);
    __asm__ volatile("mov %%cr2, %0" : "=r"(vm->regs.cr2));
    if (fail) {
      uint64_t err = vmread(F_VM_INSTR_ERROR);
      LOG_ERR("[VMX] VM-entry fallo: VM-instruction error %lu (%s)", err,
              vm->launched ? "VMRESUME" : "VMLAUNCH");
      fail_exit(x, 0, err, 0);
      ret = 1;
      break;
    }
    vm->launched = true;

    uint32_t reason_full = (uint32_t)vmread(F_EXIT_REASON);
    uint32_t reason = reason_full & 0xFFFF;
    if (reason_full & (1u << 31)) { // fallo de entrada
      uint64_t qual = vmread(F_EXIT_QUAL);
      LOG_ERR("[VMX] VM-entry invalida: motivo %u qual=%lx (estado del huesped "
              "no valido o fallo al cargar MSR)", reason, qual);
      fail_exit(x, reason_full, reason, qual);
      ret = 1;
      break;
    }
    x->hw_reason = reason;

    // Reinyectar el evento que se estaba entregando cuando salimos.
    uint64_t idtv = vmread(F_IDTV_INFO);

    switch (reason) {
    case XR_EXT_INTR:
      // La IRQ del host queda pendiente: se sirve al restaurar IF. Ceder la CPU.
      ret = 0;
      goto leave;
    case XR_INTR_WINDOW:
      continue;
    case XR_EXC_NMI:
      __asm__ volatile("int $2"); // NMI del host que salto en el huesped
      continue;
    case XR_CPUID:
      vm->regs.rsp = vmread(F_GUEST_RSP);
      emu_cpuid(vm, &g);
      skip_insn(vm);
      continue;
    case XR_RDMSR:
    case XR_WRMSR:
      if (emu_msr(vm, &g, reason == XR_WRMSR))
        skip_insn(vm);
      else
        inject_gp();
      continue;
    case XR_CR_ACCESS:
      if (emu_cr_access(vm, &g))
        continue;
      fail_exit(x, reason, 0xC0, vmread(F_EXIT_QUAL));
      ret = 1;
      goto leave;
    case XR_EPT_VIOLATION: {
      uint64_t gpa = vmread(F_GUEST_PHYS_ADDR);
      uint64_t qual = vmread(F_EXIT_QUAL);
      int r2 = ept_fault(vm, gpa);
      if (r2 == 0) {
        if (idtv & (1u << 31)) {
          vmwrite(F_ENTRY_INTR_INFO, idtv);
          if (idtv & (1u << 11))
            vmwrite(F_ENTRY_ERRCODE, vmread(F_IDTV_ERR));
        }
        continue;
      }
      if (r2 < 0) {
        fail_exit(x, reason, (uint64_t)(-r2), gpa);
        ret = 1;
        goto leave;
      }
      x->reason = AVM_EXIT_MMIO; // F1: sin decodificar la instruccion
      x->mmio.gpa = gpa;
      x->mmio.size = 0;
      x->mmio.is_write = (qual >> 1) & 1;
      x->mmio.data = 0;
      ret = 1;
      goto leave;
    }
    case XR_IO: {
      uint64_t q = vmread(F_EXIT_QUAL);
      uint32_t size = (uint32_t)(q & 7) + 1;
      bool in = (q >> 3) & 1;
      bool string = (q >> 4) & 1;
      if (string) { // INS/OUTS: F2
        fail_exit(x, reason, 0x10, q);
        ret = 1;
        goto leave;
      }
      x->reason = AVM_EXIT_IO;
      x->io.port = (uint16_t)(q >> 16);
      x->io.size = (uint8_t)size;
      x->io.is_write = !in;
      x->io.count = 1;
      x->io.data = in ? 0 : (uint32_t)(g.rax & (size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1));
      uint64_t len = vmread(F_EXIT_INSTR_LEN);
      if (in) {
        vm->pending_in = true;
        vm->pending_in_size = (uint8_t)size;
        vm->pending_in_len = (uint8_t)len;
      } else {
        vm->regs.rip = vmread(F_GUEST_RIP) + len; // OUT: se completa ya
        vmwrite(F_GUEST_RIP, vm->regs.rip);
      }
      ret = 1;
      goto leave;
    }
    case XR_HLT:
      skip_insn(vm);
      x->reason = AVM_EXIT_HLT;
      ret = 1;
      goto leave;
    case XR_TRIPLE_FAULT:
      x->reason = AVM_EXIT_SHUTDOWN;
      ret = 1;
      goto leave;
    default: {
      uint64_t q = vmread(F_EXIT_QUAL);
      LOG_WARN("[VMX] salida sin manejar: motivo %u qual=%lx rip=%lx", reason, q,
               vmread(F_GUEST_RIP));
      fail_exit(x, reason, reason, q);
      ret = 1;
      goto leave;
    }
    }
  }

leave:
  // Estado del huesped de vuelta a software.
  vm->regs.rax = g.rax; vm->regs.rbx = g.rbx; vm->regs.rcx = g.rcx;
  vm->regs.rdx = g.rdx; vm->regs.rsi = g.rsi; vm->regs.rdi = g.rdi;
  vm->regs.rbp = g.rbp; vm->regs.r8 = g.r8; vm->regs.r9 = g.r9;
  vm->regs.r10 = g.r10; vm->regs.r11 = g.r11; vm->regs.r12 = g.r12;
  vm->regs.r13 = g.r13; vm->regs.r14 = g.r14; vm->regs.r15 = g.r15;
  vmcs_read_guest(vm);
  __asm__ volatile("fxsave %0" : "=m"(*(uint8_t(*)[512])(fx + 512)) : : "memory");
  __asm__ volatile("fxrstor %0" : : "m"(*(uint8_t(*)[512])fx) : "memory");
  vmclear(vm->vmcs_phys);
  irq_restore(fl);
  preempt_enable();
  return ret;
}

static int64_t vm_run(struct avm_vm *vm, uint64_t uarg) {
  struct avm_exit x;
  if (!g_vmx.usable)
    return -ENODEV;
  if (!access_ok((void *)uarg, sizeof(x)))
    return -EFAULT;
  if (__atomic_exchange_n(&vm->busy, 1, __ATOMIC_ACQUIRE))
    return -EBUSY;
  int64_t rc = 0;
  memset(&x, 0, sizeof(x));

  if (vm->pending_in) { // el VMM rellena io.data del IN anterior
    struct avm_exit prev;
    if (copy_from_user(&prev, (void *)uarg, sizeof(prev)) < 0) {
      rc = -EFAULT;
      goto out;
    }
    uint32_t mask = vm->pending_in_size == 4 ? 0xFFFFFFFFu
                                             : (1u << (8 * vm->pending_in_size)) - 1;
    vm->regs.rax = (vm->regs.rax & ~(uint64_t)mask) | (prev.io.data & mask);
    vm->regs.rip += vm->pending_in_len;
    vm->pending_in = false;
  }

  for (;;) {
    task_t *cur = sched_current();
    if (cur && signal_unblocked_of(cur)) {
      x.reason = AVM_EXIT_SIGNAL;
      break;
    }
    memset(&x, 0, sizeof(x));
    int r = run_window(vm, &x);
    if (r < 0) {
      rc = r;
      goto out;
    }
    if (r == 1)
      break;
    sched_yield(); // salida por IRQ del host: dejar que el planificador mande
  }
  if (copy_to_user((void *)uarg, &x, sizeof(x)) < 0)
    rc = -EFAULT;
out:
  __atomic_store_n(&vm->busy, 0, __ATOMIC_RELEASE);
  return rc;
}

// ---------------------------------------------------------------------------
// VFS
// ---------------------------------------------------------------------------
static struct avm_vm *node_vm(vfs_node_t *node) {
  struct avm_vm *vm = (struct avm_vm *)node->priv;
  return (vm && vm->magic == VM_MAGIC) ? vm : NULL;
}

static void slot_release(struct slot *s) {
  if (!s->used)
    return;
  vfs_fd_put_ref(s->fd);
  memset(s, 0, sizeof(*s));
}

static int avm_open(vfs_node_t *node, int flags) {
  (void)flags;
  if (!g_vmx.usable)
    return -ENODEV;
  struct avm_vm *vm = (struct avm_vm *)kzalloc(sizeof(*vm));
  if (!vm)
    return -ENOMEM;
  uint64_t vmcs = pmm_alloc_page(), msrp = pmm_alloc_page(),
           fxp = pmm_alloc_page();
  if (!vmcs || !msrp || !fxp) {
    if (vmcs) pmm_free_page(vmcs);
    if (msrp) pmm_free_page(msrp);
    if (fxp) pmm_free_page(fxp);
    kfree(vm);
    return -ENOMEM;
  }
  memset(phys_to_virt(vmcs), 0, PAGE_SIZE);
  memset(phys_to_virt(msrp), 0, PAGE_SIZE);
  memset(phys_to_virt(fxp), 0, PAGE_SIZE);
  *(uint32_t *)phys_to_virt(vmcs) = g_vmx.caps.vmcs_revision;
  vm->vmcs_phys = vmcs;
  vm->msr_page_phys = msrp;
  vm->fx_page_phys = fxp;
  vm->gmsr = (struct msr_ent *)phys_to_virt(msrp);
  vm->hmsr = (struct msr_ent *)((uint8_t *)phys_to_virt(msrp) + 256);
  for (int i = 0; i < NGMSR; i++) {
    vm->gmsr[i].index = guest_msr_idx[i];
    vm->hmsr[i].index = guest_msr_idx[i];
  }
  // fxsave del huesped inicial: estado FPU por defecto (FCW=0x37F, MXCSR=0x1F80)
  uint8_t *fx = (uint8_t *)phys_to_virt(fxp) + 512;
  *(uint16_t *)fx = 0x37F;
  *(uint32_t *)(fx + 24) = 0x1F80;
  vm->ept_pml4 = ept_new_table(vm);
  if (!vm->ept_pml4) {
    pmm_free_page(vmcs); pmm_free_page(msrp); pmm_free_page(fxp);
    kfree(vm);
    return -ENOMEM;
  }
  vm->magic = VM_MAGIC;
  vmcs_setup_static(vm);
  vm_reset_regs(vm);
  node->priv = vm;
  return 0;
}

static int avm_close(vfs_node_t *node) {
  struct avm_vm *vm = node_vm(node);
  if (!vm)
    return 0;
  node->priv = NULL;
  for (int i = 0; i < AVM_MAX_SLOTS; i++)
    slot_release(&vm->slots[i]);
  ept_free_all(vm);
  pmm_free_page(vm->vmcs_phys);
  pmm_free_page(vm->msr_page_phys);
  pmm_free_page(vm->fx_page_phys);
  vm->magic = 0;
  kfree(vm);
  return 0;
}

static int64_t avm_ioctl(vfs_node_t *node, unsigned long req, uint64_t arg) {
  if ((req & 0xFF00UL) != AVM_IOC_BASE)
    return -ENOTTY;
  if (req == AVM_GET_CAPS) {
    struct avm_caps c;
    vmx_probe(&c);
    if (!access_ok((void *)arg, sizeof(c)))
      return -EFAULT;
    return copy_to_user((void *)arg, &c, sizeof(c)) < 0 ? -EFAULT : 0;
  }
  struct avm_vm *vm = node_vm(node);
  if (!vm)
    return -ENODEV;
  switch (req) {
  case AVM_SET_MEMORY: {
    struct avm_memory m;
    if (!access_ok((void *)arg, sizeof(m)) ||
        copy_from_user(&m, (void *)arg, sizeof(m)) < 0)
      return -EFAULT;
    if (m.slot >= AVM_MAX_SLOTS || (m.gpa | m.size | m.offset) & (PAGE_SIZE - 1) ||
        !m.size || m.gpa + m.size < m.gpa || m.gpa + m.size > (1ULL << 40))
      return -EINVAL;
    if (vm->busy)
      return -EBUSY;
    file_descriptor_t *f = vfs_fd_get_current(m.fd);
    if (!f)
      return -EBADF;
    vfs_node_t *n = vfs_fd_node(f);
    if (!n || !memfd_is_node(n) || !n->ops || !n->ops->get_page ||
        m.offset + m.size > n->size) {
      vfs_fd_put_ref(f);
      return -EINVAL;
    }
    // No se solapa con otro slot.
    for (int i = 0; i < AVM_MAX_SLOTS; i++) {
      struct slot *o = &vm->slots[i];
      if (o->used && (uint32_t)i != m.slot && m.gpa < o->gpa + o->size &&
          o->gpa < m.gpa + m.size) {
        vfs_fd_put_ref(f);
        return -EEXIST;
      }
    }
    struct slot *s = &vm->slots[m.slot];
    bool replace = s->used;
    slot_release(s);
    s->used = true;
    s->gpa = m.gpa; s->size = m.size; s->offset = m.offset;
    s->fd = f; s->node = n;
    if (replace) { // las paginas ya mapeadas del slot viejo ya no valen
      ept_free_all(vm);
      vm->ept_pml4 = ept_new_table(vm);
      if (!vm->ept_pml4)
        return -ENOMEM;
      invept_all();
    }
    return 0;
  }
  case AVM_SET_REGS: {
    if (vm->busy)
      return -EBUSY;
    if (!access_ok((void *)arg, sizeof(vm->regs)))
      return -EFAULT;
    struct avm_regs r;
    if (copy_from_user(&r, (void *)arg, sizeof(r)) < 0)
      return -EFAULT;
    vm->regs = r;
    vm->cr0_shadow = r.cr0;
    vm->cr4_shadow = r.cr4;
    return 0;
  }
  case AVM_GET_REGS:
    if (!access_ok((void *)arg, sizeof(vm->regs)))
      return -EFAULT;
    return copy_to_user((void *)arg, &vm->regs, sizeof(vm->regs)) < 0 ? -EFAULT : 0;
  case AVM_RUN:
    return vm_run(vm, arg);
  case AVM_INJECT_IRQ:
    if (arg < 32 || arg > 255)
      return -EINVAL;
    vm->pending_irq = (int32_t)arg;
    return 0;
  }
  return -ENOTTY;
}

static vfs_ops_t avm_ops = {
    .open = avm_open,
    .close = avm_close,
    .ioctl = avm_ioctl,
};

void vmx_init(void) {
  g_vmx.usable = vmx_probe(&g_vmx.caps);
  g_vmx.basic = g_vmx.caps.flags & AVM_CAP_VMX ? rdmsr(MSR_VMX_BASIC) : 0;
  g_vmx.true_ctls = (g_vmx.basic >> 55) & 1;
  g_vmx.probed = true;
  LOG_INFO("[VMX] caps=%x ept_vpid=%lx usable=%d", g_vmx.caps.flags,
           g_vmx.caps.ept_vpid_cap, (int)g_vmx.usable);
  devtmpfs_register("aurora-vm", DEVT_MKDEV(10, 233), 0660, 0, 0, &avm_ops, NULL);
}
