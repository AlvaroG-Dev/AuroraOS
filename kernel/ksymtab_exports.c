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
KEXPORT_SYMBOL(pmm_free_pages_count);
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
// [LinuxKPI L3] Modelo de dispositivos, PCI e interrupciones. La capa replica
// struct device/driver y pci_device_t (kernel/linuxkpi/aurora.h): estos
// asserts avisan si cambian.
#include "apic.h"
#include "idt.h"
#include "irq_threaded.h"
#include "sysfs_dyn.h"
KEXPORT_SYMBOL(bus_del_driver);
KEXPORT_SYMBOL(device_unregister);
KEXPORT_SYMBOL(pci_bus);
KEXPORT_SYMBOL(pci_get_devices);
KEXPORT_SYMBOL(pci_read_config_byte);
KEXPORT_SYMBOL(pci_read_config_word);
KEXPORT_SYMBOL(pci_read_config_dword);
KEXPORT_SYMBOL(pci_write_config_byte);
KEXPORT_SYMBOL(pci_write_config_word);
KEXPORT_SYMBOL(pci_write_config_dword);
KEXPORT_SYMBOL(msi_vector_alloc);
KEXPORT_SYMBOL(msi_vector_free);
KEXPORT_SYMBOL(irq_request_threaded);
KEXPORT_SYMBOL(irq_free);
KEXPORT_SYMBOL(ioapic_route_pci_intx);
KEXPORT_SYMBOL(ioapic_mask_irq);
KEXPORT_SYMBOL(smp_cpu_lapic_id);
KEXPORT_SYMBOL(pmm_alloc_pages_below);
KEXPORT_SYMBOL(sysfs_dyn_set_file);
KEXPORT_SYMBOL(sysfs_dyn_add_symlink);
KEXPORT_SYMBOL(sysfs_dyn_add_attr);
KEXPORT_SYMBOL(sysfs_dyn_remove);
KEXPORT_SYMBOL(sysfs_dyn_remove_tree);
KEXPORT_SYMBOL(sysfs_dyn_get);   // pruebas de la capa: leer /sys como el VFS
KEXPORT_SYMBOL(sysfs_dyn_store);
KEXPORT_SYMBOL(test_skip);
_Static_assert(sizeof(struct device) == 64 && offsetof(struct device, bus) == 16 &&
                   offsetof(struct device, priv) == 24 &&
                   offsetof(struct device, driver) == 48,
               "struct device cambio: actualizar struct aurora_device");
_Static_assert(sizeof(struct driver) == 72 && offsetof(struct driver, probe) == 24 &&
                   offsetof(struct driver, remove) == 32,
               "struct driver cambio: actualizar struct aurora_driver");
_Static_assert(sizeof(pci_device_t) == 60 && offsetof(pci_device_t, vendor_id) == 4 &&
                   offsetof(pci_device_t, class_id) == 12 &&
                   offsetof(pci_device_t, irq_line) == 18 &&
                   offsetof(pci_device_t, bar) == 20 &&
                   offsetof(pci_device_t, bar_is_64) == 50,
               "pci_device_t cambio: actualizar struct aurora_pci_device");
_Static_assert(sizeof(sysfs_dyn_attr_ops_t) == 16,
               "sysfs_dyn_attr_ops_t cambio: actualizar struct aurora_sysfs_attr_ops");
