# Interfaz de SO del nucleo abierto de NVIDIA (Plan_NVIDIA.md, T0.2)

Generado por `scripts/nvidia-os-api.py` sobre NVIDIA open **615.78.08** (fuentes en `build/linuxkpi_src/nvidia-open`, no versionadas). Analisis lexico: los usos cuentan menciones en `src/nvidia`, `src/common` y `src/nvidia-modeset` (incluye la propia declaracion y los `#define`).

Columna **Linux** = fichero de `kernel-open/` que lo define hoy (lo que habria que reescribir en una capa propia, o lo que correria tal cual sobre LinuxKPI si se elige `kernel-open/`). Columna **Disposicion** la rellena el autor/Claude en T0.2b (ver abajo).


## os_* (el RM llama a la capa de SO) (165)

Declaradas en `src/nvidia/arch/nvalloc/unix/include/os-interface.h`. Una capa propia debe implementarlas **todas** las que tengan usos > 1 (>1 porque la declaracion cuenta).

| Funcion | Usos | Linux | Prototipo |
|---|---:|---|---|
| `os_alloc_mem` | 23 | nvidia/os-interface.c | `NV_STATUS os_alloc_mem(void **, NvU64)` |
| `os_free_mem` | 29 | nvidia/os-interface.c | `void os_free_mem(void *)` |
| `os_get_system_time` | 3 | nvidia/os-interface.c | `NV_STATUS os_get_system_time(NvU32 *, NvU32 *)` |
| `os_get_monotonic_time_ns` | 3 | nvidia/os-interface.c | `NvU64 os_get_monotonic_time_ns(void)` |
| `os_get_monotonic_time_ns_hr` | 3 | nvidia/os-interface.c | `NvU64 os_get_monotonic_time_ns_hr(void)` |
| `os_get_monotonic_tick_resolution_ns` | 2 | nvidia/os-interface.c | `NvU64 os_get_monotonic_tick_resolution_ns(void)` |
| `os_delay` | 2 | nvidia/os-interface.c | `NV_STATUS os_delay(NvU32)` |
| `os_delay_us` | 3 | nvidia/os-interface.c | `NV_STATUS os_delay_us(NvU32)` |
| `os_get_cpu_frequency` | 2 | nvidia/os-interface.c | `NvU64 os_get_cpu_frequency(void)` |
| `os_get_current_process` | 4 | nvidia/nv-mmap.c, nvidia/os-interface.c | `NvU32 os_get_current_process(void)` |
| `os_get_current_process_name` | 3 | nvidia/os-interface.c | `void os_get_current_process_name(char *, NvU32)` |
| `os_get_current_thread` | 3 | nvidia/os-interface.c | `NV_STATUS os_get_current_thread(NvU64 *)` |
| `os_string_copy` | 3 | nvidia/os-interface.c | `char os_string_copy(char *, const char *)` |
| `os_string_length` | 6 | nvidia/os-interface.c | `NvU32 os_string_length(const char *)` |
| `os_strtoul` | 3 | nvidia/os-interface.c | `NvU32 os_strtoul(const char *, char **, NvU32)` |
| `os_string_compare` | 3 | nvidia/os-interface.c | `NvS32 os_string_compare(const char *, const char *)` |
| `os_snprintf` | 8 | nvidia/os-interface.c | `NvS32 os_snprintf(char *, NvU32, const char *, ...)` |
| `os_vsnprintf` | 1 | nvidia/os-interface.c | `NvS32 os_vsnprintf(char *, NvU32, const char *, va_list)` |
| `os_log_error` | 1 | nvidia/os-interface.c | `void os_log_error(const char *, va_list)` |
| `os_mem_copy` | 8 | nvidia/os-interface.c | `void os_mem_copy(void *, const void *, NvU32)` |
| `os_memcpy_from_user` | 2 | nvidia/os-interface.c | `NV_STATUS os_memcpy_from_user(void *, const void *, NvU32)` |
| `os_memcpy_to_user` | 3 | nvidia/os-interface.c | `NV_STATUS os_memcpy_to_user(void *, const void *, NvU32)` |
| `os_mem_set` | 7 | nvidia/os-interface.c | `void os_mem_set(void *, NvU8, NvU32)` |
| `os_mem_cmp` | 2 | nvidia/os-interface.c | `NvS32 os_mem_cmp(const NvU8 *, const NvU8 *, NvU32)` |
| `os_pci_init_handle` | 3 | nvidia/os-pci.c | `void os_pci_init_handle(NvU32, NvU8, NvU8, NvU8, NvU16 *, NvU16 *)` |
| `os_pci_read_byte` | 2 | nvidia/os-pci.c | `NV_STATUS os_pci_read_byte(void *, NvU32, NvU8 *)` |
| `os_pci_read_word` | 3 | nvidia/os-pci.c | `NV_STATUS os_pci_read_word(void *, NvU32, NvU16 *)` |
| `os_pci_read_dword` | 4 | nvidia/os-pci.c | `NV_STATUS os_pci_read_dword(void *, NvU32, NvU32 *)` |
| `os_pci_write_byte` | 2 | nvidia/os-pci.c | `NV_STATUS os_pci_write_byte(void *, NvU32, NvU8)` |
| `os_pci_write_word` | 2 | nvidia/os-pci.c | `NV_STATUS os_pci_write_word(void *, NvU32, NvU16)` |
| `os_pci_write_dword` | 2 | nvidia/os-pci.c | `NV_STATUS os_pci_write_dword(void *, NvU32, NvU32)` |
| `os_pci_remove_supported` | 2 | nvidia/os-pci.c | `NvBool os_pci_remove_supported(void)` |
| `os_pci_remove` | 2 | nvidia/os-pci.c | `void os_pci_remove(void *)` |
| `os_map_kernel_space` | 6 | nvidia/os-interface.c | `void os_map_kernel_space(NvU64, NvU64, NvU32)` |
| `os_unmap_kernel_space` | 6 | nvidia/os-interface.c | `void os_unmap_kernel_space(void *, NvU64)` |
| `os_flush_cpu_cache_all` | 2 | nvidia/os-interface.c | `NV_STATUS os_flush_cpu_cache_all(void)` |
| `os_flush_user_cache` | 2 | nvidia/os-interface.c | `NV_STATUS os_flush_user_cache(void)` |
| `os_flush_cpu_write_combine_buffer` | 2 | nvidia/os-interface.c | `void os_flush_cpu_write_combine_buffer(void)` |
| `os_io_read_byte` | 3 | nvidia/os-interface.c | `NvU8 os_io_read_byte(NvU32)` |
| `os_io_read_word` | 3 | nvidia/os-interface.c | `NvU16 os_io_read_word(NvU32)` |
| `os_io_read_dword` | 2 | nvidia/os-interface.c | `NvU32 os_io_read_dword(NvU32)` |
| `os_io_write_byte` | 3 | nvidia/os-interface.c | `void os_io_write_byte(NvU32, NvU8)` |
| `os_io_write_word` | 3 | nvidia/os-interface.c | `void os_io_write_word(NvU32, NvU16)` |
| `os_io_write_dword` | 2 | nvidia/os-interface.c | `void os_io_write_dword(NvU32, NvU32)` |
| `os_is_administrator` | 2 | nvidia/os-interface.c | `NvBool os_is_administrator(void)` |
| `os_check_access` | 2 | nvidia/os-interface.c | `NvBool os_check_access(RsAccessRight accessRight)` |
| `os_dbg_init` | 2 | nvidia/os-interface.c | `void os_dbg_init(void)` |
| `os_dbg_breakpoint` | 3 | nvidia/os-interface.c | `void os_dbg_breakpoint(void)` |
| `os_dbg_set_level` | 2 | nvidia/os-interface.c | `void os_dbg_set_level(NvU32)` |
| `os_get_cpu_count` | 2 | nvidia/os-interface.c | `NvU32 os_get_cpu_count(void)` |
| `os_get_cpu_number` | 2 | nvidia/os-interface.c | `NvU32 os_get_cpu_number(void)` |
| `os_disable_console_access` | 6 | nvidia/os-interface.c | `void os_disable_console_access(void)` |
| `os_enable_console_access` | 6 | nvidia/os-interface.c | `void os_enable_console_access(void)` |
| `os_registry_init` | 2 | nvidia/os-registry.c | `NV_STATUS os_registry_init(void)` |
| `os_get_max_user_va` | 2 | nvidia/os-interface.c | `NvU64 os_get_max_user_va(void)` |
| `os_schedule` | 3 | nvidia/os-interface.c | `NV_STATUS os_schedule(void)` |
| `os_alloc_spinlock` | 2 | nvidia/os-interface.c | `NV_STATUS os_alloc_spinlock(void **)` |
| `os_free_spinlock` | 2 | nvidia/os-interface.c | `void os_free_spinlock(void *)` |
| `os_acquire_spinlock` | 2 | nvidia/os-interface.c | `NvU64 os_acquire_spinlock(void *)` |
| `os_release_spinlock` | 2 | nvidia/os-interface.c | `void os_release_spinlock(void *, NvU64)` |
| `os_queue_work_item` | 4 | nvidia/os-interface.c | `NV_STATUS os_queue_work_item(struct os_work_queue *, void *)` |
| `os_flush_work_queue` | 5 | nvidia/os-interface.c | `NV_STATUS os_flush_work_queue(struct os_work_queue *, NvBool)` |
| `os_is_queue_flush_ongoing` | 3 | nvidia/os-interface.c | `NvBool os_is_queue_flush_ongoing(struct os_work_queue *)` |
| `os_alloc_mutex` | 2 | nvidia/nv-vm.c, nvidia/os-interface.c | `NV_STATUS os_alloc_mutex(void **)` |
| `os_free_mutex` | 2 | nvidia/os-interface.c | `void os_free_mutex(void *)` |
| `os_acquire_mutex` | 2 | nvidia/os-interface.c | `NV_STATUS os_acquire_mutex(void *)` |
| `os_cond_acquire_mutex` | 2 | nvidia/os-interface.c | `NV_STATUS os_cond_acquire_mutex(void *)` |
| `os_release_mutex` | 2 | nvidia/os-interface.c | `void os_release_mutex(void *)` |
| `os_alloc_semaphore` | 2 | nvidia/os-interface.c | `void os_alloc_semaphore(NvU32)` |
| `os_free_semaphore` | 2 | nvidia/os-interface.c | `void os_free_semaphore(void *)` |
| `os_acquire_semaphore` | 2 | nvidia/os-interface.c | `NV_STATUS os_acquire_semaphore(void *)` |
| `os_cond_acquire_semaphore` | 2 | nvidia/os-interface.c | `NV_STATUS os_cond_acquire_semaphore(void *)` |
| `os_release_semaphore` | 2 | nvidia/os-interface.c | `NV_STATUS os_release_semaphore(void *)` |
| `os_alloc_rwlock` | 2 | nvidia/os-interface.c | `void os_alloc_rwlock(void)` |
| `os_free_rwlock` | 2 | nvidia/os-interface.c | `void os_free_rwlock(void *)` |
| `os_acquire_rwlock_read` | 2 | nvidia/os-interface.c | `NV_STATUS os_acquire_rwlock_read(void *)` |
| `os_acquire_rwlock_write` | 2 | nvidia/os-interface.c | `NV_STATUS os_acquire_rwlock_write(void *)` |
| `os_cond_acquire_rwlock_read` | 2 | nvidia/os-interface.c | `NV_STATUS os_cond_acquire_rwlock_read(void *)` |
| `os_cond_acquire_rwlock_write` | 2 | nvidia/os-interface.c | `NV_STATUS os_cond_acquire_rwlock_write(void *)` |
| `os_release_rwlock_read` | 2 | nvidia/os-interface.c | `void os_release_rwlock_read(void *)` |
| `os_release_rwlock_write` | 2 | nvidia/os-interface.c | `void os_release_rwlock_write(void *)` |
| `os_semaphore_may_sleep` | 4 | nvidia/os-interface.c | `NvBool os_semaphore_may_sleep(void)` |
| `os_get_version_info` | 3 | nvidia/os-interface.c | `NV_STATUS os_get_version_info(os_version_info*)` |
| `os_get_is_openrm` | 2 | nvidia/os-interface.c | `NV_STATUS os_get_is_openrm(NvBool *)` |
| `os_is_bif_reset_supported` | 2 | nvidia/os-interface.c | `NvBool os_is_bif_reset_supported(void *)` |
| `os_is_isr` | 3 | nvidia/os-interface.c | `NvBool os_is_isr(void)` |
| `os_dump_stack` | 5 | nvidia/os-interface.c | `void os_dump_stack(void)` |
| `os_is_efi_enabled` | 2 | nvidia/os-interface.c | `NvBool os_is_efi_enabled(void)` |
| `os_is_xen_dom0` | 1 | nvidia/os-interface.c | `NvBool os_is_xen_dom0(void)` |
| `os_is_vgx_hyper` | 5 | nvidia/nv-pci.c, nvidia/nv-procfs.c, nvidia/nv-vm.c, nvidia/nv.c, nvidia/os-interface.c | `NvBool os_is_vgx_hyper(void)` |
| `os_inject_vgx_msi` | 2 | nvidia/os-interface.c | `NV_STATUS os_inject_vgx_msi(NvU16, NvU64, NvU32)` |
| `os_is_grid_supported` | 6 | nvidia/os-interface.c | `NvBool os_is_grid_supported(void)` |
| `os_get_grid_csp_support` | 3 | nvidia/os-interface.c | `NvU32 os_get_grid_csp_support(void)` |
| `os_bug_check` | 2 | nvidia/os-interface.c | `void os_bug_check(NvU32, const char *)` |
| `os_lock_user_pages` | 1 | nvidia/os-mlock.c | `NV_STATUS os_lock_user_pages(void *, NvU64, void **, NvU32)` |
| `os_lookup_user_io_memory` | 1 | nvidia/os-mlock.c | `NV_STATUS os_lookup_user_io_memory(void *, NvU64, NvU64 **)` |
| `os_unlock_user_pages` | 2 | nvidia/os-mlock.c | `NV_STATUS os_unlock_user_pages(NvU64, void *, NvU32)` |
| `os_match_mmap_offset` | 2 | nvidia/os-usermap.c | `NV_STATUS os_match_mmap_offset(void *, NvU64, NvU64 *)` |
| `os_get_euid` | 3 | nvidia/os-interface.c | `NV_STATUS os_get_euid(NvU32 *)` |
| `os_get_smbios_header` | 2 | nvidia/os-interface.c | `NV_STATUS os_get_smbios_header(NvU64 *pSmbsAddr)` |
| `os_get_acpi_rsdp_from_uefi` | 2 | nvidia/os-interface.c | `NV_STATUS os_get_acpi_rsdp_from_uefi(NvU32 *)` |
| `os_add_record_for_crashLog` | 2 | nvidia/os-interface.c | `void os_add_record_for_crashLog(void *, NvU32)` |
| `os_delete_record_for_crashLog` | 2 | nvidia/os-interface.c | `void os_delete_record_for_crashLog(void *)` |
| `os_call_vgpu_vfio` | 4 | nvidia/os-interface.c | `NV_STATUS os_call_vgpu_vfio(void *, NvU32)` |
| `os_device_vm_present` | 2 | nvidia/os-interface.c | `NV_STATUS os_device_vm_present(void)` |
| `os_numa_memblock_size` | 2 | nvidia/os-interface.c | `NV_STATUS os_numa_memblock_size(NvU64 *)` |
| `os_alloc_pages_node` | 2 | nvidia/os-interface.c | `NV_STATUS os_alloc_pages_node(NvS32, NvU32, NvU32, NvU64 *)` |
| `os_get_page` | 2 | nvidia/os-interface.c | `NV_STATUS os_get_page(NvU64 address)` |
| `os_put_page` | 2 | nvidia/os-interface.c | `NV_STATUS os_put_page(NvU64 address)` |
| `os_get_page_refcount` | 2 | nvidia/os-interface.c | `NvU32 os_get_page_refcount(NvU64 address)` |
| `os_count_tail_pages` | 2 | nvidia/os-interface.c | `NvU32 os_count_tail_pages(NvU64 address)` |
| `os_free_pages_phys` | 1 | nvidia/os-interface.c | `void os_free_pages_phys(NvU64, NvU32)` |
| `os_allocate_temporary_file` | 2 | nvidia/os-interface.c | `NV_STATUS os_allocate_temporary_file(void **, NvU64)` |
| `os_close_file` | 2 | nvidia/os-interface.c | `void os_close_file(void *)` |
| `os_write_file` | 2 | nvidia/os-interface.c | `NV_STATUS os_write_file(void *, NvU8 *, NvU64, NvU64)` |
| `os_read_file` | 2 | nvidia/os-interface.c | `NV_STATUS os_read_file(void *, NvU8 *, NvU64, NvU64)` |
| `os_open_readonly_file` | 1 | nvidia/nv.c, nvidia/os-interface.c | `NV_STATUS os_open_readonly_file(const char *, void **)` |
| `os_open_and_read_file` | 1 | nvidia/nv.c, nvidia/os-interface.c | `NV_STATUS os_open_and_read_file(const char *, NvU8 *, NvU64)` |
| `os_is_nvswitch_present` | 2 | nvidia/os-interface.c | `NvBool os_is_nvswitch_present(void)` |
| `os_get_random_bytes` | 2 | nvidia/os-interface.c | `NV_STATUS os_get_random_bytes(NvU8 *, NvU16)` |
| `os_get_current_process_flags` | 2 | nvidia/os-interface.c | `NvU32 os_get_current_process_flags(void)` |
| `os_alloc_wait_queue` | 2 | nvidia/os-interface.c | `NV_STATUS os_alloc_wait_queue(os_wait_queue **)` |
| `os_free_wait_queue` | 2 | nvidia/os-interface.c | `void os_free_wait_queue(os_wait_queue *)` |
| `os_wait_uninterruptible` | 2 | nvidia/os-interface.c | `void os_wait_uninterruptible(os_wait_queue *)` |
| `os_wait_interruptible` | 2 | nvidia/os-interface.c | `void os_wait_interruptible(os_wait_queue *)` |
| `os_wake_up` | 2 | nvidia/os-interface.c | `void os_wake_up(os_wait_queue *)` |
| `os_nv_cap_init` | 1 | nvidia/os-interface.c | `nv_cap_t os_nv_cap_init(const char *)` |
| `os_nv_cap_create_dir_entry` | 6 | nvidia/os-interface.c | `nv_cap_t os_nv_cap_create_dir_entry(nv_cap_t *, const char *, int)` |
| `os_nv_cap_create_file_entry` | 10 | nvidia/os-interface.c | `nv_cap_t os_nv_cap_create_file_entry(nv_cap_t *, const char *, int)` |
| `os_nv_cap_destroy_entry` | 2 | nvidia/os-interface.c | `void os_nv_cap_destroy_entry(nv_cap_t *)` |
| `os_nv_cap_validate_and_dup_fd` | 2 | nvidia/os-interface.c | `int os_nv_cap_validate_and_dup_fd(const nv_cap_t *, int)` |
| `os_nv_cap_close_fd` | 2 | nvidia/os-interface.c | `void os_nv_cap_close_fd(int)` |
| `os_imex_channel_get` | 2 | nvidia/os-interface.c | `NvS32 os_imex_channel_get(NvU64)` |
| `os_imex_channel_count` | 2 | nvidia/os-interface.c | `NvS32 os_imex_channel_count(void)` |
| `os_tegra_igpu_perf_boost` | 2 | nvidia/os-interface.c | `NV_STATUS os_tegra_igpu_perf_boost(void *, NvBool, NvU32, int)` |
| `os_get_tegra_platform` | 2 | nvidia/os-interface.c | `NV_STATUS os_get_tegra_platform(NvU32 *)` |
| `os_enable_pci_req_atomics` | 4 | nvidia/os-pci.c | `NV_STATUS os_enable_pci_req_atomics(void *, enum os_pci_req_atomics_type)` |
| `os_pci_trigger_flr` | 1 | nvidia/os-pci.c | `void os_pci_trigger_flr(void *handle)` |
| `os_get_reclaimable_memory_usage` | 2 | nvidia/os-interface.c | `NvU64 os_get_reclaimable_memory_usage(void)` |
| `os_get_numa_node_memory_usage` | 2 | nvidia/os-interface.c | `NV_STATUS os_get_numa_node_memory_usage(NvS32, NvU64 *, NvU64 *)` |
| `os_numa_add_gpu_memory` | 2 | nvidia/os-interface.c | `NV_STATUS os_numa_add_gpu_memory(void *, NvU64, NvU64, NvU32 *)` |
| `os_numa_remove_gpu_memory` | 2 | nvidia/os-interface.c | `NV_STATUS os_numa_remove_gpu_memory(void *, NvU64, NvU64, NvU32)` |
| `os_offline_page_at_address` | 2 | nvidia/os-interface.c | `NV_STATUS os_offline_page_at_address(NvU64 address)` |
| `os_get_pid_info` | 2 | nvidia/os-interface.c | `void os_get_pid_info(void)` |
| `os_put_pid_info` | 2 | nvidia/os-interface.c | `void os_put_pid_info(void *pid_info)` |
| `os_find_ns_pid` | 2 | nvidia/os-interface.c | `NV_STATUS os_find_ns_pid(void *pid_info, NvU32 *ns_pid)` |
| `os_is_init_ns` | 2 | nvidia/os-interface.c | `NvBool os_is_init_ns(void)` |
| `os_iommu_sva_bind` | 3 | nvidia/os-interface.c | `NV_STATUS os_iommu_sva_bind(void *arg, void **handle, NvU32 *pasid)` |
| `os_iommu_sva_unbind` | 2 | nvidia/os-interface.c | `void os_iommu_sva_unbind(void *handle)` |
| `os_supports_kernel_suspend_notifiers` | 2 | nvidia/os-interface.c | `NvBool os_supports_kernel_suspend_notifiers(void)` |
| `out_string` | 3 | nvidia/os-interface.c | `void out_string(const char *str)` |
| `nv_printf` | 25 | nvidia/os-interface.c | `int nv_printf(NvU32 debuglevel, const char *printf_format, ...)` |
| `nv_vprintf` | 1 | nvidia/os-interface.c | `int nv_vprintf(NvU32 debuglevel, const char *printf_format, va_list arglist)` |
| `os_cgroup_for_pid` | 2 | nvidia/os-interface.c | `void os_cgroup_for_pid(int pid, void *pidInfo, int impl)` |
| `os_cgroup_implementation` | 3 | nvidia/os-interface.c | `NvU32 os_cgroup_implementation(void)` |
| `os_dmem_cgroup_register_region` | 3 | nvidia/os-interface.c | `void os_dmem_cgroup_register_region(const char *name, NvU64 size, NvU64 precharge, void **prechargePool)` |
| `os_dmem_cgroup_unregister_region` | 3 | nvidia/os-interface.c | `void os_dmem_cgroup_unregister_region(void *region, void *prechargePool, NvU64 precharge)` |
| `os_dmem_cgroup_try_charge` | 3 | nvidia/os-interface.c | `NV_STATUS os_dmem_cgroup_try_charge(void *region, NvU64 size, void **ret_pool, void **ret_limit_pool)` |
| `os_dmem_cgroup_uncharge` | 3 | nvidia/os-interface.c | `void os_dmem_cgroup_uncharge(void *pool, NvU64 size)` |
| `os_dmem_cgroup_pool_state_put` | 3 | nvidia/os-interface.c | `void os_dmem_cgroup_pool_state_put(void *pool)` |
| `os_dmem_cgroup_state_evict_valuable` | 3 | nvidia/os-interface.c | `NvBool os_dmem_cgroup_state_evict_valuable(void *limit_pool, void *test_pool, NvBool ignore_low, NvBool *ret_hit_low)` |
| `os_cgroup_get_from_fd` | 3 | nvidia/os-interface.c | `void os_cgroup_get_from_fd(NvU32 fd)` |
| `os_cgroup_put` | 3 | nvidia/os-interface.c | `void os_cgroup_put(void *cgroup)` |
| `os_cgroup_get` | 3 | nvidia/os-interface.c | `void os_cgroup_get(void *cgroup)` |
| `os_cgroup_parent` | 3 | nvidia/os-interface.c | `void os_cgroup_parent(void *cgroup)` |

