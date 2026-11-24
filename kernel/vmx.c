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
//    ESTA cpu. Queda activo entre ventanas; al cambiar de tarea se hace VMCLEAR
//    (vmx_sched_out), asi el vCPU puede migrar entre CPUs.
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
#include "time.h"
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
#define PIN_NMI (1u << 3)
#define PROC_INTR_WINDOW (1u << 2)
#define PROC_HLT (1u << 7)
#define PROC_MWAIT (1u << 10)
#define PROC_MOV_DR (1u << 23)
#define PROC_CR8_LOAD (1u << 19)
#define PROC_CR8_STORE (1u << 20)
#define PROC_MONITOR (1u << 29)
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
  F_GUEST_PDPTE0 = 0x280A,
  F_VMCS_LINK = 0x2800, F_GUEST_DEBUGCTL = 0x2802, F_GUEST_EFER = 0x2806,
  F_HOST_EFER = 0x2C02,
  F_PIN = 0x4000, F_PROC = 0x4002, F_EXC_BITMAP = 0x4004, F_PF_MASK = 0x4006,
  F_PF_MATCH = 0x4008, F_CR3_COUNT = 0x400A, F_EXIT_CTLS = 0x400C,
  F_EXIT_MSR_STORE_CNT = 0x400E, F_EXIT_MSR_LOAD_CNT = 0x4010,
  F_ENTRY_CTLS = 0x4012, F_ENTRY_MSR_LOAD_CNT = 0x4014,
  F_ENTRY_INTR_INFO = 0x4016, F_ENTRY_ERRCODE = 0x4018,
  F_ENTRY_INSTR_LEN = 0x401A, F_PROC2 = 0x401E,
  F_VM_INSTR_ERROR = 0x4400, F_EXIT_REASON = 0x4402, F_EXIT_INTR_INFO = 0x4404,
  F_IDTV_INFO = 0x4408, F_IDTV_ERR = 0x440A, F_EXIT_INSTR_LEN = 0x440C, F_EXIT_INSTR_INFO = 0x440E, F_GUEST_LINEAR = 0x640A,
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
  XR_GETSEC = 11, XR_INVD = 13, XR_RSM = 17, XR_VMX_FIRST = 18, XR_VMX_LAST = 27,
  XR_MWAIT = 36, XR_MONITOR = 39, XR_MOV_DR = 29, XR_XSETBV = 55,
  XR_CPUID = 10, XR_HLT = 12, XR_CR_ACCESS = 28, XR_IO = 30, XR_RDMSR = 31,
  XR_WRMSR = 32, XR_INVALID_GUEST = 33, XR_MSR_LOAD_FAIL = 34,
  XR_EPT_VIOLATION = 48, XR_EPT_MISCONFIG = 49,
  XR_INIT = 3, XR_SIPI = 4, XR_TASK_SWITCH = 9, XR_INVLPG = 14, XR_RDPMC = 15,
  XR_INVEPT = 50, XR_INVVPID = 53, XR_WBINVD = 54, XR_VMFUNC = 59,
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
// MSRs del huesped que el VMCS no guarda (SYSCALL, KERNEL_GS_BASE y TSC_AUX):
// se cargan y se guardan con las listas de MSR de VM-entry/VM-exit. TSC_AUX
// solo si la CPU tiene RDTSCP (si no, leerlo en el host daria #GP).
#define NGMSR_MAX 6
#define MSR_TSC_AUX 0xC0000103u
static const uint32_t guest_msr_idx[NGMSR_MAX] = {
    MSR_STAR, MSR_LSTAR, MSR_CSTAR, MSR_SFMASK, MSR_KERNEL_GS_BASE, MSR_TSC_AUX};
static int g_ngmsr = 5;

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
  // MTRR virtuales (como KVM): solo se guardan. Con EPT el tipo de memoria lo
  // decide el EPT (WB) con el PAT del huesped; los MTRR del huesped no se aplican.
  uint64_t mtrr_def, mtrr_fix[11], mtrr_var[16];
  uint32_t interruptibility, activity;
  int32_t pending_irq; // -1 = ninguna; vector | AVM_IRQ_TPR
  uint32_t tpr;        // TPR del LAPIC (registro 0x80); CR8 = tpr >> 4
  uint32_t tpr_wait;   // clase bloqueada por el TPR en el VMM (0 = ninguna)
  // Evento que se estaba entregando cuando hubo una salida (IDT-vectoring):
  // se vuelve a inyectar en la siguiente entrada o se perderia.
  bool reinj;
  uint32_t reinj_info, reinj_err, reinj_len;
  uint32_t stuck_logged; // avisos de "256 salidas inyectando" ya escritos
  struct avm_stats st;
  // VMCS incremental: el contenido del VMCS sobrevive a VMCLEAR/VMPTRLD, asi que
  // cada ventana solo escribe lo que cambio. `snap` es lo que el VMCS tiene (en
  // los campos que el kernel copia); `regs_stale`: tras una ventana solo RIP,
  // RSP, RFLAGS, CR0/CR3/CR4, EFER e interrumpibilidad estan al dia en `regs`
  // (el resto se lee del VMCS al pedirlo: AVM_GET_REGS/SET_REGS).
  struct avm_regs snap;
  uint64_t snap_cr0_shadow, snap_cr4_shadow;
  bool vmcs_full;      // hay que escribir todo (VMCS nuevo)
  bool vmcs_cleared;   // ya se hizo el VMCLEAR inicial
  bool regs_stale;
  // Con regs_stale: ademas de lo anterior, CS/SS (AR y base) y las bases de
  // ES/DS/FS/GS estan al dia (lo que usan la emulacion MMIO e INS/OUTS del VMM:
  // AVM_GET_REGS_LITE). Con KVM anidado leer el resto del VMCS (selectores,
  // limites, GDTR/IDTR, TR/LDTR, SYSENTER) cuesta una salida al hipervisor de
  // fuera por campo, ~130 us en cada salida MMIO.
  bool regs_lite_ok;
  uint64_t host_cr0_c, host_cr3_c; // ultimos HOST_CR0/CR3 escritos en el VMCS
  int ctl_cpu;         // CPU para la que se escribio el estado del host (-1 = ninguna)
  uint32_t ctl_debug;  // vm->debug con el que se escribio el bitmap de excepciones
  uint32_t msr_logged;
  uint32_t timeout_ms;
  uint64_t dr[8];
  uint32_t debug, exc_logged;
  uint32_t vmflags; // AVM_VMF_*
  uint64_t last_exc_rip, last_exc_qual;
  uint32_t last_exc_vec, exc_repeat;
  struct avm_fast fast; // atajos de dispositivos (ver aurora_vm.h)
  // IN pendiente de completar en el siguiente AVM_RUN
  bool pending_in;
  uint8_t pending_in_size;
  uint8_t pending_in_len;
  struct slot slots[AVM_MAX_SLOTS];
  // EPT
  uint64_t ept_pml4; // fisica
  bool ept_dirty;    // el EPT cambio: INVEPT antes de la siguiente entrada
  int last_cpu;      // CPU de la ultima ventana (-1 = ninguna)
  uint64_t *ept_tables;
  size_t ept_ntables, ept_cap;
  // VMCS + listas de MSR (una pagina)
  uint64_t vmcs_phys;
  uint64_t msr_page_phys;
  struct msr_ent *gmsr; // g_ngmsr: carga al entrar / guarda al salir
  struct msr_ent *hmsr; // g_ngmsr: carga al salir (valores del host)
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
// VMCS que una ventana dejo activo y actual en cada CPU (sin VMCLEAR). Mientras
// la tarea no sale de la CPU, la siguiente ventana entra con VMRESUME sin
// VMCLEAR/VMPTRLD (con KVM anidado cada uno obliga a resincronizar el VMCS
// entero). Al cambiar de tarea, vmx_sched_out() hace el VMCLEAR: asi un VMCS
// nunca queda activo en una CPU distinta de la que lo va a usar.
static uint64_t g_loaded_vmcs[MAX_CPUS];

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
  uint64_t c0f0 = rdmsr(MSR_VMX_CR0_FIXED0), c0f1 = rdmsr(MSR_VMX_CR0_FIXED1);
  uint64_t c4f0 = rdmsr(MSR_VMX_CR4_FIXED0), c4f1 = rdmsr(MSR_VMX_CR4_FIXED1);
  // El kernel corre con CR0.NE=0; VMX lo exige a 1 (como hace Linux): se
  // suben los bits que piden los MSR fijos antes de comprobar.
  uint64_t cr0 = read_cr0() | c0f0, cr4 = read_cr4() | CR4_VMXE | c4f0;
  if (cr0 != read_cr0())
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
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

#define EPT_CLUSTER 64 // paginas (256 KiB) que se mapean por cada fallo EPT

// Tabla de ultimo nivel (PT) del EPT que cubre gpa; la crea si falta.
static uint64_t *ept_get_pt(struct avm_vm *vm, uint64_t gpa) {
  uint64_t table = vm->ept_pml4;
  for (int level = 3; level >= 1; level--) {
    uint64_t *t = (uint64_t *)phys_to_virt(table);
    unsigned idx = (unsigned)((gpa >> (12 + 9 * level)) & 0x1FF);
    if (!(t[idx] & EPT_RWX)) {
      uint64_t n = ept_new_table(vm);
      if (!n)
        return NULL;
      t[idx] = n | EPT_RWX;
    }
    table = t[idx] & PTE_FRAME;
  }
  return (uint64_t *)phys_to_virt(table);
}

