// kernel/panic.c
#include "panic.h"
#include "cpu.h"
#include "gdt.h"
#include "heap.h"
#include "klog.h"
#include "paging.h"
#include "sched.h"
#include "serial.h"
#include "slab.h"
#include <stdarg.h>

extern uint8_t __text_start;
extern uint8_t __text_end;

static int in_panic = 0;

static int is_kernel_text(uint64_t addr) {
  uint64_t start = (uint64_t)&__text_start;
  uint64_t end = (uint64_t)&__text_end;
  return addr >= start && addr < end;
}

void backtrace(uint64_t rbp, uint64_t rip, int max_frames) {
  serial_puts("\n[BT] Stack trace:");
  serial_puts("\n[BT]   #0 ");
  serial_hex(rip);

  uint64_t frame_rbp = rbp;
  int frame = 1;

  while (frame < max_frames) {
    if (frame_rbp == 0 || (frame_rbp & 0x7) != 0)
      break;

    uint64_t *f = (uint64_t *)frame_rbp;
    uint64_t next_rbp = f[0];
    uint64_t ret_rip = f[1];

    if (!is_kernel_text(ret_rip))
      break;

    serial_puts("\n[BT]   #");
    serial_putn(frame, 10, 0);
    serial_puts(" ");
    serial_hex(ret_rip);

    frame_rbp = next_rbp;
    frame++;
  }

  if (frame >= max_frames) {
    serial_puts("\n[BT]   ... (más de ");
    serial_putn(max_frames, 10, 0);
    serial_puts(" frames)");
  }
  serial_puts("\n[BT] Fin del backtrace.\n");
}

void dump_registers(registers_t *regs) {
  uint64_t cr0, cr2, cr3, cr4;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

  if (regs) {
    serial_puts("\n--- CPU state ---");
    serial_puts("\nRIP: ");
    serial_hex(regs->rip);
    serial_puts("  CS: ");
    serial_hex(regs->cs);
    serial_puts("  RFLAGS: ");
    serial_hex(regs->rflags);
    serial_puts("\nRSP: ");
    serial_hex(regs->rsp);
    serial_puts("  SS: ");
    serial_hex(regs->ss);
    serial_puts("\nRAX: ");
    serial_hex(regs->rax);
    serial_puts("  RBX: ");
    serial_hex(regs->rbx);
    serial_puts("  RCX: ");
    serial_hex(regs->rcx);
    serial_puts("  RDX: ");
    serial_hex(regs->rdx);
    serial_puts("\nRSI: ");
    serial_hex(regs->rsi);
    serial_puts("  RDI: ");
    serial_hex(regs->rdi);
    serial_puts("  RBP: ");
    serial_hex(regs->rbp);
    serial_puts("\nR8 : ");
    serial_hex(regs->r8);
    serial_puts("  R9 : ");
    serial_hex(regs->r9);
    serial_puts("  R10: ");
    serial_hex(regs->r10);
    serial_puts("  R11: ");
    serial_hex(regs->r11);
    serial_puts("\nR12: ");
    serial_hex(regs->r12);
    serial_puts("  R13: ");
    serial_hex(regs->r13);
    serial_puts("  R14: ");
    serial_hex(regs->r14);
    serial_puts("  R15: ");
    serial_hex(regs->r15);
  } else {
    serial_puts("\n(no hay contexto de interrupción)");
  }

  serial_puts("\n--- Control registers ---");
  serial_puts("\nCR0: ");
  serial_hex(cr0);
  serial_puts("  CR2: ");
  serial_hex(cr2);
  serial_puts("  CR3: ");
  serial_hex(cr3);
  serial_puts("  CR4: ");
  serial_hex(cr4);
  serial_puts("\n");
}