## nv_* (servicios de la capa de SO que el RM invoca) (180)

Declaradas en `nv.h` con `NV_API_CALL`; implementadas por la capa Linux (`nv.c`, `nv-vm.c`, `nv-mmap.c`, `nv-dma.c`...).

| Funcion | Usos | Linux | Prototipo |
|---|---:|---|---|
| `nv_get_dev_minor` | 2 | nvidia/nv.c | `NvU32 nv_get_dev_minor(nv_state_t *)` |
| `nv_alloc_kernel_mapping` | 2 | nvidia/nv.c | `void nv_alloc_kernel_mapping(nv_state_t *, void *, NvU64, NvU32, NvU64, void **)` |
| `nv_free_kernel_mapping` | 2 | nvidia/nv.c | `void nv_free_kernel_mapping(nv_state_t *, void *, void *, void *)` |
| `nv_alloc_user_mapping` | 2 | nvidia/nv-usermap.c | `NV_STATUS nv_alloc_user_mapping(nv_state_t *, void *, NvU64, NvU32, NvU64, NvU32, NvU64 *, void **)` |
| `nv_free_user_mapping` | 2 | nvidia/nv-usermap.c | `void nv_free_user_mapping(nv_state_t *, void *, NvU64, void *)` |
| `nv_add_mapping_context_to_file` | 2 | nvidia/nv-usermap.c | `NV_STATUS nv_add_mapping_context_to_file(nv_state_t *, nv_usermap_access_params_t*, NvU32, void *, NvU64, NvU32)` |
| `nv_get_kern_phys_address` | 1 | nvidia/nv-vtophys.c | `NvU64 nv_get_kern_phys_address(NvU64)` |
| `nv_get_user_phys_address` | 1 | — | `NvU64 nv_get_user_phys_address(NvU64)` |
| `nv_get_adapter_state` | 1 | nvidia/nv.c | `nv_state_t nv_get_adapter_state(NvU32, NvU8, NvU8)` |
| `nv_get_ctl_state` | 10 | nvidia/nv.c | `nv_state_t nv_get_ctl_state(void)` |
| `nv_set_dma_address_size` | 3 | nvidia/nv.c | `void nv_set_dma_address_size(nv_state_t *, NvU32)` |
| `nv_alias_pages` | 2 | nvidia/nv.c | `NV_STATUS nv_alias_pages(nv_state_t *, NvU32, NvU64, NvU32, NvU32, NvU64, NvU64 *, NvBool, void **)` |
| `nv_alloc_pages` | 2 | nvidia/nv.c | `NV_STATUS nv_alloc_pages(nv_state_t *, NvU32, NvU64, NvBool, NvU32, NvBool, NvBool, NvS32, NvU64 *, void **)` |
| `nv_free_pages` | 2 | nvidia/nv.c | `NV_STATUS nv_free_pages(nv_state_t *, NvU32, NvBool, NvU32, void *)` |
| `nv_register_user_pages` | 2 | nvidia/nv.c | `NV_STATUS nv_register_user_pages(nv_state_t *, NvU64, NvU64 *, void *, void **, NvBool)` |
| `nv_unregister_user_pages` | 3 | nvidia/nv.c | `void nv_unregister_user_pages(nv_state_t *, NvU64, void **, void **)` |
| `nv_register_peer_io_mem` | 2 | nvidia/nv.c | `NV_STATUS nv_register_peer_io_mem(nv_state_t *, NvU64 *, NvU64, void **)` |
| `nv_unregister_peer_io_mem` | 3 | nvidia/nv.c | `void nv_unregister_peer_io_mem(nv_state_t *, void *)` |
| `nv_register_sgt` | 2 | nvidia/nv.c | `NV_STATUS nv_register_sgt(nv_state_t *, NvU64 *, NvU64, NvU32, void **, struct sg_table *, void *, NvBool)` |
| `nv_unregister_sgt` | 4 | nvidia/nv.c | `void nv_unregister_sgt(nv_state_t *, struct sg_table **, void **, void *)` |
| `nv_register_phys_pages` | 3 | nvidia/nv.c | `NV_STATUS nv_register_phys_pages(nv_state_t *, NvU64 *, NvU64, NvU32, void **)` |
| `nv_unregister_phys_pages` | 4 | nvidia/nv.c | `void nv_unregister_phys_pages(nv_state_t *, void *)` |
| `nv_dma_map_sgt` | 1 | nvidia/nv-dma.c | `NV_STATUS nv_dma_map_sgt(nv_dma_device_t *, NvU64, NvU64 *, NvU32, NvBool, void **)` |
| `nv_dma_map_alloc` | 4 | nvidia/nv-dma.c | `NV_STATUS nv_dma_map_alloc(nv_dma_device_t *, NvU64, NvU64 *, NvBool, NvBool, void **)` |
| `nv_dma_unmap_alloc` | 3 | nvidia/nv-dma.c | `NV_STATUS nv_dma_unmap_alloc(nv_dma_device_t *, NvU64, NvU64 *, void **)` |
| `nv_dma_map_peer` | 3 | nvidia/nv-dma.c | `NV_STATUS nv_dma_map_peer(nv_dma_device_t *, nv_dma_device_t *, NvU8, NvU64, NvU64 *)` |
| `nv_dma_unmap_peer` | 3 | nvidia/nv-dma.c | `void nv_dma_unmap_peer(nv_dma_device_t *, NvU64, NvU64)` |
| `nv_dma_map_mmio` | 2 | nvidia/nv-dma.c | `NV_STATUS nv_dma_map_mmio(nv_dma_device_t *, NvU64, NvU64 *)` |
| `nv_dma_unmap_mmio` | 2 | nvidia/nv-dma.c | `void nv_dma_unmap_mmio(nv_dma_device_t *, NvU64, NvU64)` |
| `nv_dma_cache_invalidate` | 2 | nvidia/nv-dma.c | `void nv_dma_cache_invalidate(nv_dma_device_t *, void *)` |
| `nv_dma_get_dev_pagemap` | 2 | nvidia/nv-dma.c | `void nv_dma_get_dev_pagemap(NvU64)` |
| `nv_dma_put_dev_pagemap` | 3 | nvidia/nv-dma.c | `void nv_dma_put_dev_pagemap(void *)` |
| `nv_dma_init_sysmem_window_for_fabric_access` | 2 | nvidia/nv-dma.c | `NV_STATUS nv_dma_init_sysmem_window_for_fabric_access(nv_state_t *, NvU64, NvU64 *, NvU64 *, NvBool *)` |
| `nv_dma_destroy_sysmem_window_for_fabric_access` | 2 | nvidia/nv-dma.c | `void nv_dma_destroy_sysmem_window_for_fabric_access(nv_state_t *, NvU64, NvU64)` |
| `nv_grdma_pci_topology_supported` | 2 | nvidia/nv-dmabuf.c, nvidia/nv-pci.c | `NvBool nv_grdma_pci_topology_supported(nv_state_t *, nv_dma_device_t *)` |
| `nv_start_rc_timer` | 3 | nvidia/nv.c | `NvS32 nv_start_rc_timer(nv_state_t *)` |
| `nv_stop_rc_timer` | 3 | nvidia/nv.c | `NvS32 nv_stop_rc_timer(nv_state_t *)` |
| `nv_post_event` | 2 | nvidia/nv.c | `void nv_post_event(nv_event_t *, NvHandle, NvU32, NvU32, NvU16, NvBool)` |
| `nv_get_event` | 2 | nvidia/nv.c | `NvS32 nv_get_event(nv_file_private_t *, nv_event_t *, NvU32 *)` |
| `nv_i2c_add_adapter` | 2 | nvidia/nv-i2c.c | `void nv_i2c_add_adapter(nv_state_t *, NvU32)` |
| `nv_i2c_del_adapter` | 2 | nvidia/nv-i2c.c | `void nv_i2c_del_adapter(nv_state_t *, void *)` |
| `nv_acpi_methods_init` | 2 | nvidia/nv-acpi.c | `void nv_acpi_methods_init(NvU32 *)` |
| `nv_acpi_methods_uninit` | 2 | nvidia/nv-acpi.c | `void nv_acpi_methods_uninit(void)` |
| `nv_acpi_method` | 1 | nvidia/nv-acpi.c | `NV_STATUS nv_acpi_method(NvU32, NvU32, NvU32, void *, NvU16, NvU32 *, void *, NvU16 *)` |
| `nv_acpi_d3cold_dsm_for_upstream_port` | 2 | nvidia/nv-acpi.c | `NV_STATUS nv_acpi_d3cold_dsm_for_upstream_port(nv_state_t *, NvU8 *, NvU32, NvU32, NvU32 *)` |
| `nv_acpi_dsm_method` | 2 | nvidia/nv-acpi.c | `NV_STATUS nv_acpi_dsm_method(nv_state_t *, NvU8 *, NvU32, NvBool, NvU32, void *, NvU16, NvU32 *, void *, NvU16 *)` |
| `nv_acpi_ddc_method` | 2 | nvidia/nv-acpi.c | `NV_STATUS nv_acpi_ddc_method(nv_state_t *, void *, NvU32 *, NvBool)` |
| `nv_acpi_dod_method` | 2 | nvidia/nv-acpi.c | `NV_STATUS nv_acpi_dod_method(nv_state_t *, NvU32 *, NvU32 *)` |
| `nv_acpi_rom_method` | 2 | nvidia/nv-acpi.c | `NV_STATUS nv_acpi_rom_method(nv_state_t *, NvU32 *, NvU32 *)` |
| `nv_acpi_get_powersource` | 3 | nvidia/nv-acpi.c | `NV_STATUS nv_acpi_get_powersource(NvU32 *)` |
| `nv_acpi_is_battery_present` | 2 | nvidia/nv-acpi.c | `NvBool nv_acpi_is_battery_present(void)` |
| `nv_acpi_mux_method` | 3 | nvidia/nv-acpi.c | `NV_STATUS nv_acpi_mux_method(nv_state_t *, NvU32 *, NvU32, const char *)` |
| `nv_log_error` | 2 | nvidia/nv.c | `NV_STATUS nv_log_error(nv_state_t *, NvU32, const char *, va_list)` |
| `nv_requires_dma_remap` | 2 | nvidia/nv-vm.c, nvidia/nv.c | `NvBool nv_requires_dma_remap(nv_state_t *)` |
| `nv_is_rm_firmware_active` | 1 | nvidia/nv.c | `NvBool nv_is_rm_firmware_active(nv_state_t *)` |
| `nv_get_firmware` | 4 | nvidia/nv.c | `const void nv_get_firmware(nv_state_t *, nv_firmware_type_t, nv_firmware_chip_family_t, const void **, NvU32 *)` |
| `nv_put_firmware` | 4 | nvidia/nv.c | `void nv_put_firmware(const void *)` |
| `nv_get_file_private` | 7 | nvidia/nv.c | `nv_file_private_t nv_get_file_private(NvS32, NvBool, void **)` |
| `nv_put_file_private` | 11 | nvidia/nv.c | `void nv_put_file_private(void *)` |
| `nv_get_device_memory_config` | 4 | nvidia/nv.c | `NV_STATUS nv_get_device_memory_config(nv_state_t *, NvU64 *, NvU64 *, NvU64 *, NvU64 *, NvU32 *, NvS32 *)` |
| `nv_get_egm_info` | 2 | nvidia/nv.c | `NV_STATUS nv_get_egm_info(nv_state_t *, NvU64 *, NvU64 *, NvS32 *)` |
| `nv_p2p_free_platform_data` | 1 | nvidia/nv-p2p.c | `void nv_p2p_free_platform_data(void *data)` |
| `nv_revoke_gpu_mappings` | 2 | nvidia/nv-mmap.c | `NV_STATUS nv_revoke_gpu_mappings(nv_state_t *)` |
| `nv_acquire_mmap_lock` | 4 | nvidia/nv-mmap.c | `void nv_acquire_mmap_lock(nv_state_t *)` |
| `nv_release_mmap_lock` | 4 | nvidia/nv-mmap.c | `void nv_release_mmap_lock(nv_state_t *)` |
| `nv_get_all_mappings_revoked_locked` | 3 | nvidia/nv-mmap.c | `NvBool nv_get_all_mappings_revoked_locked(nv_state_t *)` |
| `nv_set_safe_to_mmap_locked` | 3 | nvidia/nv-mmap.c | `void nv_set_safe_to_mmap_locked(nv_state_t *, NvBool)` |
| `nv_acquire_file_va` | 1 | nvidia/nv-mmap.c | `nv_alloc_mapping_list_node_t* nv_acquire_file_va(nv_file_private_t *, NvBool)` |
| `nv_release_file_va` | 1 | nvidia/nv-mmap.c | `void nv_release_file_va(nv_file_private_t *, NvBool)` |
| `nv_indicate_idle` | 6 | nvidia/nv.c | `NV_STATUS nv_indicate_idle(nv_state_t *)` |
| `nv_indicate_not_idle` | 4 | nvidia/nv.c | `NV_STATUS nv_indicate_not_idle(nv_state_t *)` |
| `nv_idle_holdoff` | 2 | nvidia/nv.c | `void nv_idle_holdoff(nv_state_t *)` |
| `nv_dynamic_power_available` | 2 | nvidia/nv.c | `NvBool nv_dynamic_power_available(nv_state_t *)` |
| `nv_audio_dynamic_power` | 2 | nvidia/nv.c | `void nv_audio_dynamic_power(nv_state_t *)` |
| `nv_control_soc_irqs` | 3 | nvidia/nv.c | `void nv_control_soc_irqs(nv_state_t *, NvBool bEnable)` |
| `nv_get_current_irq_priv_data` | 2 | nvidia/nv-platform.c | `NV_STATUS nv_get_current_irq_priv_data(nv_state_t *, NvU32 *)` |
| `nv_acquire_fabric_mgmt_cap` | 2 | nvidia/nv.c | `NV_STATUS nv_acquire_fabric_mgmt_cap(int, int*)` |
| `nv_cap_drv_init` | 1 | nvidia/nv-caps.c | `int nv_cap_drv_init(void)` |
| `nv_cap_drv_exit` | 1 | nvidia/nv-caps.c | `void nv_cap_drv_exit(void)` |
| `nv_is_gpu_accessible` | 2 | nvidia/nv.c | `NvBool nv_is_gpu_accessible(nv_state_t *)` |
| `nv_match_gpu_os_info` | 2 | nvidia/nv.c | `NvBool nv_match_gpu_os_info(nv_state_t *, void *)` |
| `nv_get_updated_emu_seg` | 2 | nvidia/nv.c | `void nv_get_updated_emu_seg(NvU32 *start, NvU32 *end)` |
| `nv_get_screen_info` | 4 | nvidia/nv.c | `void nv_get_screen_info(nv_state_t *, NvU64 *, NvU32 *, NvU32 *, NvU32 *, NvU32 *, NvU64 *)` |
| `nv_set_gpu_pg_mask` | 1 | nvidia/nv.c | `void nv_set_gpu_pg_mask(nv_state_t *)` |
| `nv_trigger_gpu_flr` | 1 | nvidia/nv.c | `void nv_trigger_gpu_flr(nv_state_t *)` |
| `nv_dma_import_sgt` | 2 | nvidia/nv-dma.c | `NV_STATUS nv_dma_import_sgt(nv_dma_device_t *, struct sg_table *, struct drm_gem_object *)` |
| `nv_dma_release_sgt` | 3 | nvidia/nv-dma.c | `void nv_dma_release_sgt(struct sg_table *, struct drm_gem_object *)` |
| `nv_dma_import_dma_buf` | 2 | nvidia/nv-dmabuf.c | `NV_STATUS nv_dma_import_dma_buf(nv_dma_device_t *, struct dma_buf *, NvBool, NvU32 *, struct sg_table **, nv_dma_buf_t **)` |
| `nv_dma_import_from_fd` | 2 | nvidia/nv-dmabuf.c | `NV_STATUS nv_dma_import_from_fd(nv_dma_device_t *, NvS32, NvBool, NvU32 *, struct sg_table **, nv_dma_buf_t **)` |
| `nv_dma_release_dma_buf` | 3 | nvidia/nv-dmabuf.c | `void nv_dma_release_dma_buf(nv_dma_buf_t *)` |
| `nv_schedule_uvm_isr` | 2 | nvidia/nv.c | `void nv_schedule_uvm_isr(nv_state_t *)` |
| `nv_schedule_uvm_drain_p2p` | 2 | nvidia/nv.c | `NV_STATUS nv_schedule_uvm_drain_p2p(NvU8 *)` |
| `nv_schedule_uvm_resume_p2p` | 2 | nvidia/nv.c | `void nv_schedule_uvm_resume_p2p(NvU8 *)` |
| `nv_platform_supports_s0ix` | 3 | nvidia/nv.c | `NvBool nv_platform_supports_s0ix(void)` |
| `nv_s2idle_pm_configured` | 2 | nvidia/nv.c | `NvBool nv_s2idle_pm_configured(void)` |
| `nv_pci_tegra_register_power_domain` | 1 | nvidia/nv.c | `NvBool nv_pci_tegra_register_power_domain(nv_state_t *, NvBool)` |
| `nv_pci_tegra_pm_init` | 2 | nvidia/nv.c | `NvBool nv_pci_tegra_pm_init(nv_state_t *)` |
| `nv_pci_tegra_pm_deinit` | 2 | nvidia/nv.c | `void nv_pci_tegra_pm_deinit(nv_state_t *)` |
| `nv_pci_read_gpu_pdi_from_dvsec` | 2 | nvidia/nv-pci.c | `NV_STATUS nv_pci_read_gpu_pdi_from_dvsec(nv_state_t *, NvU64 *)` |
| `nv_is_chassis_notebook` | 2 | nvidia/nv.c | `NvBool nv_is_chassis_notebook(void)` |
| `nv_allow_runtime_suspend` | 3 | nvidia/nv.c | `void nv_allow_runtime_suspend(nv_state_t *nv)` |
| `nv_disallow_runtime_suspend` | 2 | nvidia/nv.c | `void nv_disallow_runtime_suspend(nv_state_t *nv)` |
| `nv_get_num_phys_pages` | 2 | nvidia/nv.c | `NV_STATUS nv_get_num_phys_pages(void *, NvU32 *)` |
| `nv_get_phys_pages` | 2 | nvidia/nv.c | `NV_STATUS nv_get_phys_pages(void *, void *, NvU32 *)` |
| `nv_get_disp_smmu_stream_ids` | 2 | nvidia/nv.c | `void nv_get_disp_smmu_stream_ids(nv_state_t *, NvU32 *, NvU32 *)` |
| `nv_bpmp_send_mrq` | 2 | nvidia/nv-bpmp.c | `NV_STATUS nv_bpmp_send_mrq(nv_state_t *, NvU32, const void *, NvU32, void *, NvU32, NvS32 *, NvS32 *)` |
| `nv_i2c_transfer` | 2 | nvidia/nv-i2c.c | `NV_STATUS nv_i2c_transfer(nv_state_t *, NvU32, NvU8, nv_i2c_msg_t *, int)` |
| `nv_i2c_unregister_clients` | 2 | nvidia/nv-i2c.c | `void nv_i2c_unregister_clients(nv_state_t *)` |
| `nv_i2c_bus_status` | 2 | nvidia/nv-i2c.c | `NV_STATUS nv_i2c_bus_status(nv_state_t *, NvU32, NvS32 *, NvS32 *)` |
| `nv_imp_get_import_data` | 2 | nvidia/nv-imp.c | `NV_STATUS nv_imp_get_import_data(TEGRA_IMP_IMPORT_DATA *)` |
| `nv_imp_get_uefi_data` | 2 | nvidia/nv-imp.c | `NV_STATUS nv_imp_get_uefi_data(nv_state_t *nv, NvU32 *iso_bw_kbps, NvU32 *floor_bw_kbps)` |
| `nv_imp_enable_disable_rfl` | 2 | nvidia/nv-imp.c | `NV_STATUS nv_imp_enable_disable_rfl(nv_state_t *nv, NvBool bEnable)` |
| `nv_imp_icc_set_bw` | 2 | nvidia/nv-imp.c | `NV_STATUS nv_imp_icc_set_bw(nv_state_t *nv, NvU32 avg_bw_kbps, NvU32 floor_bw_kbps)` |
| `nv_get_num_dpaux_instances` | 2 | nvidia/nv.c | `NV_STATUS nv_get_num_dpaux_instances(nv_state_t *nv, NvU32 *num_instances)` |
| `nv_get_tegra_brightness_level` | 2 | nvidia/nv-backlight.c | `NV_STATUS nv_get_tegra_brightness_level(nv_state_t *, NvU32 *)` |
| `nv_set_tegra_brightness_level` | 2 | nvidia/nv-backlight.c | `NV_STATUS nv_set_tegra_brightness_level(nv_state_t *, NvU32)` |
| `nv_soc_device_reset` | 1 | nvidia/nv-platform.c | `NV_STATUS nv_soc_device_reset(nv_state_t *)` |
| `nv_soc_pm_powergate` | 1 | nvidia/nv-platform-pm.c | `NV_STATUS nv_soc_pm_powergate(nv_state_t *)` |
| `nv_soc_pm_unpowergate` | 1 | nvidia/nv-platform-pm.c | `NV_STATUS nv_soc_pm_unpowergate(nv_state_t *)` |
| `nv_gpio_get_pin_state` | 1 | nvidia/nv-gpio.c | `NV_STATUS nv_gpio_get_pin_state(nv_state_t *, NvU32, NvU32 *)` |
| `nv_gpio_set_pin_state` | 1 | nvidia/nv-gpio.c | `void nv_gpio_set_pin_state(nv_state_t *, NvU32, NvU32)` |
| `nv_gpio_set_pin_direction` | 1 | nvidia/nv-gpio.c | `NV_STATUS nv_gpio_set_pin_direction(nv_state_t *, NvU32, NvU32)` |
| `nv_gpio_get_pin_direction` | 1 | nvidia/nv-gpio.c | `NV_STATUS nv_gpio_get_pin_direction(nv_state_t *, NvU32, NvU32 *)` |
| `nv_gpio_get_pin_number` | 1 | nvidia/nv-gpio.c | `NV_STATUS nv_gpio_get_pin_number(nv_state_t *, NvU32, NvU32 *)` |
| `nv_gpio_get_pin_interrupt_status` | 1 | nvidia/nv-gpio.c | `NvBool nv_gpio_get_pin_interrupt_status(nv_state_t *, NvU32, NvU32)` |
| `nv_gpio_set_pin_interrupt` | 1 | nvidia/nv-gpio.c | `NV_STATUS nv_gpio_set_pin_interrupt(nv_state_t *, NvU32, NvU32)` |
| `nv_tegra_get_rm_interface_type` | 2 | nvidia/nv-ipc-soc.c | `NvU32 nv_tegra_get_rm_interface_type(NvU32)` |
| `nv_tegra_dce_register_ipc_client` | 3 | nvidia/nv-ipc-soc.c | `NV_STATUS nv_tegra_dce_register_ipc_client(NvU32, void *, nvTegraDceClientIpcCallback, NvU32 *)` |
| `nv_tegra_dce_client_ipc_send_recv` | 2 | nvidia/nv-ipc-soc.c | `NV_STATUS nv_tegra_dce_client_ipc_send_recv(NvU32, void *, NvU32)` |
| `nv_tegra_dce_unregister_ipc_client` | 2 | nvidia/nv-ipc-soc.c | `NV_STATUS nv_tegra_dce_unregister_ipc_client(NvU32)` |
| `nv_dsi_parse_panel_props` | 1 | nvidia/nv-dsi-parse-panel-props.c | `NV_STATUS nv_dsi_parse_panel_props(nv_state_t *, void *)` |
| `nv_dsi_is_panel_connected` | 1 | nvidia/nv-dsi-parse-panel-props.c | `NvBool nv_dsi_is_panel_connected(nv_state_t *)` |
| `nv_dsi_panel_enable` | 1 | nvidia/nv-dsi-parse-panel-props.c | `NV_STATUS nv_dsi_panel_enable(nv_state_t *, void *)` |
| `nv_dsi_panel_reset` | 1 | nvidia/nv-dsi-parse-panel-props.c | `NV_STATUS nv_dsi_panel_reset(nv_state_t *, void *)` |
| `nv_dsi_panel_disable` | 1 | nvidia/nv-dsi-parse-panel-props.c | `void nv_dsi_panel_disable(nv_state_t *, void *)` |
| `nv_dsi_panel_cleanup` | 1 | nvidia/nv-dsi-parse-panel-props.c | `void nv_dsi_panel_cleanup(nv_state_t *, void *)` |
| `nv_soc_mipi_cal_reset` | 1 | nvidia/nv-platform.c | `NV_STATUS nv_soc_mipi_cal_reset(nv_state_t *)` |
| `nv_soc_fuse_register_read` | 1 | nvidia/nv-platform.c | `NvU32 nv_soc_fuse_register_read(NvU32 addr)` |
| `nv_get_hdcp_enabled` | 1 | nvidia/nv-platform.c | `NvBool nv_get_hdcp_enabled(nv_state_t *nv)` |
| `nv_get_valid_window_head_mask` | 1 | nvidia/nv-platform.c | `NV_STATUS nv_get_valid_window_head_mask(nv_state_t *nv, NvU64 *)` |
| `nv_dp_uphy_pll_init` | 1 | nvidia/nv-clk.c | `NV_STATUS nv_dp_uphy_pll_init(nv_state_t *, NvU32, NvU32)` |
| `nv_dp_uphy_pll_deinit` | 1 | nvidia/nv-clk.c | `NV_STATUS nv_dp_uphy_pll_deinit(nv_state_t *)` |
| `nv_soc_i2c_hsp_semaphore_acquire` | 1 | nvidia/nv-platform.c | `NV_STATUS nv_soc_i2c_hsp_semaphore_acquire(NvU32 ownerId, NvBool bAcquire, NvU64 timeout)` |
| `nv_soc_tsec_send_cmd` | 1 | nvidia/nv-platform.c | `NvU32 nv_soc_tsec_send_cmd(void* cmd, nv_soc_tsec_cb_func_t cb_func, void* cb_context)` |
| `nv_soc_tsec_event_register` | 1 | nvidia/nv-platform.c | `NvU32 nv_soc_tsec_event_register(nv_soc_tsec_cb_func_t cb_func, void* cb_context, NvBool is_init_event)` |
| `nv_soc_tsec_event_unregister` | 1 | nvidia/nv-platform.c | `NvU32 nv_soc_tsec_event_unregister(NvBool is_init_event)` |
| `nv_soc_tsec_alloc_mem_desc` | 1 | nvidia/nv-platform.c | `void nv_soc_tsec_alloc_mem_desc(NvU32 num_bytes, NvU32 *flcn_addr)` |
| `nv_soc_tsec_free_mem_desc` | 1 | nvidia/nv-platform.c | `void nv_soc_tsec_free_mem_desc(void *mem_desc)` |
| `nv_is_clk_enabled` | 1 | nvidia/nv-clk.c | `NvBool nv_is_clk_enabled(nv_state_t *, TEGRASOC_WHICH_CLK)` |
| `nv_set_parent` | 1 | nvidia/nv-clk.c | `NV_STATUS nv_set_parent(nv_state_t *, TEGRASOC_WHICH_CLK, TEGRASOC_WHICH_CLK)` |
| `nv_get_parent` | 1 | nvidia/nv-clk.c | `NV_STATUS nv_get_parent(nv_state_t *, TEGRASOC_WHICH_CLK, TEGRASOC_WHICH_CLK*)` |
| `nv_clk_get_handles` | 1 | nvidia/nv-clk.c | `NV_STATUS nv_clk_get_handles(nv_state_t *)` |
| `nv_clk_clear_handles` | 1 | nvidia/nv-clk.c | `void nv_clk_clear_handles(nv_state_t *)` |
| `nv_enable_clk` | 2 | nvidia/nv-clk.c | `NV_STATUS nv_enable_clk(nv_state_t *, TEGRASOC_WHICH_CLK)` |
| `nv_disable_clk` | 2 | nvidia/nv-clk.c | `void nv_disable_clk(nv_state_t *, TEGRASOC_WHICH_CLK)` |
| `nv_get_curr_freq` | 1 | nvidia/nv-clk.c | `NV_STATUS nv_get_curr_freq(nv_state_t *, TEGRASOC_WHICH_CLK, NvU32 *)` |
| `nv_get_max_freq` | 2 | nvidia/nv-clk.c | `NV_STATUS nv_get_max_freq(nv_state_t *, TEGRASOC_WHICH_CLK, NvU32 *)` |
| `nv_get_min_freq` | 1 | nvidia/nv-clk.c | `NV_STATUS nv_get_min_freq(nv_state_t *, TEGRASOC_WHICH_CLK, NvU32 *)` |
| `nv_set_freq` | 2 | nvidia/nv-clk.c | `NV_STATUS nv_set_freq(nv_state_t *, TEGRASOC_WHICH_CLK, NvU32)` |
| `nv_pci_cxl_set_caching` | 2 | nvidia/nv-pci.c | `void nv_pci_cxl_set_caching(nv_state_t *, NvBool)` |
| `nv_vgpu_create_request` | 2 | — | `NV_STATUS nv_vgpu_create_request(nvidia_stack_t *, nv_state_t *, const NvU8 *, NvU32, NvU16 *, NvU32 *, NvU32 *, NvU32)` |
| `nv_vgpu_delete` | 2 | — | `NV_STATUS nv_vgpu_delete(nvidia_stack_t *, const NvU8 *, NvU16)` |
| `nv_vgpu_get_type_ids` | 2 | — | `NV_STATUS nv_vgpu_get_type_ids(nvidia_stack_t *, nv_state_t *, NvU32 *, NvU32 *, NvBool, NvU8, NvBool)` |
| `nv_vgpu_get_type_info` | 2 | — | `NV_STATUS nv_vgpu_get_type_info(nvidia_stack_t *, nv_state_t *, NvU32, char *, int, NvU8)` |
| `nv_vgpu_get_bar_info` | 2 | — | `NV_STATUS nv_vgpu_get_bar_info(nvidia_stack_t *, nv_state_t *, const NvU8 *, NvU64 *, NvU64 *, NvU64 *, NvU32 *, NvBool *, NvU8 *)` |
| `nv_vgpu_update_sysfs_info` | 2 | — | `NV_STATUS nv_vgpu_update_sysfs_info(nvidia_stack_t *, nv_state_t *, const NvU8 *, NvU32, NvU32 *)` |
| `nv_vgpu_get_hbm_info` | 2 | — | `NV_STATUS nv_vgpu_get_hbm_info(nvidia_stack_t *, nv_state_t *, const NvU8 *, NvU64 *, NvU64 *)` |
| `nv_vgpu_process_vf_info` | 2 | — | `NV_STATUS nv_vgpu_process_vf_info(nvidia_stack_t *, nv_state_t *, NvU8, NvU32, NvU8, NvU8, NvU8, NvBool, void *)` |
| `nv_gpu_bind_event` | 2 | — | `NV_STATUS nv_gpu_bind_event(nvidia_stack_t *, NvU32, NvBool *)` |
| `nv_gpu_unbind_event` | 2 | — | `NV_STATUS nv_gpu_unbind_event(nvidia_stack_t *, NvU32, NvBool *)` |
| `nv_get_current_irq_type` | 3 | nvidia/nv-gpio.c, nvidia/nv-platform.c | `nv_soc_irq_type_t nv_get_current_irq_type(nv_state_t*)` |
| `nv_flush_coherent_cpu_cache_range` | 2 | nvidia/nv.c | `void nv_flush_coherent_cpu_cache_range(nv_state_t *nv, NvU64 cpu_virtual, NvU64 size)` |
| `nv_start_snapshot_timer` | 1 | nvidia/nv.c | `void nv_start_snapshot_timer(void (*snapshot_callback)(void *context))` |
| `nv_flush_snapshot_timer` | 1 | nvidia/nv.c | `void nv_flush_snapshot_timer(void)` |
| `nv_stop_snapshot_timer` | 1 | nvidia/nv.c | `void nv_stop_snapshot_timer(void)` |
| `nv_create_nano_timer` | 2 | nvidia/nv-nano-timer.c | `void nv_create_nano_timer(nv_state_t *, void *pTmrEvent, nv_nano_timer_t **)` |
| `nv_start_nano_timer` | 2 | nvidia/nv-nano-timer.c | `void nv_start_nano_timer(nv_state_t *nv, nv_nano_timer_t *, NvU64 timens)` |
| `nv_cancel_nano_timer` | 2 | nvidia/nv-nano-timer.c | `void nv_cancel_nano_timer(nv_state_t *, nv_nano_timer_t *)` |
| `nv_destroy_nano_timer` | 2 | nvidia/nv-nano-timer.c | `void nv_destroy_nano_timer(nv_state_t *nv, nv_nano_timer_t *)` |
| `nv_get_syncpoint_aperture` | 2 | nvidia/nv-host1x.c | `NV_STATUS nv_get_syncpoint_aperture(NvU32, NvU64 *, NvU64 *, NvU32 *)` |

