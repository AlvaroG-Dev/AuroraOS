// kernel/ksymtab_exports.c
//
// Lista centralizada de símbolos del kernel visibles a módulos .kur.
// Añadir nuevos exports aquí requiere incluir el header que declara el
// símbolo y añadir la línea KEXPORT_SYMBOL.
//
// NOTA: si añades un símbolo de un header, inclúyelo arriba. Si el
// header no existe o prefieres exponer sólo una función concreta, declara
// el prototipo extern justo antes del KEXPORT.

#include "ksymtab.h"
#include "bus.h"     // bus_register, device_register, bus_add_driver,
                     // drivers_probe_all (Fase 2.1b: API para .kur)
#include "heap.h"    // kmalloc, kfree
#include "klog.h"    // klog_printf, klog_puts
#include "paging.h"  // vmm_alloc_pages, vmm_free_pages, paging_map_page
#include "pci.h"     // pci_id_match (Fase 2.1b: API para .kur)
#include "sched.h"   // sched_get_ticks, sched_current
#include "aur_loader.h"
#include "firmware.h"

KEXPORT_SYMBOL(klog_printf);
KEXPORT_SYMBOL(klog_puts);

KEXPORT_SYMBOL(kmalloc);
KEXPORT_SYMBOL(kfree);

KEXPORT_SYMBOL(vmm_alloc_pages);
KEXPORT_SYMBOL(vmm_free_pages);
KEXPORT_SYMBOL(paging_map_page);

KEXPORT_SYMBOL(sched_get_ticks);
KEXPORT_SYMBOL(sched_current);

KEXPORT_SYMBOL(bus_register);
KEXPORT_SYMBOL(device_register);
KEXPORT_SYMBOL(bus_add_driver);
KEXPORT_SYMBOL(drivers_probe_all);
KEXPORT_SYMBOL(pci_id_match);

KEXPORT_SYMBOL(aur_loader_parse_streaming);
KEXPORT_SYMBOL(request_firmware);
KEXPORT_SYMBOL(release_firmware);

#include "usb/usb_core.h"
KEXPORT_SYMBOL(usb_device_register);
KEXPORT_SYMBOL(usb_device_unregister);
KEXPORT_SYMBOL(usb_bus_init);
// [LinuxKPI] Servicios que importa linuxkpi.kur (kernel/linuxkpi/aurora.h).
// mem* tambien los puede emitir el compilador en cualquier .kur.
#include "string.h"
#include "uaccess.h"
#include "pmm.h"
#include "panic.h"
#include "timerfd.h"
#include "test.h"
KEXPORT_SYMBOL(memcpy);
KEXPORT_SYMBOL(memset);
KEXPORT_SYMBOL(memmove);
KEXPORT_SYMBOL(memcmp);
KEXPORT_SYMBOL(klog_vprintf);
KEXPORT_SYMBOL(pmm_alloc_pages);
KEXPORT_SYMBOL(pmm_free_pages);
KEXPORT_SYMBOL(pmm_total_pages);
KEXPORT_SYMBOL(paging_get_phys);
KEXPORT_SYMBOL(mmio_map);
KEXPORT_SYMBOL(mmio_unmap);
KEXPORT_SYMBOL(kernel_realtime_ms);
KEXPORT_SYMBOL(preempt_disable);
KEXPORT_SYMBOL(preempt_enable);
KEXPORT_SYMBOL(copy_to_user);
KEXPORT_SYMBOL(copy_from_user);
KEXPORT_SYMBOL(strncpy_from_user);
KEXPORT_SYMBOL(panic_noctx);
// [LinuxKPI L2] Aparcar/despertar tareas, kthreads, tiempo y RCU.
#include "kthread.h"
#include "smp_boot.h"
#include "wait.h"
KEXPORT_SYMBOL(sched_yield);
KEXPORT_SYMBOL(sched_set_blocked_deadline);
KEXPORT_SYMBOL(sched_rcu_qs_count);
KEXPORT_SYMBOL(preempt_count);
KEXPORT_SYMBOL(wait_queue_init);
KEXPORT_SYMBOL(wait_queue_add_locked);
KEXPORT_SYMBOL(wait_queue_remove_locked);
KEXPORT_SYMBOL(wake_up_all_locked);
KEXPORT_SYMBOL(spin_lock_irqsave);
KEXPORT_SYMBOL(spin_unlock_irqrestore);
KEXPORT_SYMBOL(kthread_create);
KEXPORT_SYMBOL(kthread_start);
KEXPORT_SYMBOL(kthread_stop);
KEXPORT_SYMBOL(kthread_task);
KEXPORT_SYMBOL(klog_get_tsc_freq);
KEXPORT_SYMBOL(smp_online_cpus);
// kernel/linuxkpi/aurora.h replica wait_queue_t como struct aurora_wait_queue.
_Static_assert(sizeof(wait_queue_t) == 32 && offsetof(wait_queue_t, lock) == 0,
               "wait_queue_t cambio: actualizar struct aurora_wait_queue");
// Pruebas en .kur (tests/linuxkpi): informan con TEST_ASSERT del kernel.
KEXPORT_SYMBOL(test_pass);
KEXPORT_SYMBOL(test_fail);
