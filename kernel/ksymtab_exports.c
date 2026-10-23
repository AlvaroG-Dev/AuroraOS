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

#include "usb/usb_core.h"
KEXPORT_SYMBOL(usb_device_register);
KEXPORT_SYMBOL(usb_device_unregister);
KEXPORT_SYMBOL(usb_bus_init);