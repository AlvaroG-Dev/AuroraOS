// kernel/vmx.h — mini-VM de AuroraOS: Intel VT-x + EPT (ver Plan_MiniVM.md).
#ifndef KERNEL_VMX_H
#define KERNEL_VMX_H

#include "aurora_vm.h"
#include <stdbool.h>
#include <stdint.h>

// Registra /dev/aurora-vm (siempre; si la CPU no sirve, AVM_GET_CAPS lo dice y
// AVM_RUN devuelve -ENODEV).
void vmx_init(void);

// Lee las capacidades VMX de la CPU actual. Devuelve true si se puede correr
// un huesped (VMX + EPT + unrestricted guest). No activa VMX.
bool vmx_probe(struct avm_caps *out);

// sched.c, antes de cambiar de tarea (IRQ apagadas): VMCLEAR del VMCS que una
// ventana dejo activo en esta CPU.
void vmx_sched_out(void);

#endif