## rm_* (el RM las exporta; la capa de SO las llama) (97)

Implementadas por `src/nvidia` (no hay que escribirlas): son los puntos de entrada del RM.

| Funcion | Prototipo |
|---|---|
| `rm_init_rm` | `NvBool rm_init_rm(nvidia_stack_t *)` |
| `rm_shutdown_rm` | `void rm_shutdown_rm(nvidia_stack_t *)` |
| `rm_init_private_state` | `NvBool rm_init_private_state(nvidia_stack_t *, nv_state_t *)` |
| `rm_free_private_state` | `void rm_free_private_state(nvidia_stack_t *, nv_state_t *)` |
| `rm_init_adapter` | `NvBool rm_init_adapter(nvidia_stack_t *, nv_state_t *)` |
| `rm_disable_adapter` | `void rm_disable_adapter(nvidia_stack_t *, nv_state_t *)` |
| `rm_shutdown_adapter` | `void rm_shutdown_adapter(nvidia_stack_t *, nv_state_t *)` |
| `rm_exclude_adapter` | `NV_STATUS rm_exclude_adapter(nvidia_stack_t *, nv_state_t *)` |
| `rm_acquire_api_lock` | `NV_STATUS rm_acquire_api_lock(nvidia_stack_t *)` |
| `rm_release_api_lock` | `NV_STATUS rm_release_api_lock(nvidia_stack_t *)` |
| `rm_acquire_gpu_lock` | `NV_STATUS rm_acquire_gpu_lock(nvidia_stack_t *, nv_state_t *)` |
| `rm_release_gpu_lock` | `NV_STATUS rm_release_gpu_lock(nvidia_stack_t *, nv_state_t *)` |
| `rm_acquire_all_gpus_lock` | `NV_STATUS rm_acquire_all_gpus_lock(nvidia_stack_t *)` |
| `rm_release_all_gpus_lock` | `NV_STATUS rm_release_all_gpus_lock(nvidia_stack_t *)` |
| `rm_ioctl` | `NV_STATUS rm_ioctl(nvidia_stack_t *, nv_state_t *, nv_file_private_t *, NvU32, void *, NvU32)` |
| `rm_validate_ioctls` | `NV_STATUS rm_validate_ioctls(NvU32, NvU32)` |
| `rm_isr` | `NvBool rm_isr(nvidia_stack_t *, nv_state_t *, NvU32 *)` |
| `rm_isr_bh` | `void rm_isr_bh(nvidia_stack_t *, nv_state_t *)` |
| `rm_isr_bh_unlocked` | `void rm_isr_bh_unlocked(nvidia_stack_t *, nv_state_t *)` |
| `rm_is_msix_allowed` | `NvBool rm_is_msix_allowed(nvidia_stack_t *, nv_state_t *)` |
| `rm_wait_for_bar_firewall` | `NvBool rm_wait_for_bar_firewall(nvidia_stack_t *, NvU32 domain, NvU8 bus, NvU8 device, NvU8 function, NvU16 devId, NvU16 subsystemId)` |
| `rm_pmu_perfmon_get_load` | `NV_STATUS rm_pmu_perfmon_get_load(nvidia_stack_t *, nv_state_t *, NvU32 *, TEGRASOC_DEVFREQ_CLK)` |
| `rm_power_management` | `NV_STATUS rm_power_management(nvidia_stack_t *, nv_state_t *, nv_pm_action_t)` |
| `rm_stop_user_channels` | `NV_STATUS rm_stop_user_channels(nvidia_stack_t *, nv_state_t *)` |
| `rm_restart_user_channels` | `NV_STATUS rm_restart_user_channels(nvidia_stack_t *, nv_state_t *)` |
| `rm_save_low_res_mode` | `NV_STATUS rm_save_low_res_mode(nvidia_stack_t *, nv_state_t *)` |
| `rm_get_gpu_uuid` | `char rm_get_gpu_uuid(nvidia_stack_t *, nv_state_t *)` |
| `rm_get_gpu_uuid_raw` | `const NvU8 rm_get_gpu_uuid_raw(nvidia_stack_t *, nv_state_t *)` |
| `rm_set_rm_firmware_requested` | `void rm_set_rm_firmware_requested(nvidia_stack_t *, nv_state_t *)` |
| `rm_cleanup_file_private` | `void rm_cleanup_file_private(nvidia_stack_t *, nv_state_t *, nv_file_private_t *)` |
| `rm_unbind_lock` | `void rm_unbind_lock(nvidia_stack_t *, nv_state_t *)` |
| `rm_read_registry_dword` | `NV_STATUS rm_read_registry_dword(nvidia_stack_t *, nv_state_t *, const char *, NvU32 *)` |
| `rm_write_registry_dword` | `NV_STATUS rm_write_registry_dword(nvidia_stack_t *, nv_state_t *, const char *, NvU32)` |
| `rm_write_registry_binary` | `NV_STATUS rm_write_registry_binary(nvidia_stack_t *, nv_state_t *, const char *, NvU8 *, NvU32)` |
| `rm_write_registry_string` | `NV_STATUS rm_write_registry_string(nvidia_stack_t *, nv_state_t *, const char *, const char *, NvU32)` |
| `rm_parse_option_string` | `void rm_parse_option_string(nvidia_stack_t *, const char *)` |
| `rm_remove_spaces` | `char rm_remove_spaces(const char *)` |
| `rm_string_token` | `char rm_string_token(char **, const char)` |
| `rm_vgpu_vfio_set_driver_vm` | `void rm_vgpu_vfio_set_driver_vm(nvidia_stack_t *, NvBool)` |
| `rm_get_adapter_status_external` | `NV_STATUS rm_get_adapter_status_external(nvidia_stack_t *, nv_state_t *)` |
| `rm_run_rc_callback` | `NV_STATUS rm_run_rc_callback(nvidia_stack_t *, nv_state_t *)` |
| `rm_execute_work_item` | `void rm_execute_work_item(nvidia_stack_t *, void *)` |
| `rm_get_device_name` | `const char rm_get_device_name(NvU16, NvU16, NvU16)` |
| `rm_is_supported_device` | `NV_STATUS rm_is_supported_device(nvidia_stack_t *, nv_state_t *)` |
| `rm_is_supported_pci_device` | `NvBool rm_is_supported_pci_device(NvU8 pci_class, NvU8 pci_subclass, NvU16 vendor, NvU16 device, NvU16 subsystem_vendor, NvU16 subsystem_device, NvBool print_legacy_warning)` |
| `rm_is_nvidia_gpu_device` | `NvBool rm_is_nvidia_gpu_device(NvU8 pci_class, NvU8 pci_subclass, NvU16 vendor, NvU16 device)` |
| `rm_i2c_remove_adapters` | `void rm_i2c_remove_adapters(nvidia_stack_t *, nv_state_t *)` |
| `rm_i2c_is_smbus_capable` | `NvBool rm_i2c_is_smbus_capable(nvidia_stack_t *, nv_state_t *, void *)` |
| `rm_i2c_transfer` | `NV_STATUS rm_i2c_transfer(nvidia_stack_t *, nv_state_t *, void *, nv_i2c_cmd_t, NvU8, NvU8, NvU32, NvU8 *)` |
| `rm_perform_version_check` | `NV_STATUS rm_perform_version_check(nvidia_stack_t *, void *, NvU32)` |
| `rm_power_source_change_event` | `void rm_power_source_change_event(nvidia_stack_t *, NvU32)` |
| `rm_request_dnotifier_state` | `void rm_request_dnotifier_state(nvidia_stack_t *, nv_state_t *)` |
| `rm_disable_gpu_state_persistence` | `void rm_disable_gpu_state_persistence(nvidia_stack_t *sp, nv_state_t *)` |
| `rm_p2p_init_mapping` | `NV_STATUS rm_p2p_init_mapping(nvidia_stack_t *, NvU64, NvU64 *, NvU64 *, NvU64 *, NvU64 *, NvU64, NvU64, NvU64, NvU64, void (*)(void *), void *)` |
| `rm_p2p_destroy_mapping` | `NV_STATUS rm_p2p_destroy_mapping(nvidia_stack_t *, NvU64)` |
| `rm_p2p_get_pages` | `NV_STATUS rm_p2p_get_pages(nvidia_stack_t *, NvU64, NvU32, NvU64, NvU64, NvU64 *, NvU32 *, NvU32 *, NvU32 *, NvU8 **, void *, NvBool *)` |
| `rm_p2p_get_gpu_info` | `NV_STATUS rm_p2p_get_gpu_info(nvidia_stack_t *, NvU64, NvU64, NvU8 **, void **)` |
| `rm_p2p_get_pages_persistent` | `NV_STATUS rm_p2p_get_pages_persistent(nvidia_stack_t *, NvU64, NvU64, void **, NvU64 *, NvU32 *, NvBool, void *, void *, void **, NvBool *)` |
| `rm_p2p_register_callback` | `NV_STATUS rm_p2p_register_callback(nvidia_stack_t *, NvU64, NvU64, NvU64, void *, void (*)(void *), void *)` |
| `rm_p2p_put_pages` | `NV_STATUS rm_p2p_put_pages(nvidia_stack_t *, NvU64, NvU32, NvU64, void *)` |
| `rm_p2p_put_pages_persistent` | `NV_STATUS rm_p2p_put_pages_persistent(nvidia_stack_t *, void *, void *, void *)` |
| `rm_p2p_dma_map_pages` | `NV_STATUS rm_p2p_dma_map_pages(nvidia_stack_t *, nv_dma_device_t *, NvU8 *, NvU64, NvU32, NvU64 *, void **)` |
| `rm_dma_buf_dup_mem_handle` | `NV_STATUS rm_dma_buf_dup_mem_handle(nvidia_stack_t *, nv_state_t *, NvHandle, NvHandle, NvHandle, NvHandle, void *, NvHandle, NvU64, NvU64, NvU8, NvHandle *, void **, NvBool *, NvU32 *, NvBool *, nv_memory_type_t *)` |
| `rm_dma_buf_undup_mem_handle` | `void rm_dma_buf_undup_mem_handle(nvidia_stack_t *, nv_state_t *, NvHandle, NvHandle)` |
| `rm_dma_buf_map_mem_handle` | `NV_STATUS rm_dma_buf_map_mem_handle(nvidia_stack_t *, nv_state_t *, NvHandle, NvHandle, MemoryRange, NvU8, void *, NvBool, MemoryArea *)` |
| `rm_dma_buf_unmap_mem_handle` | `void rm_dma_buf_unmap_mem_handle(nvidia_stack_t *, nv_state_t *, NvHandle, NvHandle, NvU8, void *, NvBool, MemoryArea)` |
| `rm_dma_buf_get_client_and_device` | `NV_STATUS rm_dma_buf_get_client_and_device(nvidia_stack_t *, nv_state_t *, NvHandle, NvHandle, NvU8, NvHandle *, NvHandle *, NvHandle *, void **, NvBool *)` |
| `rm_dma_buf_put_client_and_device` | `void rm_dma_buf_put_client_and_device(nvidia_stack_t *, nv_state_t *, NvHandle, NvHandle, NvHandle, void *)` |
| `rm_kernel_rmapi_op` | `void rm_kernel_rmapi_op(nvidia_stack_t *sp, void *ops_cmd)` |
| `rm_get_device_remove_flag` | `NvBool rm_get_device_remove_flag(nvidia_stack_t *sp, NvU32 gpu_id)` |
| `rm_gpu_copy_mmu_faults` | `NV_STATUS rm_gpu_copy_mmu_faults(nvidia_stack_t *, nv_state_t *, NvU32 *)` |
| `rm_gpu_handle_mmu_faults` | `NV_STATUS rm_gpu_handle_mmu_faults(nvidia_stack_t *, nv_state_t *, NvU32 *)` |
| `rm_is_chipset_io_coherent` | `NvBool rm_is_chipset_io_coherent(nv_stack_t *)` |
| `rm_init_event_locks` | `NvBool rm_init_event_locks(nvidia_stack_t *, nv_state_t *)` |
| `rm_destroy_event_locks` | `void rm_destroy_event_locks(nvidia_stack_t *, nv_state_t *)` |
| `rm_get_gpu_numa_info` | `NV_STATUS rm_get_gpu_numa_info(nvidia_stack_t *, nv_state_t *, nv_ioctl_numa_info_t *)` |
| `rm_gpu_numa_online` | `NV_STATUS rm_gpu_numa_online(nvidia_stack_t *, nv_state_t *)` |
| `rm_gpu_numa_offline` | `NV_STATUS rm_gpu_numa_offline(nvidia_stack_t *, nv_state_t *)` |
| `rm_is_device_sequestered` | `NvBool rm_is_device_sequestered(nvidia_stack_t *, nv_state_t *)` |
| `rm_check_for_gpu_surprise_removal` | `void rm_check_for_gpu_surprise_removal(nvidia_stack_t *, nv_state_t *)` |
| `rm_set_external_kernel_client_count` | `NV_STATUS rm_set_external_kernel_client_count(nvidia_stack_t *, nv_state_t *, NvBool)` |
| `rm_schedule_gpu_wakeup` | `NV_STATUS rm_schedule_gpu_wakeup(nvidia_stack_t *, nv_state_t *)` |
| `rm_disable_iomap_wc` | `NvBool rm_disable_iomap_wc(void)` |
| `rm_init_tegra_dynamic_power_management` | `void rm_init_tegra_dynamic_power_management(nvidia_stack_t *, nv_state_t *)` |
| `rm_init_dynamic_power_management` | `void rm_init_dynamic_power_management(nvidia_stack_t *, nv_state_t *, NvBool)` |
| `rm_cleanup_dynamic_power_management` | `void rm_cleanup_dynamic_power_management(nvidia_stack_t *, nv_state_t *)` |
| `rm_enable_dynamic_power_management` | `void rm_enable_dynamic_power_management(nvidia_stack_t *, nv_state_t *)` |
| `rm_ref_dynamic_power` | `NV_STATUS rm_ref_dynamic_power(nvidia_stack_t *, nv_state_t *, nv_dynamic_power_mode_t)` |
| `rm_unref_dynamic_power` | `void rm_unref_dynamic_power(nvidia_stack_t *, nv_state_t *, nv_dynamic_power_mode_t)` |
| `rm_transition_dynamic_power` | `NV_STATUS rm_transition_dynamic_power(nvidia_stack_t *, nv_state_t *, NvBool, NvBool *)` |
| `rm_get_power_info` | `void rm_get_power_info(nvidia_stack_t *, nv_state_t *, nv_power_info_t *)` |
| `rm_acpi_notify` | `void rm_acpi_notify(nvidia_stack_t *, nv_state_t *, NvU32)` |
| `rm_acpi_nvpcf_notify` | `void rm_acpi_nvpcf_notify(nvidia_stack_t *)` |
| `rm_is_altstack_in_use` | `NvBool rm_is_altstack_in_use(void)` |
| `rm_notify_gpu_addition` | `void rm_notify_gpu_addition(nvidia_stack_t *, nv_state_t *)` |
| `rm_notify_gpu_removal` | `void rm_notify_gpu_removal(nvidia_stack_t *, nv_state_t *)` |
| `rm_run_nano_timer_callback` | `NV_STATUS rm_run_nano_timer_callback(nvidia_stack_t *, nv_state_t *, void *pTmrEvent)` |