// Invalida en ESTA CPU las traducciones cacheadas del EPT de la VM (fisicas del
// huesped y combinadas). INVEPT solo afecta a la CPU que lo ejecuta: se llama al
// empezar cada ventana si el EPT cambio o la VM viene de otra CPU.
static void invept_ctx(uint64_t eptp) {
  uint64_t cap = g_vmx.caps.ept_vpid_cap;
  if (!(cap & (1ULL << 20)))
    return;
  struct { uint64_t eptp, rsvd; } desc = {eptp, 0};
  uint64_t type;
  if (cap & (1ULL << 25))
    type = 1; // un contexto
  else if (cap & (1ULL << 26))
    type = 2; // todos
  else
    return;
  __asm__ volatile("invept %0, %1" : : "m"(desc), "r"(type) : "cc", "memory");
}

static uint64_t vm_eptp(struct avm_vm *vm) { return vm->ept_pml4 | (3ULL << 3) | 6ULL; }

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
  if (!s->node->ops || !s->node->ops->get_page)
    return -EINVAL;
  // Se mapea el bloque alineado de EPT_CLUSTER paginas que contiene gpa (dentro
  // del slot): un fallo EPT por bloque en vez de uno por pagina. Cada fallo es
  // una salida de la VM (con KVM anidado, decenas de us) y Windows/ReactOS
  // tocan toda la RAM al ponerla a cero. Las paginas ya mapeadas no se tocan.
  uint64_t c0 = gpa & ~(uint64_t)(EPT_CLUSTER * PAGE_SIZE - 1);
  uint64_t c1 = c0 + EPT_CLUSTER * PAGE_SIZE;
  if (c0 < s->gpa)
    c0 = s->gpa;
  if (c1 > s->gpa + s->size)
    c1 = s->gpa + s->size;
  uint64_t *pt = ept_get_pt(vm, gpa); // EPT_CLUSTER <= 512: el bloque cae en una sola PT
  if (!pt)
    return -ENOMEM;
  for (uint64_t g = c0; g < c1; g += PAGE_SIZE) {
    uint64_t *e = &pt[(g >> 12) & 0x1FF];
    if (*e & EPT_RWX)
      continue;
    uint64_t phys = 0;
    int rc = s->node->ops->get_page(s->node, (s->offset + (g - s->gpa)) / PAGE_SIZE, &phys);
    if (rc < 0) {
      if (g == gpa)
        return rc; // la pagina que fallo si hace falta; las vecinas no
      continue;
    }
    *e = (phys & PTE_FRAME) | EPT_RWX | EPT_WB;
  }
  return 0;
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
  vm->mtrr_def = 0;
  memset(vm->mtrr_fix, 0, sizeof(vm->mtrr_fix));
  memset(vm->mtrr_var, 0, sizeof(vm->mtrr_var));
  vm->pending_irq = -1;
  vm->interruptibility = 0;
}