// Pruebas en .kur (tests/linuxkpi): informan con TEST_ASSERT del kernel.
KEXPORT_SYMBOL(test_pass);
KEXPORT_SYMBOL(test_fail);
// [LinuxKPI L4] DMA, vmap, dma_buf, firmware y E/S de usuario (K1-DMA, K6,
// K8). La capa replica vfs_ops_t, vfs_dirent_t, vfs_fs_ops_t, vma_t,
// vma_dev_ops_t y task_info_t (kernel/linuxkpi/aurora.h).
#include "devtmpfs.h"
#include "nl_uevent.h"
#include "aur_bus.h"
#include "drm/drm.h"
#include "drm/kms_present.h"
#include "cmdline.h"
#include "gpu_proxy.h"
#include "pf.h"
#include "process.h"
#include "signal.h"
#include "vfs.h"
#include "vmap.h"
KEXPORT_SYMBOL(aur_bus_launch_skipped);
KEXPORT_SYMBOL(nl_uevent_broadcast);
KEXPORT_SYMBOL(drm_backend_claim);
KEXPORT_SYMBOL(drm_backend_release);
KEXPORT_SYMBOL(drm_lx_scanout_open);
KEXPORT_SYMBOL(drm_lx_scanout_close);
KEXPORT_SYMBOL(drm_lx_scanout_present);
KEXPORT_SYMBOL(drm_lx_kms_register);
KEXPORT_SYMBOL(drm_lx_kms_unregister);
KEXPORT_SYMBOL(drm_lx_kms_screen_size);
KEXPORT_SYMBOL(gpup_get_framebuffer);
KEXPORT_SYMBOL(gpup_get_fb_mode);
KEXPORT_SYMBOL(cmdline_get);
KEXPORT_SYMBOL(task_info_get);
KEXPORT_SYMBOL(signal_unblocked_of);
KEXPORT_SYMBOL(task_tid);
KEXPORT_SYMBOL(vfs_node_alloc);
KEXPORT_SYMBOL(vfs_node_ref);
KEXPORT_SYMBOL(vfs_node_free);
KEXPORT_SYMBOL(vfs_node_get_priv);
KEXPORT_SYMBOL(vfs_node_ops);
KEXPORT_SYMBOL(vfs_node_set_priv);
KEXPORT_SYMBOL(vfs_fd_node);
KEXPORT_SYMBOL(vfs_fd_flags);
KEXPORT_SYMBOL(vfs_fd_get_current);
KEXPORT_SYMBOL(vfs_fd_put_ref);
KEXPORT_SYMBOL(vfs_fd_reserve);
KEXPORT_SYMBOL(vfs_fd_install_node);
KEXPORT_SYMBOL(vfs_fd_unreserve);
KEXPORT_SYMBOL(vfs_send_sigio);
KEXPORT_SYMBOL(vfs_fd_owner);
KEXPORT_SYMBOL(vfs_mount);
KEXPORT_SYMBOL(vfs_umount);
KEXPORT_SYMBOL(devtmpfs_register);
KEXPORT_SYMBOL(devtmpfs_unregister);
KEXPORT_SYMBOL(vma_dev_map_page);
KEXPORT_SYMBOL(vma_dev_zap);
KEXPORT_SYMBOL(vmap_area_alloc);
KEXPORT_SYMBOL(vmap_area_map);
KEXPORT_SYMBOL(vmap_area_free);
KEXPORT_SYMBOL(dmabuf_node_new_ext);
KEXPORT_SYMBOL(dmabuf_node_pages);
KEXPORT_SYMBOL(dmabuf_node_ext_priv);
/* L5 (K7): fences de Aurora para la capa y fence implicito de los dmabuf. */
KEXPORT_SYMBOL(aur_fence_create);
KEXPORT_SYMBOL(aur_fence_get);
KEXPORT_SYMBOL(aur_fence_put);
KEXPORT_SYMBOL(aur_fence_signal);
KEXPORT_SYMBOL(aur_fence_signaled);
KEXPORT_SYMBOL(aur_fence_wake);
KEXPORT_SYMBOL(aur_fence_watch);
KEXPORT_SYMBOL(aur_fence_unwatch);
KEXPORT_SYMBOL(aur_fence_from_fd);
KEXPORT_SYMBOL(aur_fence_to_fd);
KEXPORT_SYMBOL(dmabuf_node_set_resv_ops);
KEXPORT_SYMBOL(dmabuf_node_export_fence);
KEXPORT_SYMBOL(dmabuf_node_import_fence);
_Static_assert(sizeof(struct dmabuf_resv_ops) == 24 &&
                   offsetof(struct dmabuf_resv_ops, import_fence) == 8 &&
                   offsetof(struct dmabuf_resv_ops, poll) == 16,
               "dmabuf_resv_ops cambio: actualizar struct aurora_dmabuf_resv_ops");
_Static_assert(sizeof(vfs_ops_t) == 29 * 8 && offsetof(vfs_ops_t, read) == 0 &&
                   offsetof(vfs_ops_t, open) == 16 && offsetof(vfs_ops_t, close) == 24 &&
                   offsetof(vfs_ops_t, readdir) == 80 && offsetof(vfs_ops_t, poll) == 112 &&
                   offsetof(vfs_ops_t, poll_wq) == 120 && offsetof(vfs_ops_t, ioctl) == 128 &&
                   offsetof(vfs_ops_t, get_page) == 184 &&
                   offsetof(vfs_ops_t, read_fd) == 192 &&
                   offsetof(vfs_ops_t, mmap_dev) == 224,
               "vfs_ops_t cambio: actualizar struct aurora_vfs_ops");
_Static_assert(sizeof(vfs_dirent_t) == 144 && offsetof(vfs_dirent_t, type) == 128 &&
                   offsetof(vfs_dirent_t, size) == 136,
               "vfs_dirent_t cambio: actualizar struct aurora_vfs_dirent");
_Static_assert(sizeof(vfs_fs_ops_t) == 24, "vfs_fs_ops_t cambio: actualizar aurora.h");
_Static_assert(offsetof(vma_t, start) == 0 && offsetof(vma_t, end) == 8 &&
                   offsetof(vma_t, flags) == 16 && offsetof(vma_t, file_offset) == 48 &&
                   offsetof(vma_t, dev_ops) == 64 && offsetof(vma_t, dev_priv) == 72 &&
                   offsetof(vma_t, dev_proc) == 80 && offsetof(vma_t, dev_flags) == 88 &&
                   offsetof(vma_t, dev_cache) == 92,
               "vma_t cambio: actualizar struct aurora_vma");
_Static_assert(sizeof(vma_dev_ops_t) == 24, "vma_dev_ops_t cambio: actualizar aurora.h");
_Static_assert(sizeof(task_info_t) == 48 && offsetof(task_info_t, proc) == 16 &&
                   offsetof(task_info_t, comm) == 28,
               "task_info_t cambio: actualizar struct aurora_task_info");