## nvkms_* (nvidia-modeset llama a su capa de SO) (53)

Declaradas en `src/nvidia-modeset/os-interface/include/nvidia-modeset-os-interface.h`; las implementa `kernel-open/nvidia-modeset/nvidia-modeset-linux.c`.

| Funcion | Usos | Linux | Prototipo |
|---|---:|---|---|
| `nvkms_test_fail_alloc_core_channel` | 4 | nvidia-modeset/nvidia-modeset-linux.c | `NvBool nvkms_test_fail_alloc_core_channel(enum NvKmsFailAllocCoreChannelMethod method)` |
| `nvkms_force_frl_rate` | 3 | nvidia-modeset/nvidia-modeset-linux.c | `enum NvKmsFrlRateForce nvkms_force_frl_rate(void)` |
| `nvkms_conceal_vrr_caps` | 3 | nvidia-modeset/nvidia-modeset-linux.c | `NvBool nvkms_conceal_vrr_caps(void)` |
| `nvkms_enhanced_pcon_support` | 2 | nvidia-modeset/nvidia-modeset-linux.c | `NvBool nvkms_enhanced_pcon_support(void)` |
| `nvkms_output_rounding_fix` | 4 | nvidia-modeset/nvidia-modeset-linux.c | `NvBool nvkms_output_rounding_fix(void)` |
| `nvkms_disable_hdmi_frl` | 2 | nvidia-modeset/nvidia-modeset-linux.c | `NvBool nvkms_disable_hdmi_frl(void)` |
| `nvkms_disable_vrr_memclk_switch` | 2 | nvidia-modeset/nvidia-modeset-linux.c | `NvBool nvkms_disable_vrr_memclk_switch(void)` |
| `nvkms_hdmi_deepcolor` | 3 | nvidia-modeset/nvidia-modeset-linux.c | `NvBool nvkms_hdmi_deepcolor(void)` |
| `nvkms_max_output_color_bpc` | 5 | nvidia-modeset/nvidia-modeset-linux.c | `NvU32 nvkms_max_output_color_bpc(void)` |
| `nvkms_opportunistic_display_sync` | 2 | nvidia-modeset/nvidia-modeset-linux.c | `NvBool nvkms_opportunistic_display_sync(void)` |
| `nvkms_debug_force_color_space` | 2 | nvidia-modeset/nvidia-modeset-linux.c | `enum NvKmsDebugForceColorSpace nvkms_debug_force_color_space(void)` |
| `nvkms_enable_overlay_layers` | 2 | nvidia-modeset/nvidia-modeset-linux.c | `NvBool nvkms_enable_overlay_layers(void)` |
| `nvkms_debug_logging` | 2 | nvidia-modeset/nvidia-modeset-linux.c | `NvBool nvkms_debug_logging(void)` |
| `nvkms_extcon_report_hdmi` | 2 | nvidia-modeset/nvidia-modeset-linux.c | `void nvkms_extcon_report_hdmi(NvBool state)` |
| `nvkms_extcon_report_hdmi_audio` | 2 | nvidia-modeset/nvidia-modeset-linux.c | `void nvkms_extcon_report_hdmi_audio(NvBool state)` |
| `nvkms_call_rm` | 11 | nvidia-modeset/nvidia-modeset-linux.c | `void nvkms_call_rm(void *ops)` |
| `nvkms_alloc` | 4 | nvidia-modeset/nvidia-modeset-linux.c | `void* nvkms_alloc(size_t size, NvBool zero)` |
| `nvkms_free` | 5 | nvidia-modeset/nvidia-modeset-linux.c | `void nvkms_free(void *ptr, size_t size)` |
| `nvkms_memset` | 165 | nvidia-modeset/nvidia-modeset-linux.c | `void* nvkms_memset(void *ptr, NvU8 c, size_t size)` |
| `nvkms_memcpy` | 68 | nvidia-modeset/nvidia-modeset-linux.c | `void* nvkms_memcpy(void *dest, const void *src, size_t n)` |
| `nvkms_memmove` | 2 | nvidia-modeset/nvidia-modeset-linux.c | `void* nvkms_memmove(void *dest, const void *src, size_t n)` |
| `nvkms_memcmp` | 15 | nvidia-modeset/nvidia-modeset-linux.c | `int nvkms_memcmp(const void *s1, const void *s2, size_t n)` |
| `nvkms_strlen` | 9 | nvidia-modeset/nvidia-modeset-linux.c | `size_t nvkms_strlen(const char *s)` |
| `nvkms_strcmp` | 8 | nvidia-modeset/nvidia-modeset-linux.c | `int nvkms_strcmp(const char *s1, const char *s2)` |
| `nvkms_usleep` | 8 | nvidia-modeset/nvidia-modeset-linux.c | `void nvkms_usleep(NvU64 usec)` |
| `nvkms_get_usec` | 16 | nvidia-modeset/nvidia-modeset-linux.c | `NvU64 nvkms_get_usec(void)` |
| `nvkms_copyin` | 9 | nvidia-modeset/nvidia-modeset-linux.c | `int nvkms_copyin(void *kptr, NvU64 uaddr, size_t n)` |
| `nvkms_copyout` | 3 | nvidia-modeset/nvidia-modeset-linux.c | `int nvkms_copyout(NvU64 uaddr, const void *kptr, size_t n)` |
| `nvkms_yield` | 16 | nvidia-modeset/nvidia-modeset-linux.c | `void nvkms_yield(void)` |
| `nvkms_dump_stack` | 1 | nvidia-modeset/nvidia-modeset-linux.c | `void nvkms_dump_stack(void)` |
| `nvkms_syncpt_op` | 7 | nvidia-modeset/nvidia-modeset-linux.c | `NvBool nvkms_syncpt_op(enum NvKmsSyncPtOp op, NvKmsSyncPtOpParams *params)` |
| `nvkms_snprintf` | 22 | nvidia-modeset/nvidia-modeset-linux.c | `int nvkms_snprintf(char *str, size_t size, const char *format, ...) __attribute__((format (printf, 3, 4)))` |
| `nvkms_vsnprintf` | 4 | nvidia-modeset/nvidia-modeset-linux.c | `int nvkms_vsnprintf(char *str, size_t size, const char *format, va_list ap)` |
| `nvkms_log` | 3 | nvidia-modeset/nvidia-modeset-linux.c | `void nvkms_log(const int level, const char *gpuPrefix, const char *msg)` |
| `nvkms_alloc_ref_ptr` | 5 | nvidia-modeset/nvidia-modeset-linux.c | `struct nvkms_ref_ptr* nvkms_alloc_ref_ptr(void *ptr)` |
| `nvkms_free_ref_ptr` | 6 | nvidia-modeset/nvidia-modeset-linux.c | `void nvkms_free_ref_ptr(struct nvkms_ref_ptr *ref_ptr)` |
| `nvkms_inc_ref` | 2 | nvidia-modeset/nvidia-modeset-linux.c | `void nvkms_inc_ref(struct nvkms_ref_ptr *ref_ptr)` |
| `nvkms_dec_ref` | 2 | nvidia-modeset/nvidia-modeset-linux.c | `void* nvkms_dec_ref(struct nvkms_ref_ptr *ref_ptr)` |
| `nvkms_timer_proc_t` | 4 | — | `typedef void nvkms_timer_proc_t(void *dataPtr, NvU32 dataU32)` |
| `nvkms_alloc_timer` | 13 | nvidia-modeset/nvidia-modeset-linux.c | `nvkms_timer_handle_t* nvkms_alloc_timer(nvkms_timer_proc_t *proc, void *dataPtr, NvU32 dataU32, NvU64 usec)` |
| `nvkms_free_timer` | 16 | nvidia-modeset/nvidia-modeset-linux.c | `void nvkms_free_timer(nvkms_timer_handle_t *handle)` |
| `nvkms_get_per_open_data` | 10 | nvidia-modeset/nvidia-modeset-linux.c | `void* nvkms_get_per_open_data(int fd)` |
| `nvkms_open_gpu` | 5 | nvidia-modeset/nvidia-modeset-linux.c | `NvBool nvkms_open_gpu(NvU32 gpuId, NvBool reset_aware)` |
| `nvkms_close_gpu` | 4 | nvidia-modeset/nvidia-modeset-linux.c | `void nvkms_close_gpu(NvU32 gpuId, NvBool reset_aware)` |
| `nvkms_enumerate_gpus` | 4 | nvidia-modeset/nvidia-modeset-linux.c | `NvU32 nvkms_enumerate_gpus(nv_gpu_info_t *gpu_info)` |
| `nvkms_allow_write_combining` | 2 | nvidia-modeset/nvidia-modeset-linux.c | `NvBool nvkms_allow_write_combining(void)` |
| `nvkms_kernel_supports_syncpts` | 2 | nvidia-modeset/nvidia-modeset-linux.c | `NvBool nvkms_kernel_supports_syncpts(void)` |
| `nvkms_fd_is_nvidia_chardev` | 2 | nvidia-modeset/nvidia-modeset-linux.c | `NvBool nvkms_fd_is_nvidia_chardev(int fd)` |
| `nvkms_close_from_kapi` | 2 | nvidia-modeset/nvidia-modeset-linux.c | `void nvkms_close_from_kapi(struct nvkms_per_open *popen)` |
| `nvkms_sema_free` | 2 | nvidia-modeset/nvidia-modeset-linux.c | `void nvkms_sema_free(nvkms_sema_handle_t *sema)` |
| `nvkms_sema_down` | 3 | nvidia-modeset/nvidia-modeset-linux.c | `void nvkms_sema_down(nvkms_sema_handle_t *sema)` |
| `nvkms_sema_up` | 3 | nvidia-modeset/nvidia-modeset-linux.c | `void nvkms_sema_up(nvkms_sema_handle_t *sema)` |
| `nvkms_unregister_backlight` | 2 | nvidia-modeset/nvidia-modeset-linux.c | `void nvkms_unregister_backlight(struct nvkms_backlight_device *nvkms_bd)` |

## Resumen

- `os_*`: 165 funciones; 11 sin usos aparte de la declaracion (candidatas a stub): `os_vsnprintf`, `os_log_error`, `os_is_xen_dom0`, `os_lock_user_pages`, `os_lookup_user_io_memory`, `os_free_pages_phys`, `os_open_readonly_file`, `os_open_and_read_file`, `os_nv_cap_init`, `os_pci_trigger_flr`, `nv_vprintf`.
- `nv_*` de la capa de SO: 180; `rm_*` del RM: 97; `nvkms_*`: 53.
- `os_*` definidas en `kernel-open/`: 165 de 165.