void dump_scheduler(void) {
  task_t *cur = sched_current();

  serial_puts("\n--- Scheduler ---");
  if (!cur) {
    serial_puts("\n  current_task = NULL\n");
    return;
  }

  serial_puts("\n  current: id=");
  serial_putn(cur->id, 10, 0);
  serial_puts(" state=");
  serial_putn((uint64_t)cur->state, 10, 0);
  serial_puts(" preempt=");
  serial_putn((uint64_t)cur->preempt_count, 10, 0);
  serial_puts(" ptr=");
  serial_hex((uint64_t)cur);

  serial_puts("\n  task list:");
  task_t *start = cur;
  task_t *p = start;
  for (int i = 0; i < 32; i++) {
    // [FIX] Antes de dereferenciar, comprobar que el puntero está
    // en un rango razonable (kernel text + heap + slab):
    //   0xffffffff80000000 .. 0xffffffff90000000
    uint64_t addr = (uint64_t)p;
    if (addr < 0xffffffff80000000ULL || addr >= 0xffffffff90000000ULL) {
      serial_puts("\n    [");
      serial_putn((uint64_t)i, 10, 0);
      serial_puts("] puntero inválido: ");
      serial_hex(addr);
      serial_puts(" -> abortando recorrido");
      break;
    }

    serial_puts("\n    [");
    serial_putn((uint64_t)i, 10, 0);
    serial_puts("] ptr=");
    serial_hex(addr);
    serial_puts(" id=");
    serial_putn(p->id, 10, 0);
    serial_puts(" state=");
    serial_putn((uint64_t)p->state, 10, 0);
    serial_puts(" wait=");
    serial_puts(p->waiting_on ? "y" : "n");
    serial_puts(" idle=");
    serial_putn((uint64_t)p->is_idle, 10, 0);
    p = p->next;
    if (p == start)
      break;
  }
  serial_puts("\n");
}

void dump_paging(void) {
  serial_puts("\n--- Paging ---\n");
  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  serial_puts("  CR3: ");
  serial_hex(cr3);
  serial_puts("\n  PHYS_MAP_BASE: ");
  serial_hex(PHYS_MAP_BASE);
  serial_puts("\n  MMIO_MAP_BASE: ");
  serial_hex(MMIO_MAP_BASE);
  serial_puts("\n  HEAP_VMA: ");
  serial_hex(HEAP_VMA);
  serial_puts("\n  SLAB_VMA: ");
  serial_hex(SLAB_VMA);
  serial_puts("\n");
}

void dump_heap(void) {
  serial_puts("\n--- Heap ---\n");
  heap_dump();
}

void dump_slab(void) {
  serial_puts("\n--- SLAB ---\n");
  slab_dump_stats();
}

// ---------------------------------------------------------------------------
// Implementación real
// ---------------------------------------------------------------------------
__attribute__((noreturn)) static void panic_v(registers_t *regs,
                                              const char *fmt, va_list ap) {
  if (in_panic) {
    __asm__ volatile("cli; hlt");
    __builtin_unreachable();
  }
  in_panic = 1;
  __asm__ volatile("cli");

  serial_puts("\n");
  serial_puts("\n=======================================================\n");
  serial_puts("                  !!! KERNEL PANIC !!!\n");
  serial_puts("=======================================================\n");

  serial_puts("panic: ");
  klog_vprintf(KLOG_PANIC, fmt, ap);
  serial_puts("\n");

  if (regs) {
    serial_puts("\nVector: ");
    serial_putn(regs->int_num, 10, 0);
    serial_puts("  Error code: ");
    serial_hex(regs->error_code);
    serial_puts("\n");
  }

  dump_registers(regs);
  dump_scheduler();
  dump_paging();
  dump_slab();
  dump_heap();

  uint64_t gs_base = rdmsr(0xC0000101);
  uint64_t kgs_base = rdmsr(0xC0000102);
  LOG_PANIC("MSR_GS_BASE=0x%lx MSR_KERNEL_GS_BASE=0x%lx",
            (unsigned long)gs_base, (unsigned long)kgs_base);
  LOG_PANIC("cpu_local_data[0]=%p cpu_local_data[1]=%p",
            (void *)&cpu_local_data[0], (void *)&cpu_local_data[1]);

  uint64_t star = rdmsr(MSR_STAR);
  uint16_t r3_cs = (uint16_t)((star >> 48) & 0xFFFF);
  LOG_PANIC("STAR R3_CS=0x%x (expected USER_CS=0x%x)", r3_cs, USER_CS);
  LOG_PANIC("sysret will load CS=0x%x SS=0x%x", r3_cs | 3, (r3_cs + 8) | 3);

  if (regs) {
    serial_puts("\n--- Backtrace ---");
    backtrace(regs->rbp, regs->rip, 16);
  }

  serial_puts("\n=======================================================\n");
  serial_puts("System Halted.\n");

  while (1) {
    __asm__ volatile("cli; hlt");
  }
  __builtin_unreachable();
}

__attribute__((noreturn)) void panic(registers_t *regs, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  panic_v(regs, fmt, ap);
  va_end(ap);
  __builtin_unreachable();
}

__attribute__((noreturn)) void panic_noctx(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  panic_v(NULL, fmt, ap);
  va_end(ap);
  __builtin_unreachable();
}

int panic_in_progress(void) { return in_panic; }