// ---------------------------------------------------------------------------
// Estado -> VMCS (con el VMCS cargado en esta CPU)
// ---------------------------------------------------------------------------
// MSR de VMX leidos una vez: con KVM anidado cada RDMSR de estos es una salida
// al hipervisor de fuera, y se leian unas 16 veces por ventana.
static uint64_t g_c0f0, g_c0f1, g_c4f0, g_c4f1;
static inline uint64_t rdtsc_k(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}
static uint64_t fixed_cr0(struct avm_vm *vm, uint64_t want) {
  uint64_t f0 = g_c0f0, f1 = g_c0f1;
  (void)vm;
  f0 &= ~((1ULL << 0) | (1ULL << 31)); // unrestricted guest: PE y PG libres
  return (want | f0) & f1;
}
static uint64_t fixed_cr4(uint64_t want) {
  return (want | g_c4f0) & g_c4f1;
}
// Bits que el huesped no controla (los fuerza VMX): el huesped ve la sombra.
static uint64_t cr0_owned(void) {
  uint64_t f0 = g_c0f0, f1 = g_c0f1;
  f0 &= ~((1ULL << 0) | (1ULL << 31));
  // PE y PG tambien se interceptan: el kernel decide EFER.LMA y el control
  // IA-32e al activar la paginacion (el hardware no lo hace dentro del VMX).
  return f0 | ~f1 | (1ULL << 0) | (1ULL << 31);
}
static uint64_t cr4_owned(void) {
  return g_c4f0 | ~g_c4f1;
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
  vm->pinbased = adjust_ctl(PIN_EXT_INTR | PIN_NMI, g_vmx.true_ctls ? MSR_VMX_TRUE_PIN
                                                          : MSR_VMX_PINBASED);
  // CR8 siempre intercepta: sin esto (y sin TPR shadow) MOV CR8 del huesped
  // escribiria el TPR del APIC REAL del host.
  vm->procbased = adjust_ctl(PROC_HLT | PROC_UNCOND_IO | PROC_SECONDARY |
                                 PROC_MOV_DR | PROC_MWAIT | PROC_MONITOR |
                                 PROC_CR8_LOAD | PROC_CR8_STORE,
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
  vmwrite(F_EXC_BITMAP, (1u << 3) | (vm->debug ? ((1u << 6) | (1u << 8) | (1u << 13) | (1u << 14)) : 0));
  vmwrite(F_PF_MASK, 0);
  vmwrite(F_PF_MATCH, 0);
  vmwrite(F_CR3_COUNT, 0);
  vmwrite(F_EPT_POINTER, vm_eptp(vm));
  vmwrite(F_VMCS_LINK, ~0ULL);
  vmwrite(F_CR0_MASK, cr0_owned());
  vmwrite(F_CR4_MASK, cr4_owned());
  // Listas de MSR
  vmwrite(F_ENTRY_MSR_LOAD_ADDR, vm->msr_page_phys);
  vmwrite(F_EXIT_MSR_STORE_ADDR, vm->msr_page_phys);
  vmwrite(F_EXIT_MSR_LOAD_ADDR, vm->msr_page_phys + 256);
  vmwrite(F_ENTRY_MSR_LOAD_CNT, g_ngmsr);
  vmwrite(F_EXIT_MSR_STORE_CNT, g_ngmsr);
  vmwrite(F_EXIT_MSR_LOAD_CNT, g_ngmsr);
  for (int i = 0; i < g_ngmsr; i++)
    vm->hmsr[i].value = rdmsr(guest_msr_idx[i]);

  // Estado del host (la CPU puede haber cambiado desde la ultima entrada).
  vm->host_cr0_c = read_cr0();
  vm->host_cr3_c = read_cr3();
  vmwrite(F_HOST_CR0, vm->host_cr0_c);
  vmwrite(F_HOST_CR3, vm->host_cr3_c);
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
  // CR0/CR4: los bits que VMX no posee (PE, PG, PAE...) los cambia el huesped
  // directamente en el VMCS sin salir; los poseidos por VMX (NE...) viven en la
  // sombra. Hay que releer los primeros o la siguiente entrada los desharia.
  {
    uint64_t o0 = cr0_owned(), o4 = cr4_owned();
    r->cr0 = (vm->cr0_shadow & o0) | (vmread(F_GUEST_CR0) & ~o0);
    vm->cr0_shadow = r->cr0;
    r->cr4 = (vm->cr4_shadow & o4) | (vmread(F_GUEST_CR4) & ~o4);
    vm->cr4_shadow = r->cr4;
  }
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

// Lo minimo al salir de cada ventana: lo que usan el kernel y el VMM en todas
// las salidas. El resto se lee bajo demanda (vm_sync_regs).
static void vmcs_read_hot(struct avm_vm *vm) {
  struct avm_regs *r = &vm->regs;
  r->rsp = vmread(F_GUEST_RSP);
  r->rip = vmread(F_GUEST_RIP);
  r->rflags = vmread(F_GUEST_RFLAGS);
  r->cr3 = vmread(F_GUEST_CR3);
  r->efer = vmread(F_GUEST_EFER);
  uint64_t o0 = cr0_owned(), o4 = cr4_owned();
  r->cr0 = (vm->cr0_shadow & o0) | (vmread(F_GUEST_CR0) & ~o0);
  vm->cr0_shadow = r->cr0;
  r->cr4 = (vm->cr4_shadow & o4) | (vmread(F_GUEST_CR4) & ~o4);
  vm->cr4_shadow = r->cr4;
  vm->interruptibility = (uint32_t)vmread(F_GUEST_INTERRUPTIBILITY);
  vm->snap.rsp = r->rsp; vm->snap.rip = r->rip; vm->snap.rflags = r->rflags;
  vm->snap.cr3 = r->cr3; vm->snap.efer = r->efer;
  vm->snap.cr0 = r->cr0; vm->snap.cr4 = r->cr4;
  vm->snap_cr0_shadow = vm->cr0_shadow;
  vm->snap_cr4_shadow = vm->cr4_shadow;
}

// Lo que usa el VMM para emular MMIO e INS/OUTS, ademas de lo de vmcs_read_hot:
// AR y base de CS/SS y bases de ES/DS/FS/GS (con KVM anidado son campos del VMCS
// en sombra: leerlos no sale al hipervisor de fuera).
static void vmcs_read_lite(struct avm_vm *vm) {
  struct avm_regs *r = &vm->regs;
  r->cs.ar = (uint32_t)vmread(F_GUEST_CS_AR);
  r->cs.base = vmread(F_GUEST_CS_BASE);
  r->ss.ar = (uint32_t)vmread(F_GUEST_SS_AR);
  r->ss.base = vmread(F_GUEST_SS_BASE);
  r->es.base = vmread(F_GUEST_ES_BASE);
  r->ds.base = vmread(F_GUEST_DS_BASE);
  r->fs.base = vmread(F_GUEST_FS_BASE);
  r->gs.base = vmread(F_GUEST_GS_BASE);
  vm->snap.cs.ar = r->cs.ar; vm->snap.cs.base = r->cs.base;
  vm->snap.ss.ar = r->ss.ar; vm->snap.ss.base = r->ss.base;
  vm->snap.es.base = r->es.base; vm->snap.ds.base = r->ds.base;
  vm->snap.fs.base = r->fs.base; vm->snap.gs.base = r->gs.base;
}

static bool seg_eq(const struct avm_seg *a, const struct avm_seg *b) {
  return a->base == b->base && a->limit == b->limit && a->selector == b->selector && a->ar == b->ar;
}

// Escribe en el VMCS solo los campos de `regs` que difieren de `snap`.
static void vmcs_write_guest_diff(struct avm_vm *vm) {
  struct avm_regs *r = &vm->regs, *o = &vm->snap;
  if (r->cr0 != o->cr0 || vm->cr0_shadow != vm->snap_cr0_shadow) {
    vmwrite(F_GUEST_CR0, fixed_cr0(vm, r->cr0));
    vmwrite(F_CR0_SHADOW, vm->cr0_shadow);
  }
  if (r->cr4 != o->cr4 || vm->cr4_shadow != vm->snap_cr4_shadow) {
    vmwrite(F_GUEST_CR4, fixed_cr4(r->cr4));
    vmwrite(F_CR4_SHADOW, vm->cr4_shadow);
  }
  if (r->cr3 != o->cr3) vmwrite(F_GUEST_CR3, r->cr3);
  if (r->rsp != o->rsp) vmwrite(F_GUEST_RSP, r->rsp);
  if (r->rip != o->rip) vmwrite(F_GUEST_RIP, r->rip);
  if (r->rflags != o->rflags) vmwrite(F_GUEST_RFLAGS, r->rflags | 2);
  if (r->efer != o->efer) {
    vmwrite(F_GUEST_EFER, r->efer);
    bool lma = (r->efer >> 10) & 1;
    vmwrite(F_ENTRY_CTLS, lma ? (vm->entryctl | ENTRY_IA32E) : (vm->entryctl & ~ENTRY_IA32E));
  }
#define SEGW(n, N)                                                                         \
  if (!seg_eq(&r->n, &o->n))                                                               \
    write_seg(F_GUEST_##N##_SEL, F_GUEST_##N##_BASE, F_GUEST_##N##_LIM, F_GUEST_##N##_AR, &r->n);
  SEGW(es, ES) SEGW(cs, CS) SEGW(ss, SS) SEGW(ds, DS) SEGW(fs, FS) SEGW(gs, GS)
  SEGW(tr, TR) SEGW(ldtr, LDTR)
#undef SEGW
  if (r->gdt_base != o->gdt_base) vmwrite(F_GUEST_GDTR_BASE, r->gdt_base);
  if (r->gdt_limit != o->gdt_limit) vmwrite(F_GUEST_GDTR_LIM, r->gdt_limit);
  if (r->idt_base != o->idt_base) vmwrite(F_GUEST_IDTR_BASE, r->idt_base);
  if (r->idt_limit != o->idt_limit) vmwrite(F_GUEST_IDTR_LIM, r->idt_limit);
}

static void snap_take(struct avm_vm *vm) {
  vm->snap = vm->regs;
  vm->snap_cr0_shadow = vm->cr0_shadow;
  vm->snap_cr4_shadow = vm->cr4_shadow;
}

static int vmx_enable_this_cpu(void);

// Deja el VMCS de `vm` actual en esta CPU (IRQ apagadas). Si ya lo estaba (lo
// dejo la ventana anterior y la tarea no salio de la CPU) no toca nada y se
// conserva el estado de lanzamiento (VMRESUME).
static bool vmcs_make_current(struct avm_vm *vm, int cpu) {
  if (g_loaded_vmcs[cpu] == vm->vmcs_phys)
    return true;
  if (g_loaded_vmcs[cpu]) { // otro VM de esta misma tarea
    vmclear(g_loaded_vmcs[cpu]);
    g_loaded_vmcs[cpu] = 0;
  }
  if (!vm->vmcs_cleared) {
    if (!vmclear(vm->vmcs_phys))
      return false;
    vm->vmcs_cleared = true;
  }
  if (!vmptrld(vm->vmcs_phys))
    return false;
  vm->launched = false; // estaba limpio (VMCLEAR): hay que usar VMLAUNCH
  g_loaded_vmcs[cpu] = vm->vmcs_phys;
  return true;
}

// Llamada desde sched.c justo antes de cambiar de tarea (IRQ apagadas, en la
// CPU que la tarea abandona): guarda en memoria el VMCS que quedo activo.
void vmx_sched_out(void) {
  int cpu = smp_processor_id();
  if (g_loaded_vmcs[cpu]) {
    vmclear(g_loaded_vmcs[cpu]);
    g_loaded_vmcs[cpu] = 0;
  }
}

// Trae a `regs` todo el estado del VMCS (cargandolo un momento en esta CPU).
static int vm_sync_regs(struct avm_vm *vm) {
  if (!vm->regs_stale)
    return 0;
  preempt_disable();
  int rc = vmx_enable_this_cpu();
  if (rc < 0) {
    preempt_enable();
    return rc;
  }
  unsigned long fl = irq_save();
  if (!vmcs_make_current(vm, smp_processor_id())) {
    irq_restore(fl);
    preempt_enable();
    return -EIO;
  }
  vmcs_read_guest(vm);
  snap_take(vm);
  irq_restore(fl);
  preempt_enable();
  vm->regs_stale = false;
  vm->regs_lite_ok = true;
  return 0;
}

// ---------------------------------------------------------------------------
// Inyeccion y emulacion rapida
// ---------------------------------------------------------------------------
// #GP(0). En modo real (CR0.PE=0) VM-entry exige NO entregar codigo de error.
static void inject_gp(struct avm_vm *vm) {
  if (vm->cr0_shadow & 1) {
    vmwrite(F_ENTRY_ERRCODE, 0);
    vmwrite(F_ENTRY_INTR_INFO, 13 | (3u << 8) | (1u << 11) | (1u << 31));
  } else {
    vmwrite(F_ENTRY_INTR_INFO, 13 | (3u << 8) | (1u << 31));
  }
}
static void inject_ud(void) {
  vmwrite(F_ENTRY_INTR_INFO, 6 | (3u << 8) | (1u << 31));
}

static void skip_insn(struct avm_vm *vm) {
  // OJO: el RIP vivo esta en el VMCS; vm->regs.rip solo se actualiza al salir
  // de la ventana (usarlo aqui hacia retroceder al huesped al inicio).
  vm->regs.rip = vmread(F_GUEST_RIP) + vmread(F_EXIT_INSTR_LEN);
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

// ---------------------------------------------------------------------------
// Atajos de dispositivos en el kernel (struct avm_fast)
// ---------------------------------------------------------------------------
static int guest_phys_read(struct avm_vm *vm, uint64_t gpa, void *buf, size_t len);
// ---------------------------------------------------------------------------
// RTC: OUT 0x70 (indice) e IN 0x71 del registro C, el que la interrupcion de
// reloj de Windows lee en cada tick. PF avanza sobre el TSC con la misma rejilla
// que el VMM (rtc_next_tsc/rtc_period_tsc); los periodos sin leer se funden en
// uno como en el chip. true = hecho (el llamador salta la instruccion).
static bool fast_rtc_io(struct avm_vm *vm, struct gregs *g, uint16_t port, uint32_t size,
                        bool in) {
  struct avm_fast *f = &vm->fast;
  if (!(f->flags & AVM_FAST_RTC) || size != 1)
    return false;
  if (port == 0x70 && !in) {
    f->rtc_index = (uint8_t)g->rax;
    f->rtc_dirty |= 1;
    return true;
  }
  if (port == 0x71 && in && (f->rtc_index & 0x7F) == 0x0C) {
    if (f->rtc_period_tsc) {
      uint64_t now = rdtsc_k();
      // Con PIE el cruce de periodo es un flanco de IRQ8 que el IOAPIC/PIC tiene
      // que latchear aunque la lectura borre C enseguida (Windows espera ese tick
      // de reloj mientras sondea C: perderlo daba 0x5C/0x110 en vm17). Eso lo
      // hace el VMM: se le deja esta lectura.
      if (now >= f->rtc_next_tsc && f->rtc_pie)
        return false;
      if (now >= f->rtc_next_tsc) {
        uint64_t n = (now - f->rtc_next_tsc) / f->rtc_period_tsc + 1;
        f->rtc_next_tsc += n * f->rtc_period_tsc;
        f->rtc_c |= 0x40; // sin PIE: solo PF, sin interrupcion
      }
    }
    g->rax = (g->rax & ~0xFFULL) | f->rtc_c;
    f->rtc_c = 0;
    f->rtc_dirty |= 2;
    return true;
  }
  return false;
}

// Lineal -> fisica del huesped en modo largo (4 niveles). -1 si no se puede.
static int guest_lin_to_phys(struct avm_vm *vm, uint64_t lin, uint64_t *gpa) {
  if (vmread(F_GUEST_CR4) & (1ULL << 12)) // LA57: no
    return -1;
  uint64_t t = vmread(F_GUEST_CR3) & 0x000FFFFFFFFFF000ULL, e;
  for (int lvl = 3; lvl >= 0; lvl--) {
    uint64_t idx = (lin >> (12 + 9 * lvl)) & 0x1FF;
    if (guest_phys_read(vm, t + idx * 8, &e, 8) < 0 || !(e & 1))
      return -1;
    if ((lvl == 1 || lvl == 2) && (e & 0x80)) {
      uint64_t sz = 1ULL << (12 + 9 * lvl);
      *gpa = ((e & 0x000FFFFFFFFFF000ULL) & ~(sz - 1)) | (lin & (sz - 1));
      return 0;
    }
    t = e & 0x000FFFFFFFFFF000ULL;
  }
  *gpa = t | (lin & 0xFFF);
  return 0;
}

// EOI del LAPIC (escritura en 0xFEE000B0): se cuenta y el VMM lo aplica en la
// siguiente vuelta (antes de elegir la proxima interrupcion). Solo en modo largo
// y con MOV r/m32, r32 (89) o MOV r/m32, imm32 (C7 /0), que es como lo hacen
// Windows y Linux; cualquier otra cosa sigue yendo al VMM.
static bool fast_eoi(struct avm_vm *vm, uint64_t gpa) {
  // Una por vuelta y solo con la interrupcion inyectada ya entregada: el VMM
  // activa el atajo cuando sabe a que vector ira ese EOI (y que es de flanco).
  if (!(vm->fast.flags & AVM_FAST_EOI) || gpa != 0xFEE000B0ULL || vm->pending_irq >= 0)
    return false;
  if (!(vmread(F_GUEST_EFER) & (1ULL << 10)) || !(vmread(F_GUEST_CS_AR) & (1u << 13)))
    return false;
  uint64_t rip = vmread(F_GUEST_RIP);
  uint8_t b[15] = {0};
  int avail = 0;
  while (avail < 15) { // hasta 15 bytes, como mucho dos paginas
    uint64_t lin = rip + (uint64_t)avail, pa;
    int n = (int)(0x1000 - (lin & 0xFFF));
    if (n > 15 - avail)
      n = 15 - avail;
    if (guest_lin_to_phys(vm, lin, &pa) < 0 || guest_phys_read(vm, pa, &b[avail], (size_t)n) < 0)
      break;
    avail += n;
  }
  if (!avail)
    return false;
  int k = 0;
  if (b[k] == 0x3E || b[k] == 0x2E) // DS/CS (inofensivos)
    k++;
  if ((b[k] & 0xF0) == 0x40) {
    if (b[k] & 8) // REX.W: escritura de 64 bits, no
      return false;
    k++;
  }
  uint8_t op = b[k++];
  if (op != 0x89 && op != 0xC7)
    return false;
  uint8_t modrm = b[k++];
  unsigned mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
  if (mod == 3 || (op == 0xC7 && reg != 0))
    return false;
  if (rm == 4) {
    uint8_t sib = b[k++];
    if (mod == 0 && (sib & 7) == 5)
      k += 4;
  } else if (mod == 0 && rm == 5) {
    k += 4; // relativo a RIP
  }
  k += mod == 1 ? 1 : mod == 2 ? 4 : 0;
  if (op == 0xC7)
    k += 4;
  if (k > avail)
    return false;
  vm->regs.rip = rip + (uint64_t)k;
  vmwrite(F_GUEST_RIP, vm->regs.rip);
  vmwrite(F_GUEST_INTERRUPTIBILITY, vmread(F_GUEST_INTERRUPTIBILITY) & ~3ULL);
  return true;
}

static void emu_cpuid(struct avm_vm *vm, struct gregs *g) {
  uint32_t leaf = (uint32_t)g->rax, sub = (uint32_t)g->rcx;
  uint32_t a, b, c, d;
  cpuid(leaf, sub, &a, &b, &c, &d);
  switch (leaf) {
  case 0:
    // Hoja basica maxima 0xD, como QEMU. Sobre todo sin la 0x15/0x16: con
    // ellas Windows toma la frecuencia del cristal como la del temporizador
    // del LAPIC y la del TSC, que no son las de los dispositivos emulados (el
    // LAPIC virtual va a 1 GHz), y rechaza los temporizadores (0x5C, 0x110).
    if (a > 0xD)
      a = 0xD;
    break;
  case 6:
    a = 1u << 2; // solo ARAT: el temporizador del LAPIC no se para (como QEMU)
    b = c = d = 0;
    break;
  case 1:
    c &= ~((1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) |
           (1u << 7) | (1u << 8) | (1u << 12) | (1u << 15) | (1u << 17) |
           (1u << 21) | (1u << 24) | (1u << 26) | (1u << 27) | (1u << 28) |
           (1u << 29) | (1u << 31));
    // MCE y MCA se anuncian: Windows 10 x64 los exige (sin ellos, pantalla azul
    // 0x5D UNSUPPORTED_PROCESSOR con CPUID.1:EDX de primer parametro). Los
    // bancos se emulan vacios (MCG_CAP y MCi_* en emu_msr).
    d &= ~((1u << 21) | (1u << 22) | (1u << 28) | (1u << 29) | (1u << 31));
    d |= (1u << 7) | (1u << 14);
    if (!(vm->vmflags & AVM_VMF_APIC))
      d &= ~(1u << 9); // APIC local: solo si el VMM lo emula (AVM_SET_FLAGS)
    b = (b & 0x0000FFFFu) | (1u << 16); // sin APIC id, 1 logico
    break;
  case 4:
    a &= 0x3FFF; // un solo hilo/nucleo
    break;
  case 7:
    if (sub == 0) {
      uint32_t invpcid = b & (1u << 10);
      // Sin XSAVE/AVX no puede haber nada con prefijo VEX (BMI1/BMI2 lo son: el
      // huesped las usaria y daria #UD). FSGSBASE tampoco (CR4.FSGSBASE no se deja).
      b &= (1u << 7) | (1u << 9) | (1u << 18) | (1u << 19);
      if (vm->procbased2 & PROC2_INVPCID)
        b |= invpcid; // solo si el host lo tiene
      c = 0;
      d = 0;
    } else {
      a = b = c = d = 0;
    }
    break;
  case 0xA: case 0xB: case 0xD:
    a = b = c = d = 0;
    break;
  case 0x80000001:
    // LAHF/SAHF en 64 bits y PREFETCHW: Windows 8.1+ x64 los exige (si no,
    // UNSUPPORTED_PROCESSOR). Toda CPU Intel de 64 bits ejecuta ambos (PREFETCHW
    // es un NOP en las que no lo anuncian), pero un L1 virtualizado puede ocultarlos.
    c |= (1u << 0) | (1u << 8);
    if (!(vm->procbased2 & PROC2_RDTSCP))
      d &= ~(1u << 27);
    break;
  default:
    if ((leaf > 0xD && leaf < 0x80000000) || (leaf >= 0x40000000 && leaf < 0x50000000))
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
  for (int i = 0; i < g_ngmsr; i++)
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
      // SCE, LME, NXE; LMA (bit 10) es de solo lectura: escribirlo se ignora (no
      // es #GP). Windows lee EFER, le pone NXE y lo vuelve a escribir con LMA=1;
      // con #GP el cargador daba triple fallo justo tras el logo.
      if (val & ~(EFER_SCE | (1ULL << 8) | (1ULL << 10) | EFER_NXE)) {
        ok = false;
        break;
      }
      uint64_t cur = vmread(F_GUEST_EFER);
      // Cambiar LME con la paginacion activa es #GP, como en el hardware.
      if (((val ^ cur) & (1ULL << 8)) && (vm->cr0_shadow & (1ULL << 31))) {
        ok = false;
        break;
      }
      uint64_t keep = cur & (1ULL << 10); // LMA lo decide CR0.PG
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
  case 0x1B:  // APIC_BASE: el VMM emula el LAPIC en 0xFEE00000 (BSP, activado)
    if (!write && (vm->vmflags & AVM_VMF_APIC))
      out = 0xFEE00900ULL;
    break;
  case 0x3A:  // FEATURE_CONTROL: bloqueado, sin VMX/SMX
    out = 1;
    break;
  case 0x35:  // CORE_THREAD_COUNT: 1 nucleo, 1 hilo
    out = 0x00010001;
    break;
  case 0xFE: // MTRRcap: 8 rangos variables, fijos y WC (como QEMU)
    if (write) { ok = false; break; }
    out = 0x508;
    break;
  case 0x2FF: // MTRRdefType
    if (write) vm->mtrr_def = val & 0xCFF; else out = vm->mtrr_def;
    break;
  case 0x200 ... 0x20F: // MTRRphysBase/Mask 0-7
    if (write) vm->mtrr_var[idx - 0x200] = val; else out = vm->mtrr_var[idx - 0x200];
    break;
  case 0x250: case 0x258: case 0x259: case 0x268 ... 0x26F: { // fijos
    int k = idx == 0x250 ? 0 : idx == 0x258 ? 1 : idx == 0x259 ? 2 : 3 + (int)(idx - 0x268);
    if (write) vm->mtrr_fix[k] = val; else out = vm->mtrr_fix[k];
    break;
  }
  case 0x179: // MCG_CAP: 10 bancos, con MCG_CTL (como QEMU)
    if (write) { ok = false; break; }
    out = 10 | (1ULL << 8);
    break;
  case 0x17A: // MCG_STATUS
  case 0x17B: // MCG_CTL
  case 0x400 ... 0x427: // MCi_CTL/STATUS/ADDR/MISC de los 10 bancos: sin errores
    if (!write)
      out = (idx >= 0x400 && (idx & 3) == 0) || idx == 0x17B ? ~0ULL : 0;
    break;
  case 0x280 ... 0x289: // MCi_CTL2
    out = 0;
    break;
  case 0x1A0: // MISC_ENABLE: cadenas rapidas
    out = 1;
    break;
  default:
    // Como KVM con ignore_msrs: lectura 0 y escritura ignorada. Un #GP en un
    // MSR que Windows lee sin proteger es una pantalla azul.
    if (vm->msr_logged < 32) {
      vm->msr_logged++;
      LOG_INFO("[VMX] MSR %x %s sin emular (%s)", idx, write ? "escrito" : "leido",
               write ? "se ignora" : "da 0");
    }
    out = 0;
    break;
  }
done:
  if (ok && !write) {
    g->rax = out & 0xFFFFFFFFULL;
    g->rdx = out >> 32;
  }
  return ok;
}

// Lee memoria fisica del huesped (dentro de una sola pagina). 0 o -1.
static int guest_phys_read(struct avm_vm *vm, uint64_t gpa, void *buf, size_t len) {
  struct slot *s = find_slot(vm, gpa);
  if (!s || !s->node->ops || !s->node->ops->get_page ||
      (gpa & (PAGE_SIZE - 1)) + len > PAGE_SIZE)
    return -1;
  uint64_t pgoff = (s->offset + ((gpa & ~(uint64_t)(PAGE_SIZE - 1)) - s->gpa)) / PAGE_SIZE;
  uint64_t phys = 0;
  if (s->node->ops->get_page(s->node, pgoff, &phys) < 0)
    return -1;
  memcpy(buf, (uint8_t *)phys_to_virt(phys & PTE_FRAME) + (gpa & (PAGE_SIZE - 1)), len);
  return 0;
}

// Paginacion PAE de 32 bits: con EPT, VM-entry carga los PDPTE desde el VMCS (no
// desde CR3). Cuando el kernel emula MOV CR0/CR3 tiene que cargarlos el.
static void load_pdptes(struct avm_vm *vm) {
  bool pae = (vm->cr4_shadow >> 5) & 1, pg = (vm->cr0_shadow >> 31) & 1;
  bool lma = (vm->regs.efer >> 10) & 1;
  if (!pae || !pg || lma)
    return;
  uint64_t pd[4] = {0, 0, 0, 0};
  uint64_t base = vmread(F_GUEST_CR3) & 0xFFFFFFE0ULL;
  if (guest_phys_read(vm, base, pd, sizeof(pd)) < 0)
    memset(pd, 0, sizeof(pd));
  for (int i = 0; i < 4; i++)
    vmwrite(F_GUEST_PDPTE0 + 2 * i, pd[i]);
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

// 0 = emulado (seguir), 1 = emulado y hay que salir al VMM (AVM_EXIT_TPR),
// -1 = no se sabe emular.
static int emu_cr_access(struct avm_vm *vm, struct gregs *g) {
  uint64_t q = vmread(F_EXIT_QUAL);
  int cr = (int)(q & 0xF), type = (int)((q >> 4) & 3), reg = (int)((q >> 8) & 0xF);
  if (type == 2) { // CLTS
    vm->cr0_shadow &= ~(1ULL << 3);
    vm->regs.cr0 = vm->cr0_shadow;
    vmwrite(F_CR0_SHADOW, vm->cr0_shadow);
    vmwrite(F_GUEST_CR0, fixed_cr0(vm, vm->regs.cr0));
    skip_insn(vm);
    return 0;
  }
  if (type == 3) { // LMSW: solo los 4 bits bajos
    uint64_t v = (q >> 16) & 0xF;
    vm->cr0_shadow = (vm->cr0_shadow & ~0xFULL) | v | (vm->cr0_shadow & 1);
    vm->regs.cr0 = vm->cr0_shadow;
    vmwrite(F_CR0_SHADOW, vm->cr0_shadow);
    vmwrite(F_GUEST_CR0, fixed_cr0(vm, vm->regs.cr0));
    skip_insn(vm);
    return 0;
  }
  uint64_t *rp = gpr(g, vm, reg);
  if (reg == 4)
    vm->regs.rsp = vmread(F_GUEST_RSP);
  if (type == 1) { // MOV desde CR
    switch (cr) {
    case 0: *rp = vm->cr0_shadow; break;
    case 3: *rp = vmread(F_GUEST_CR3); break;
    case 4: *rp = vm->cr4_shadow; break;
    case 8: *rp = (vm->tpr >> 4) & 0xF; break;
    default: return -1;
    }
    skip_insn(vm);
    return 0;
  }
  uint64_t v = *rp;
  switch (cr) {
  case 0:
    if ((v >> 32) || (((v >> 31) & 1) && !((v >> 0) & 1))) { inject_gp(vm); return 0; }
    vm->cr0_shadow = v | (1ULL << 4); // ET siempre
    vm->regs.cr0 = vm->cr0_shadow;
    vmwrite(F_CR0_SHADOW, vm->cr0_shadow);
    vmwrite(F_GUEST_CR0, fixed_cr0(vm, vm->regs.cr0));
    update_long_mode(vm);
    load_pdptes(vm);
    break;
  case 3:
    vmwrite(F_GUEST_CR3, v);
    load_pdptes(vm);
    break;
  case 4:
    if (v & ~CR4_ALLOWED) { inject_gp(vm); return 0; }
    vm->cr4_shadow = v;
    vm->regs.cr4 = v;
    vmwrite(F_CR4_SHADOW, v);
    vmwrite(F_GUEST_CR4, fixed_cr4(v));
    break;
  case 8: {
    if (v & ~0xFULL) { inject_gp(vm); return 0; }
    uint32_t old = vm->tpr;
    vm->tpr = (uint32_t)v << 4;
    skip_insn(vm);
    // Subir: la interrupcion del LAPIC aun sin entregar puede no caber ya.
    if (vm->tpr > old && vm->pending_irq >= 0 && (vm->pending_irq & AVM_IRQ_TPR) &&
        ((uint32_t)vm->pending_irq & 0xF0) <= vm->tpr)
      return 1;
    // Bajar por debajo de lo que el VMM tiene bloqueado: que la entregue.
    if (vm->tpr < old && vm->tpr_wait && vm->tpr_wait > (vm->tpr >> 4))
      return 1;
    return 0;
  }
  default:
    return -1;
  }
  skip_insn(vm);
  return 0;
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
static int run_window(struct avm_vm *vm, struct avm_exit *x, uint64_t deadline_tick) {
  uint64_t t_win0 = rdtsc_k();
  uint64_t win_tick0 = tick_count;
  bool first_entry = true;
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
  // El VMCS sigue activo en esta CPU si la tarea no salio de ella desde la
  // ventana anterior; si no, VMPTRLD. Su contenido se conserva siempre, asi que
  // solo se escribe lo que cambio.
  int cpu = smp_processor_id();
  if (!vmcs_make_current(vm, cpu)) {
    irq_restore(fl);
    preempt_enable();
    fail_exit(x, 0, 0xFFFF, 0);
    return 1;
  }
  if (vm->ept_dirty || cpu != vm->last_cpu) {
    invept_ctx(vm_eptp(vm));
    vm->ept_dirty = false;
    vm->last_cpu = cpu;
  }
  if (vm->vmcs_full) {
    if (vm->regs_stale) { // no perder lo que solo esta en el VMCS
      vmcs_read_guest(vm);
      vm->regs_stale = false;
    }
    vmcs_write_controls_and_host(vm);
    vmcs_write_guest(vm);
    snap_take(vm);
    vm->ctl_cpu = cpu;
    vm->ctl_debug = vm->debug;
    vm->vmcs_full = false;
  } else {
    if (vm->ctl_cpu != cpu) { // estado del host de otra CPU (GDT, TSS, GS...)
      vmcs_write_controls_and_host(vm);
      vm->ctl_cpu = cpu;
      vm->ctl_debug = vm->debug;
    } else {
      // Solo lo que puede cambiar entre ventanas en la misma CPU y la misma
      // tarea (cada VMWRITE de un campo del host es una salida con KVM anidado).
      uint64_t c0 = read_cr0(), c3 = read_cr3();
      if (c0 != vm->host_cr0_c) { // TS puede cambiar (FPU perezosa)
        vmwrite(F_HOST_CR0, c0);
        vm->host_cr0_c = c0;
      }
      if (c3 != vm->host_cr3_c) {
        vmwrite(F_HOST_CR3, c3);
        vm->host_cr3_c = c3;
      }
      vmwrite(F_HOST_FS_BASE, rdmsr(MSR_FS_BASE));
      // STAR/LSTAR/CSTAR/SFMASK y TSC_AUX son fijos en cada CPU (se leyeron al
      // escribir el estado del host para esta CPU); KERNEL_GS_BASE es de la tarea.
      vm->hmsr[4].value = rdmsr(MSR_KERNEL_GS_BASE);
      if (vm->ctl_debug != vm->debug) {
        vmwrite(F_EXC_BITMAP, (1u << 3) | (vm->debug ? ((1u << 6) | (1u << 8) | (1u << 13) | (1u << 14)) : 0));
        vm->ctl_debug = vm->debug;
      }
    }
    vmcs_write_guest_diff(vm);
    snap_take(vm);
    vmwrite(F_ENTRY_INTR_INFO, 0);
    vmwrite(F_GUEST_PENDING_DBG, 0);
  }
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
  // Tras un fallo de entrada el estado de lanzamiento del VMCS es dudoso: VMCLEAR
  // al salir de la ventana y la siguiente empieza con VMPTRLD + VMLAUNCH.
  bool clear_at_leave = false;
  unsigned inner_exits = 0;

  for (;;) {
    // Nunca quedarse en la ventana (IRQ del host apagadas) mas de unas salidas
    // rapidas seguidas: un huesped que falla en bucle colgaria la CPU. No se
    // sale con un evento ya preparado para inyectar (se perderia).
    // Un evento ya preparado (p. ej. una excepcion que inyecto el manejador de la
    // ultima salida) se guarda en reinj al salir de la ventana (ver `leave`): asi
    // la ventana termina siempre. Antes un huesped que provocaba en bucle salidas
    // con inyeccion dejaba esta CPU con las IRQ apagadas para siempre (la VM no
    // volvia al VMM: ventana congelada y sin poder cerrarla).
    if (++inner_exits > 256 && (!(vmread(F_ENTRY_INTR_INFO) & (1u << 31)) || !vm->reinj)) {
      if ((vmread(F_ENTRY_INTR_INFO) & (1u << 31)) && vm->stuck_logged < 4) {
        vm->stuck_logged++;
        LOG_WARN("[VMX] 256 salidas seguidas inyectando eventos: info=%lx ultima salida %u "
                 "rip=%lx cs=%lx cr0=%lx rflags=%lx", vmread(F_ENTRY_INTR_INFO),
                 (unsigned)vmread(F_EXIT_REASON), vmread(F_GUEST_RIP), vmread(F_GUEST_CS_SEL),
                 vmread(F_GUEST_CR0), vmread(F_GUEST_RFLAGS));
      }
      ret = 0;
      goto leave;
    }
    uint32_t proc = vm->procbased;
    // Primero el evento que una salida interrumpio a medio entregar.
    if (vm->reinj && !(vmread(F_ENTRY_INTR_INFO) & (1u << 31))) {
      uint32_t t = (vm->reinj_info >> 8) & 7;
      vmwrite(F_ENTRY_INTR_INFO, vm->reinj_info & ~(1u << 12));
      if (vm->reinj_info & (1u << 11))
        vmwrite(F_ENTRY_ERRCODE, vm->reinj_err);
      if (t == 4 || t == 5 || t == 6) // INT n / INT3 / INTO: longitud de la instruccion
        vmwrite(F_ENTRY_INSTR_LEN, vm->reinj_len);
      vm->reinj = false;
    }
    // Interrupcion pendiente del VMM: inyectar si el huesped puede recibirla.
    if (vm->pending_irq >= 0) {
      uint64_t rfl = vmread(F_GUEST_RFLAGS);
      uint64_t intr = vmread(F_GUEST_INTERRUPTIBILITY);
      uint64_t info = vmread(F_ENTRY_INTR_INFO);
      bool tpr_ok = !(vm->pending_irq & AVM_IRQ_TPR) ||
                    ((uint32_t)vm->pending_irq & 0xF0) > (vm->tpr & 0xF0);
      if (!tpr_ok) {
        // Bloqueada por CR8: no se pide ventana (saldria en bucle); al bajar
        // CR8 hay salida y se reevalua aqui.
      } else if ((rfl & (1u << 9)) && !(intr & 3) && !(info & (1u << 31))) {
        vmwrite(F_ENTRY_INTR_INFO, ((uint32_t)vm->pending_irq & 0xFF) | (1u << 31));
        vm->pending_irq = -1;
      } else {
        proc |= PROC_INTR_WINDOW;
      }
    }
    (void)need_intr_window;
    vmwrite(F_PROC, proc);

    __asm__ volatile("mov %0, %%cr2" : : "r"(vm->regs.cr2) : "memory");
    uint64_t t_e0 = rdtsc_k();
    if (first_entry) {
      first_entry = false;
      vm->st.windows++;
      vm->st.tsc_setup += t_e0 - t_win0;
    }
    int fail = vmx_enter(&g, vm->launched);
    vm->st.tsc_guest += rdtsc_k() - t_e0;
    vm->st.exits++;
    __asm__ volatile("mov %%cr2, %0" : "=r"(vm->regs.cr2));
    if (fail) {
      uint64_t err = vmread(F_VM_INSTR_ERROR);
      vm->vmcs_full = true; // reescribir todo en la siguiente ventana
      clear_at_leave = true;
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
      vm->vmcs_full = true; // reescribir todo en la siguiente ventana
      clear_at_leave = true;
      LOG_ERR("[VMX] VM-entry invalida: motivo %u qual=%lx (estado del huesped "
              "no valido o fallo al cargar MSR)", reason, qual);
      fail_exit(x, reason_full, reason, qual);
      ret = 1;
      break;
    }
    x->hw_reason = reason;

    // Evento que se estaba entregando cuando salimos (interrupcion inyectada,
    // excepcion, INT n): hay que volver a inyectarlo o se pierde. Las salidas
    // por excepcion (solo con depuracion) reinyectan la suya y no este.
    uint64_t idtv = vmread(F_IDTV_INFO);
    if ((idtv & (1u << 31)) && reason != XR_EXC_NMI) {
      vm->reinj = true;
      vm->reinj_info = (uint32_t)idtv;
      vm->reinj_err = (idtv & (1u << 11)) ? (uint32_t)vmread(F_IDTV_ERR) : 0;
      vm->reinj_len = (uint32_t)vmread(F_EXIT_INSTR_LEN);
    }

    switch (reason) {
    case XR_EXT_INTR: {
      // La IRQ del host se sirve aqui mismo (IF un instante) y se vuelve a
      // entrar sin rehacer la ventana: rehacerla (VMCLEAR, VMPTRLD y todo el
      // estado) cuesta ~0,5 ms con KVM anidado y habia una por cada IRQ del host.
      // Se sale para ceder la CPU, por una senal, por el plazo del VMM o cada
      // 2 ticks como mucho.
      vm->st.ext_exits++;
      irq_restore(fl);
      fl = irq_save();
      task_t *cur = sched_current();
      if (tick_count >= deadline_tick || tick_count - win_tick0 >= 2 ||
          (cur && (cur->need_resched || signal_unblocked_of(cur)))) {
        ret = 0;
        goto leave;
      }
      continue;
    }
    case XR_INTR_WINDOW:
      continue;
    case XR_EXC_NMI: {
      uint32_t info = (uint32_t)vmread(F_EXIT_INTR_INFO);
      if (((info >> 8) & 7) == 2) {
        __asm__ volatile("int $2"); // NMI del host que salto en el huesped
        continue;
      }
      if (!(info & (1u << 31)))
        continue;
      uint32_t vec = info & 0xFF;
      // INT3 en anillo 0 sin depurador: se hace lo que haria un depurador
      // conectado ("g"), seguir tras la instruccion. Windows 10 x64 ejecuta un
      // DbgBreakPoint al arrancar en esta VM y, sin depurador, eso es la
      // pantalla azul 0x1E (80000003). Los INT3 de anillo 3 van al huesped.
      if (vec == 3 && ((info >> 8) & 7) == 6 && (vmread(F_GUEST_CR0) & 1) &&
          !(vmread(F_GUEST_SS_AR) & 0x60)) { // modo protegido/largo y CPL 0
        uint64_t rip_bp = vmread(F_GUEST_RIP);
        vm->st.bp_skipped++;
        vm->st.bp_rip = rip_bp;
        if (vm->st.bp_skipped <= 8)
          LOG_WARN("[VMX] INT3 del huesped en anillo 0 rip=%lx: se salta (sin depurador)", rip_bp);
        skip_insn(vm);
        continue;
      }
      uint64_t err = (info & (1u << 11)) ? vmread(0x4406) : 0;
      uint64_t q = vmread(F_EXIT_QUAL);
      if (vm->exc_logged < 16) {
        vm->exc_logged++;
        LOG_WARN("[VMX] excepcion del huesped #%u err=%lx qual=%lx rip=%lx rsp=%lx "
                 "cs=%lx cr0=%lx cr4=%lx efer=%lx cr3=%lx",
                 vec, err, q, vmread(F_GUEST_RIP), vmread(F_GUEST_RSP),
                 vmread(F_GUEST_CS_SEL), vmread(F_GUEST_CR0),
                 vmread(F_GUEST_CR4), vmread(F_GUEST_EFER), vmread(F_GUEST_CR3));
      }
      {
        // Atasco = la MISMA excepcion en el mismo sitio y con la misma direccion
        // (un REP MOVS que recorre paginas ausentes falla en el mismo RIP con
        // CR2 distinto: eso es normal).
        uint64_t rip_now = vmread(F_GUEST_RIP);
        if (rip_now == vm->last_exc_rip && vec == vm->last_exc_vec && q == vm->last_exc_qual) {
          if (++vm->exc_repeat > 64) { // el huesped se ha atascado fallando
            LOG_WARN("[VMX] excepcion #%u repetida en rip=%lx: se da por apagado",
                     vec, rip_now);
            vm->reinj = false; // el VMM reinicia la CPU: no reinyectar nada viejo
            vmwrite(F_ENTRY_INTR_INFO, 0);
            x->reason = AVM_EXIT_SHUTDOWN;
            ret = 1;
            goto leave;
          }
        } else {
          vm->last_exc_rip = rip_now;
          vm->last_exc_vec = vec;
          vm->last_exc_qual = q;
          vm->exc_repeat = 0;
        }
      }
      if (vec == 14)
        vm->regs.cr2 = q;
      vmwrite(F_ENTRY_INTR_INFO, info & ~(1u << 12)); // reinyectar tal cual
      if (info & (1u << 11))
        vmwrite(F_ENTRY_ERRCODE, err);
      continue;
    }
    case XR_CPUID:
      vm->regs.rsp = vmread(F_GUEST_RSP);
      emu_cpuid(vm, &g);
      skip_insn(vm);
      continue;
    case XR_VMX_FIRST ... XR_VMX_LAST: // VMCALL, VMCLEAR, ... VMXON
    case XR_INVEPT:
    case XR_INVVPID:
    case XR_VMFUNC:
      inject_ud();
      continue;
    case XR_INVLPG: // sin VPID cada entrada/salida vacia la TLB del huesped
    case XR_WBINVD:
      skip_insn(vm);
      continue;
    case XR_RDPMC:
      inject_gp(vm);
      continue;
    case XR_INIT:
    case XR_SIPI:
      continue;
    case XR_RDMSR:
    case XR_WRMSR:
      if (emu_msr(vm, &g, reason == XR_WRMSR))
        skip_insn(vm);
      else
        inject_gp(vm);
      continue;
    case XR_CR_ACCESS: {
      int cr_rc = emu_cr_access(vm, &g);
      if (cr_rc == 0)
        continue;
      if (cr_rc == 1) {
        x->reason = AVM_EXIT_TPR;
        ret = 1;
        goto leave;
      }
      fail_exit(x, reason, 0xC0, vmread(F_EXIT_QUAL));
      ret = 1;
      goto leave;
    }
    case XR_EPT_VIOLATION: {
      uint64_t gpa = vmread(F_GUEST_PHYS_ADDR);
      uint64_t qual = vmread(F_EXIT_QUAL);
      int r2 = ept_fault(vm, gpa);
      if (r2 == 0) {
        vm->st.ept_maps++;
        continue;
      } // el evento interrumpido (si lo habia) lo reinyecta la cabecera del bucle
      if (r2 < 0) {
        fail_exit(x, reason, (uint64_t)(-r2), gpa);
        ret = 1;
        goto leave;
      }
      if (((qual >> 1) & 1) && fast_eoi(vm, gpa)) {
        vm->fast.eoi_count++;
        vm->fast.flags &= ~AVM_FAST_EOI;
        continue;
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
      if (string) { // INS/OUTS: el VMM mueve los datos (F3b); aqui solo se describe
        // Tamano de direccion y segmento: de la informacion de instruccion si la
        // CPU la da (VMX_BASIC bit 54); si no, el tamano por omision del codigo y
        // DS para OUTS (sin mirar prefijos de segmento, rarisimos con OUTS).
        bool lm64 = (vmread(F_GUEST_EFER) & (1ULL << 10)) && (vmread(F_GUEST_CS_AR) & (1u << 13));
        uint64_t info = vmread(F_EXIT_INSTR_INFO);
        unsigned asz, seg;
        if ((g_vmx.basic >> 54) & 1) {
          asz = (unsigned)(info >> 7) & 7; // 0=16, 1=32, 2=64 bits
          seg = (unsigned)(info >> 15) & 7;
        } else {
          asz = lm64 ? 2 : (vmread(F_GUEST_CS_AR) & (1u << 14)) ? 1 : 0;
          seg = 3;
        }
        if (in || seg > 5)
          seg = 0; // INS escribe siempre en ES:(E)DI
        uint64_t amask = asz == 0 ? 0xFFFFULL : asz == 1 ? 0xFFFFFFFFULL : ~0ULL;
        bool rep = (q >> 5) & 1;
        uint64_t cnt = rep ? (g.rcx & amask) : 1;
        uint64_t slen = vmread(F_EXIT_INSTR_LEN);
        vm->regs.rip = vmread(F_GUEST_RIP) + slen; // se completa ya; el VMM ajusta RSI/RDI/RCX
        vmwrite(F_GUEST_RIP, vm->regs.rip);
        // REP con RCX=0 tambien sale: el VMM recalcula la cuenta decodificando los
        // prefijos (asz puede venir mal con KVM anidado) y no hace nada si es 0.
        x->reason = AVM_EXIT_IO;
        x->io.port = (uint16_t)(q >> 16);
        x->io.size = (uint8_t)size;
        x->io.is_write = !in;
        x->io.count = cnt > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)cnt;
        x->io.data = 0;
        x->io.string = 1;
        x->io.asize = (uint8_t)asz;
        x->io.rep = rep;
        x->io.ilen = (uint8_t)slen;
        // Lineal = base del segmento + (E)SI/(E)DI. No se usa el campo "guest-linear
        // address" de la salida: con KVM anidado puede llegar a 0, y entonces los
        // datos de un REP INSW acababan en la IVT (disco IDE de 0 MiB y triple fallo).
        static const uint32_t seg_base[6] = {F_GUEST_ES_BASE, F_GUEST_CS_BASE, F_GUEST_SS_BASE,
                                             F_GUEST_DS_BASE, F_GUEST_FS_BASE, F_GUEST_GS_BASE};
        uint64_t sbase = (lm64 && seg < 4) ? 0 : vmread(seg_base[seg]);
        uint64_t lin = sbase + ((in ? g.rdi : g.rsi) & amask);
        x->io.linear = lm64 ? lin : (lin & 0xFFFFFFFFULL);
        ret = 1;
        goto leave;
      }
      if (fast_rtc_io(vm, &g, (uint16_t)(q >> 16), size, in)) {
        skip_insn(vm);
        continue;
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
    case XR_INVD: // sin invalidar cachés: el huésped no puede tocar las del host
    case XR_MWAIT:
    case XR_MONITOR:
      skip_insn(vm);
      continue;
    case XR_GETSEC:
    case XR_RSM:
      inject_ud();
      continue;
    case XR_XSETBV: // XSAVE no se anuncia por CPUID
      inject_gp(vm);
      continue;
    case XR_MOV_DR: {
      uint64_t q = vmread(F_EXIT_QUAL);
      int dr = (int)(q & 7), r = (int)((q >> 8) & 0xF);
      uint64_t *rp = gpr(&g, vm, r);
      if (r == 4)
        vm->regs.rsp = vmread(F_GUEST_RSP);
      if (q & 0x10) {
        *rp = dr == 6 ? (vm->dr[6] | 0xFFFF0FF0ULL)
              : dr == 7 ? 0x400ULL | vm->dr[7] : vm->dr[dr];
      } else {
        vm->dr[dr] = *rp; // solo se recuerda: los DR del host no se tocan
      }
      skip_insn(vm);
      continue;
    }
    case XR_HLT:
      skip_insn(vm);
      x->reason = AVM_EXIT_HLT;
      ret = 1;
      goto leave;
    case XR_TRIPLE_FAULT:
      LOG_WARN("[VMX] triple fallo: rip=%lx rsp=%lx cs=%lx cr0=%lx cr4=%lx efer=%lx "
               "cr3=%lx idtr=%lx/%lx", vmread(F_GUEST_RIP), vmread(F_GUEST_RSP),
               vmread(F_GUEST_CS_SEL), vmread(F_GUEST_CR0), vmread(F_GUEST_CR4),
               vmread(F_GUEST_EFER), vmread(F_GUEST_CR3),
               vmread(F_GUEST_IDTR_BASE), vmread(F_GUEST_IDTR_LIM));
      vm->reinj = false; // el VMM reinicia la CPU: no reinyectar nada viejo
      vmwrite(F_ENTRY_INTR_INFO, 0);
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

leave:;
  uint64_t t_lv0 = rdtsc_k();
  // Evento preparado para la siguiente entrada que no llego a entrar: el VMCS se
  // reescribe en la proxima ventana (ENTRY_INTR_INFO = 0), asi que se guarda.
  {
    uint64_t ei = vmread(F_ENTRY_INTR_INFO);
    if ((ei & (1u << 31)) && !vm->reinj) {
      vm->reinj = true;
      vm->reinj_info = (uint32_t)ei;
      vm->reinj_err = (ei & (1u << 11)) ? (uint32_t)vmread(F_ENTRY_ERRCODE) : 0;
      vm->reinj_len = (uint32_t)vmread(F_ENTRY_INSTR_LEN);
    }
  }
  // Estado del huesped de vuelta a software.
  vm->regs.rax = g.rax; vm->regs.rbx = g.rbx; vm->regs.rcx = g.rcx;
  vm->regs.rdx = g.rdx; vm->regs.rsi = g.rsi; vm->regs.rdi = g.rdi;
  vm->regs.rbp = g.rbp; vm->regs.r8 = g.r8; vm->regs.r9 = g.r9;
  vm->regs.r10 = g.r10; vm->regs.r11 = g.r11; vm->regs.r12 = g.r12;
  vm->regs.r13 = g.r13; vm->regs.r14 = g.r14; vm->regs.r15 = g.r15;
  // Salidas tras las que el VMM siempre pide los registros (MMIO, INS/OUTS, HLT,
  // apagado): se leen ya, con el VMCS cargado. En las demas (E/S simple, plazo,
  // TPR) basta lo minimo y el resto se lee si se pide.
  if (ret == 1 && (x->reason == AVM_EXIT_SHUTDOWN || x->reason == AVM_EXIT_FAIL)) {
    vmcs_read_guest(vm);
    snap_take(vm);
    vm->regs_stale = false;
    vm->regs_lite_ok = true;
  } else if (ret == 1 && (x->reason == AVM_EXIT_MMIO || x->reason == AVM_EXIT_HLT ||
                          (x->reason == AVM_EXIT_IO && x->io.string))) {
    vmcs_read_hot(vm);
    vmcs_read_lite(vm);
    vm->regs_stale = true;
    vm->regs_lite_ok = true;
  } else {
    vmcs_read_hot(vm);
    vm->regs_stale = true;
    vm->regs_lite_ok = false;
  }
  vmwrite(F_ENTRY_INTR_INFO, 0); // lo pendiente ya esta en reinj
  __asm__ volatile("fxsave %0" : "=m"(*(uint8_t(*)[512])(fx + 512)) : : "memory");
  __asm__ volatile("fxrstor %0" : : "m"(*(uint8_t(*)[512])fx) : "memory");
  // Sin VMCLEAR: el VMCS queda activo en esta CPU (vmx_sched_out lo limpia si
  // la tarea sale de ella).
  if (clear_at_leave) {
    vmclear(vm->vmcs_phys);
    g_loaded_vmcs[cpu] = 0;
  }
  vm->st.tsc_leave += rdtsc_k() - t_lv0;
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
  if (!vm->ept_pml4) // ept_reset sin memoria
    return -ENOMEM;
  if (__atomic_exchange_n(&vm->busy, 1, __ATOMIC_ACQUIRE))
    return -EBUSY;
  int64_t rc = 0;
  memset(&x, 0, sizeof(x));
  uint64_t t_start = tick_count;
  uint64_t tsc_run0 = rdtsc_k();
  uint64_t last_pp = tick_count;
  vm->st.runs++;

  {
    struct avm_exit prev;
    if (copy_from_user(&prev, (void *)uarg, sizeof(prev)) < 0) {
      rc = -EFAULT;
      goto out;
    }
    vm->fast = prev.fast;
    vm->fast.eoi_count = 0;
    vm->fast.rtc_dirty = 0;
    if (vm->pending_in) { // el VMM rellena io.data del IN anterior
      uint32_t mask = vm->pending_in_size == 4 ? 0xFFFFFFFFu
                                               : (1u << (8 * vm->pending_in_size)) - 1;
      if (vm->pending_in_size == 4)
        vm->regs.rax = prev.io.data; // IN de 32 bits: extiende a cero
      else
        vm->regs.rax = (vm->regs.rax & ~(uint64_t)mask) | (prev.io.data & mask);
      vm->regs.rip += vm->pending_in_len;
      vm->pending_in = false;
    }
  }

  for (;;) {
    task_t *cur = sched_current();
    if ((cur && signal_unblocked_of(cur)) ||
        (vm->timeout_ms && tick_count - t_start >= vm->timeout_ms)) {
      x.reason = AVM_EXIT_SIGNAL;
      break;
    }
    memset(&x, 0, sizeof(x));
    int r = run_window(vm, &x, vm->timeout_ms ? t_start + vm->timeout_ms : ~0ULL);
    if (r < 0) {
      rc = r;
      goto out;
    }
    if (r == 1)
      break;
    // Ventana cortada (IRQ del host, plazo, cada 2 ticks): ceder solo si hay
    // otra tarea lista. sched_yield saltaba al idle aunque no la hubiera y la
    // VM perdia un tick entero cada vez (~2 ms por salida con ReactOS).
    if (tick_count - last_pp >= 10 || (cur && cur->need_resched)) {
      last_pp = tick_count;
      sched_preempt_point();
    }
  }
  x.irq_pending = vm->pending_irq;
  x.tpr = vm->tpr;
  x.fast = vm->fast;
  if (copy_to_user((void *)uarg, &x, sizeof(x)) < 0)
    rc = -EFAULT;
out:
  vm->st.tsc_run += rdtsc_k() - tsc_run0;
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

// Tira el EPT entero (se rellena por demanda). Las CPU con traducciones viejas
// las invalidan al empezar la siguiente ventana (ept_dirty / cambio de CPU).
static int ept_reset(struct avm_vm *vm) {
  ept_free_all(vm);
  vm->ept_pml4 = ept_new_table(vm);
  vm->ept_dirty = true;
  return vm->ept_pml4 ? 0 : -ENOMEM;
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
  for (int i = 0; i < g_ngmsr; i++) {
    vm->gmsr[i].index = guest_msr_idx[i];
    vm->hmsr[i].index = guest_msr_idx[i];
  }
  // fxsave del huesped inicial: estado FPU por defecto (FCW=0x37F, MXCSR=0x1F80)
  uint8_t *fx = (uint8_t *)phys_to_virt(fxp) + 512;
  *(uint16_t *)fx = 0x37F;
  *(uint32_t *)(fx + 24) = 0x1F80;
  vm->last_cpu = -1;
  vm->ept_dirty = true;
  vm->ept_pml4 = ept_new_table(vm);
  if (!vm->ept_pml4) {
    pmm_free_page(vmcs); pmm_free_page(msrp); pmm_free_page(fxp);
    kfree(vm);
    return -ENOMEM;
  }
  vm->magic = VM_MAGIC;
  vmcs_setup_static(vm);
  vm_reset_regs(vm);
  vm->vmcs_full = true;
  vm->ctl_cpu = -1;
  node->priv = vm;
  return 0;
}

static int avm_close(vfs_node_t *node) {
  struct avm_vm *vm = node_vm(node);
  if (!vm)
    return 0;
  node->priv = NULL;
  // El VMCS puede seguir activo en esta CPU (lo dejo la ultima ventana): VMCLEAR
  // antes de liberar la pagina. Si estuviera activo en otra CPU (otro hilo del
  // proceso corriendo alli) no se libera: un VMCLEAR posterior escribiria en ella.
  bool vmcs_busy = false;
  preempt_disable();
  {
    unsigned long fl = irq_save();
    int cpu = smp_processor_id();
    for (int c = 0; c < MAX_CPUS; c++) {
      if (g_loaded_vmcs[c] != vm->vmcs_phys)
        continue;
      if (c == cpu) {
        vmclear(vm->vmcs_phys);
        g_loaded_vmcs[c] = 0;
      } else {
        vmcs_busy = true;
      }
    }
    irq_restore(fl);
  }
  preempt_enable();
  for (int i = 0; i < AVM_MAX_SLOTS; i++)
    slot_release(&vm->slots[i]);
  ept_free_all(vm);
  if (vmcs_busy)
    LOG_WARN("[VMX] VMCS activo en otra CPU al cerrar: se deja sin liberar");
  else
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
        m.gpa + m.size < m.gpa || m.gpa + m.size > (1ULL << 40))
      return -EINVAL;
    if (vm->busy)
      return -EBUSY;
    if (!m.size) { // tamano 0: quitar el slot
      struct slot *s0 = &vm->slots[m.slot];
      if (!s0->used)
        return 0;
      slot_release(s0);
      return ept_reset(vm);
    }
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
    if (replace) // las paginas ya mapeadas del slot viejo ya no valen
      return ept_reset(vm);
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
    int src = vm_sync_regs(vm); // snap = VMCS: la siguiente ventana escribe la diferencia
    if (src < 0)
      vm->vmcs_full = true;
    vm->regs = r;
    vm->cr0_shadow = r.cr0;
    vm->cr4_shadow = r.cr4;
    vm->regs_stale = false;
    return 0;
  }
  case AVM_GET_REGS_LITE: { // ver struct avm_vm.regs_lite_ok
    if (!access_ok((void *)arg, sizeof(vm->regs)))
      return -EFAULT;
    if (vm->busy)
      return -EBUSY;
    if (vm->regs_stale && !vm->regs_lite_ok) {
      int src = vm_sync_regs(vm);
      if (src < 0)
        return src;
    }
    return copy_to_user((void *)arg, &vm->regs, sizeof(vm->regs)) < 0 ? -EFAULT : 0;
  }
  case AVM_SET_REGS_LITE: { // solo registros generales, RIP y RFLAGS
    if (vm->busy)
      return -EBUSY;
    if (!access_ok((void *)arg, sizeof(vm->regs)))
      return -EFAULT;
    struct avm_regs r;
    if (copy_from_user(&r, (void *)arg, sizeof(r)) < 0)
      return -EFAULT;
    struct avm_regs *d = &vm->regs;
    d->rax = r.rax; d->rbx = r.rbx; d->rcx = r.rcx; d->rdx = r.rdx;
    d->rsi = r.rsi; d->rdi = r.rdi; d->rbp = r.rbp; d->rsp = r.rsp;
    d->r8 = r.r8; d->r9 = r.r9; d->r10 = r.r10; d->r11 = r.r11;
    d->r12 = r.r12; d->r13 = r.r13; d->r14 = r.r14; d->r15 = r.r15;
    d->rip = r.rip;
    d->rflags = r.rflags;
    return 0;
  }
  case AVM_GET_REGS: {
    if (!access_ok((void *)arg, sizeof(vm->regs)))
      return -EFAULT;
    if (vm->busy)
      return -EBUSY;
    int src = vm_sync_regs(vm);
    if (src < 0)
      return src;
  }
    return copy_to_user((void *)arg, &vm->regs, sizeof(vm->regs)) < 0 ? -EFAULT : 0;
  case AVM_RUN:
    return vm_run(vm, arg);
  case AVM_INJECT_IRQ:
    if ((arg & 0xFF) < 16 && (arg & AVM_IRQ_TPR)) // vectores 0-15 no son del LAPIC
      return -EINVAL;
    if (arg & ~(0xFFUL | AVM_IRQ_TPR))
      return -EINVAL;
    if (vm->pending_irq >= 0)
      return -EBUSY;
    vm->pending_irq = (int32_t)arg;
    return 0;
  case AVM_WITHDRAW_IRQ: {
    if (vm->busy)
      return -EBUSY;
    int32_t p = vm->pending_irq;
    vm->pending_irq = -1;
    return p;
  }
  case AVM_SET_TPR:
    vm->tpr = (uint32_t)arg & 0xFF;
    return 0;
  case AVM_SET_TPR_WAIT:
    vm->tpr_wait = (uint32_t)arg & 0xF;
    return 0;
  case AVM_SET_DEBUG:
    vm->debug = (uint32_t)arg & 1;
    vm->exc_logged = 0;
    return 0;
  case AVM_SET_TIMEOUT:
    vm->timeout_ms = (uint32_t)arg;
    return 0;
  case AVM_SET_FLAGS:
    vm->vmflags = (uint32_t)arg & AVM_VMF_ALL;
    return 0;
  case AVM_GET_STATS:
    if (!access_ok((void *)arg, sizeof(vm->st)))
      return -EFAULT;
    return copy_to_user((void *)arg, &vm->st, sizeof(vm->st)) < 0 ? -EFAULT : 0;
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
  if (g_vmx.caps.flags & AVM_CAP_VMX) {
    g_c0f0 = rdmsr(MSR_VMX_CR0_FIXED0);
    g_c0f1 = rdmsr(MSR_VMX_CR0_FIXED1);
    g_c4f0 = rdmsr(MSR_VMX_CR4_FIXED0);
    g_c4f1 = rdmsr(MSR_VMX_CR4_FIXED1);
  }
  g_vmx.probed = true;
  {
    uint32_t a, b, c, d;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    g_ngmsr = 5;
    if (a >= 0x80000001) {
      cpuid(0x80000001, 0, &a, &b, &c, &d);
      if (d & (1u << 27))
        g_ngmsr = 6; // RDTSCP: TSC_AUX del huesped
    }
  }
  LOG_INFO("[VMX] caps=%x ept_vpid=%lx usable=%d", g_vmx.caps.flags,
           g_vmx.caps.ept_vpid_cap, (int)g_vmx.usable);
  devtmpfs_register("aurora-vm", DEVT_MKDEV(10, 233), 0660, 0, 0, &avm_ops, NULL);
}
