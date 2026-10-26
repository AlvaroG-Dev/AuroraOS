# API de Linux que consumen los drivers objetivo (LinuxKPI, L0.3)

Generado por `scripts/linuxkpi-scan.py` (no editar a mano; la tabla completa esta en `linuxkpi_api.tsv`).

**Fuentes analizadas:** Linux 6.18.59 (kernel.org, version fijada) y NVIDIA open 615.71.09 de Ubuntu (`nvidia-kernel-source-615-open`; GitHub bloqueado en el entorno: **NVIDIA no es la version fijada 615.78.08**)

Indice: 6931 cabeceras de Linux, 393977 nombres definidos. Un simbolo cuenta si un fichero del driver lo usa, lo define una cabecera de Linux y no lo define el propio driver. **Analisis lexico**: incluye todas las ramas `#if`, macros e *inlines* (que la capa tambien tiene que dar) y algo de ruido; no ve nombres creados por macros generadoras.

## Drivers

| Driver | Ficheros | Lineas | Simbolos externos distintos |
|---|---:|---:|---:|
| `bochs` | 1 | 829 | 180 |
| `simpledrm` | 3 | 1567 | 233 |
| `virtio-gpu` | 16 | 6511 | 590 |
| `i915` | 726 | 341919 | 3772 |
| `xe` | 468 | 113746 | 1783 |
| `amdgpu` | 1985 | 1196265 | 2941 |
| `nvidia` | 215 | 90146 | 1675 |

## Simbolos distintos por nivel y driver

| Nivel | `bochs` | `simpledrm` | `virtio-gpu` | `i915` | `xe` | `amdgpu` | `nvidia` | Union | de ellos funciones (no inline) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| K0 Base C | 14 | 15 | 43 | 304 | 253 | 242 | 150 | **453** | 35 |
| K1 Memoria (sin DMA) | 14 | 18 | 38 | 254 | 154 | 120 | 216 | **458** | 93 |
| K1-dma Memoria: DMA, dma-buf, IOMMU | 0 | 1 | 35 | 44 | 37 | 50 | 71 | **104** | 35 |
| K2 Sincronizacion, tiempo, trabajo diferido | 0 | 0 | 32 | 311 | 185 | 173 | 161 | **452** | 96 |
| K3 Estructuras y utilidades | 3 | 9 | 27 | 255 | 157 | 179 | 118 | **405** | 101 |
| K4 Modelo de dispositivos | 11 | 31 | 16 | 98 | 74 | 100 | 93 | **235** | 90 |
| K5 PCI, IRQ, energia | 13 | 3 | 40 | 137 | 122 | 236 | 137 | **425** | 153 |
| K6 Firmware, I2C, ACPI, backlight, debugfs... | 4 | 14 | 4 | 208 | 81 | 207 | 194 | **501** | 137 |
| K7 Fences/sync | 0 | 0 | 36 | 81 | 62 | 65 | 33 | **103** | 44 |
| K8 Entorno de usuario / VFS | 0 | 0 | 3 | 71 | 42 | 42 | 69 | **137** | 52 |
| K9 Miscelaneo (stubs: trace, kasan, hmm...) | 7 | 3 | 21 | 101 | 66 | 68 | 76 | **193** | 66 |
| K9-drm DRM core y helpers (shim propio con la opcion A) | 114 | 138 | 214 | 1616 | 288 | 960 | 339 | **2325** | 646 |
| K9-ttm TTM, drm_buddy, drm_exec, gpuvm | 0 | 0 | 0 | 70 | 157 | 106 | 0 | **190** | 95 |
| K9-sched drm/gpu_scheduler | 0 | 0 | 0 | 0 | 24 | 42 | 0 | **46** | 27 |
| K? Sin clasificar | 0 | 1 | 2 | 14 | 41 | 32 | 18 | **77** | 15 |
| **Total** | 180 | 233 | 511 | 3564 | 1743 | 2622 | 1675 | **6104** | 1685 |

Union acumulada siguiendo el orden de trabajo (cuanto anade cada driver a lo que ya necesitan los anteriores):

| Driver | Simbolos nuevos | Acumulado | de ellos K9-drm |
|---|---:|---:|---:|
| `bochs` | 180 | 180 | 114 |
| `simpledrm` | 124 | 304 | 51 |
| `virtio-gpu` | 390 | 694 | 130 |
| `i915` | 3187 | 3881 | 1486 |
| `xe` | 678 | 4559 | 175 |
| `amdgpu` | 891 | 5450 | 330 |
| `nvidia` | 654 | 6104 | 39 |

## Por tipo de definicion

| Nivel | func | inline | macro | type | enumconst | var |
|---|---:|---:|---:|---:|---:|---:|
| K0 | 35 | 75 | 299 | 39 | 3 | 2 |
| K1 | 93 | 109 | 206 | 23 | 24 | 3 |
| K1-dma | 35 | 32 | 23 | 11 | 3 | 0 |
| K2 | 96 | 121 | 180 | 36 | 12 | 7 |
| K3 | 101 | 113 | 171 | 19 | 1 | 0 |
| K4 | 90 | 32 | 61 | 34 | 16 | 2 |
| K5 | 153 | 52 | 144 | 36 | 38 | 2 |
| K6 | 137 | 39 | 182 | 75 | 63 | 5 |
| K7 | 44 | 31 | 7 | 12 | 8 | 1 |
| K8 | 52 | 21 | 33 | 25 | 5 | 1 |
| K9 | 66 | 19 | 64 | 31 | 12 | 1 |
| K9-drm | 646 | 147 | 1203 | 165 | 163 | 1 |
| K9-ttm | 95 | 20 | 30 | 30 | 14 | 1 |
| K9-sched | 27 | 1 | 2 | 8 | 8 | 0 |
| K? | 15 | 9 | 32 | 18 | 3 | 0 |

## Estimacion heuristica de tamano de la capa

Formula (orientativa, no medida): funcion 15 lineas, inline 6, tipo 8, macro 2, variable 2, constante de enum 1. Con la **opcion A** de DRM, K9-drm/ttm/sched es el *shim* propio sobre `kernel/drm/`; los «stubs» de K9 cuentan igual aunque muchos sean triviales.

| Nivel | QEMU (bochs, simpledrm, virtio-gpu) | Todos | kLOC QEMU | kLOC todos |
|---|---:|---:|---:|---:|
| K0 | 50 | 453 | 0.2 | 1.9 |
| K1 | 60 | 458 | 0.4 | 2.7 |
| K1-dma | 35 | 104 | 0.3 | 0.9 |
| K2 | 32 | 452 | 0.2 | 2.8 |
| K3 | 32 | 405 | 0.1 | 2.7 |
| K4 | 47 | 235 | 0.3 | 2.0 |
| K5 | 53 | 425 | 0.4 | 3.2 |
| K6 | 22 | 501 | 0.2 | 3.3 |
| K7 | 36 | 103 | 0.3 | 1.0 |
| K8 | 3 | 137 | 0.0 | 1.2 |
| K9 | 26 | 193 | 0.1 | 1.5 |
| K9-drm | 295 | 2325 | 2.4 | 14.5 |
| K9-ttm | 0 | 190 | 0.0 | 1.9 |
| K9-sched | 0 | 46 | 0.0 | 0.5 |
| K? | 3 | 77 | 0.0 | 0.5 |
| **Total** | | | **5.0** | **40.4** |

## Lista por nivel (los 40 mas usados; ordenados por numero de drivers y usos)

Columnas por driver: numero de usos. `-` = no lo usa.

### K0 — Base C (453 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `size_t` | type | `linux/types` | 2 | 16 | 7 | 270 | 295 | 311 | 551 |
| `ENOMEM` | macro | `asm-generic/errno-base` | 2 | 7 | 37 | 219 | 162 | 493 | 142 |
| `container_of` | macro | `linux/container_of` | 1 | 2 | 12 | 273 | 185 | 366 | 47 |
| `IS_ERR` | inline | `linux/err` | 1 | 9 | 15 | 489 | 207 | 63 | 82 |
| `PTR_ERR` | inline | `linux/err` | 1 | 4 | 10 | 337 | 164 | 54 | 44 |
| `ENODEV` | macro | `asm-generic/errno-base` | 4 | 1 | 7 | 269 | 67 | 98 | 61 |
| `ERR_PTR` | inline | `linux/err` | - | 27 | 11 | 297 | 253 | 18 | 41 |
| `WARN_ON` | macro | `asm-generic/bug` | 2 | - | 10 | 103 | 23 | 174 | 132 |
| `IS_ENABLED` | macro | `linux/kconfig` | 2 | - | 1 | 200 | 129 | 26 | 20 |
| `uint32_t` | type | `linux/types` | 1 | 1 | 171 | 1 | 7 | - | 132 |
| `EIO` | macro | `asm-generic/errno-base` | 1 | - | 1 | 112 | 53 | 79 | 29 |
| `EBUSY` | macro | `asm-generic/errno-base` | 2 | - | 3 | 52 | 45 | 74 | 29 |
| `__iomem` | macro | `linux/compiler_types` | 2 | 2 | - | 86 | 10 | 59 | 1 |
| `EINVAL` | macro | `asm-generic/errno-base` | - | 10 | 53 | - | 282 | 2773 | 198 |
| `ETIMEDOUT` | macro | `asm-generic/errno` | - | - | 1 | 46 | 31 | 168 | 3 |
| `EFAULT` | macro | `asm-generic/errno-base` | - | - | 5 | 100 | 47 | 74 | 17 |
| `ENOENT` | macro | `asm-generic/errno-base` | - | - | 6 | 65 | 46 | 96 | 14 |
| `ALIGN` | macro | `linux/linkage` | - | - | 1 | 31 | 24 | 120 | 4 |
| `fallthrough` | macro | `linux/compiler_attributes` | - | - | 1 | 88 | 12 | 22 | 2 |
| `ENOSPC` | macro | `asm-generic/errno-base` | - | - | 1 | 63 | 29 | 24 | 4 |
| `WRITE_ONCE` | macro | `asm-generic/rwonce` | - | - | 1 | 57 | 30 | 11 | 3 |
| `msecs_to_jiffies` | inline | `linux/jiffies` | - | - | 1 | 25 | 15 | 57 | 4 |
| `dma_addr_t` | type | `linux/types` | - | - | 1 | 66 | 9 | 17 | 7 |
| `BIT_ULL` | macro | `vdso/bits` | - | - | 1 | 40 | 35 | 17 | 3 |
| `WARN_ON_ONCE` | macro | `asm-generic/bug` | - | - | 2 | 19 | 8 | 50 | 6 |
| `BUG_ON` | macro | `asm-generic/bug` | - | - | 2 | 5 | 1 | 34 | 22 |
| `UINT_MAX` | macro | `vdso/limits` | - | - | 1 | 12 | 1 | 41 | 5 |
| `IS_ERR_OR_NULL` | inline | `linux/err` | - | 1 | - | 19 | 8 | 3 | 23 |
| `ERR_CAST` | inline | `linux/err` | - | 4 | 2 | 32 | 10 | 1 | - |
| `uint64_t` | type | `linux/types` | - | - | 22 | - | 11 | 2771 | 102 |
| `ssize_t` | type | `linux/types` | - | - | - | 170 | 146 | 436 | 15 |
| `unlikely` | macro | `linux/compiler` | - | - | - | 309 | 174 | 129 | 19 |
| `dev_info` | macro | `linux/dev_printk` | - | - | - | 3 | 4 | 549 | 2 |
| `EOPNOTSUPP` | macro | `asm-generic/errno` | - | - | - | 32 | 38 | 307 | 26 |
| `offsetof` | macro | `linux/stddef` | - | - | - | 32 | 22 | 329 | 1 |
| `struct list_head` | type | `linux/types` | - | - | 10 | 135 | - | 139 | 47 |
| `dev_warn` | macro | `linux/dev_printk` | - | - | - | 2 | 2 | 322 | 1 |
| `__user` | macro | `linux/compiler_types` | - | - | - | 99 | 65 | 112 | 8 |
| `GENMASK` | macro | `linux/bits` | - | - | - | 54 | 115 | 29 | 6 |
| `BUILD_BUG_ON` | macro | `linux/build_bug` | - | - | - | 152 | 44 | 2 | 5 |

### K1 — Memoria (sin DMA) (458 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `kfree` | func | `linux/slab` | - | 1 | 26 | 332 | 149 | 1361 | 74 |
| `GFP_KERNEL` | macro | `linux/gfp_types` | - | 8 | 31 | 266 | 170 | 1118 | 35 |
| `kzalloc` | macro | `linux/slab` | - | 2 | 16 | 111 | 64 | 891 | 8 |
| `IORESOURCE_MEM` | macro | `linux/ioport` | 2 | 1 | - | 4 | 4 | 15 | 6 |
| `vmap` | func | `linux/vmalloc` | - | - | 2 | 4 | 129 | 2 | 4 |
| `kmalloc` | macro | `linux/slab` | - | - | 4 | 42 | 19 | 29 | 22 |
| `copy_to_user` | inline | `linux/uaccess` | - | - | 2 | 19 | 21 | 53 | 6 |
| `kvfree` | func | `linux/slab` | - | - | 8 | 12 | 12 | 53 | 2 |
| `GFP_ATOMIC` | macro | `linux/gfp_types` | - | - | 5 | 16 | 18 | 41 | 4 |
| `copy_from_user` | inline | `linux/uaccess` | - | - | 3 | 27 | 18 | 21 | 12 |
| `readl` | macro | `asm/io` | 1 | - | - | 3 | 2 | 40 | 9 |
| `struct resource` | type | `linux/ioport` | - | 5 | - | 15 | 5 | 14 | 9 |
| `kmem_cache_free` | func | `linux/slab` | - | - | 1 | 14 | 2 | 6 | 1 |
| `kmem_cache_destroy` | func | `linux/slab` | - | - | 1 | 12 | 4 | 3 | 1 |
| `kvmalloc_array` | macro | `linux/slab` | - | - | 3 | 6 | 2 | 5 | 1 |
| `struct vm_operations_struct` | type | `linux/mm` | - | - | 1 | 4 | 3 | 1 | 4 |
| `vunmap` | func | `linux/vmalloc` | - | - | 2 | 3 | 1 | 2 | 4 |
| `iosys_map_set_vaddr` | inline | `linux/iosys-map` | - | 1 | 1 | 6 | 1 | 1 | - |
| `kmem_cache_create` | macro | `linux/slab` | - | - | 1 | 1 | 3 | 1 | 1 |
| `u64_to_user_ptr` | macro | `linux/util_macros` | - | - | 9 | 51 | 32 | 21 | - |
| `struct page` | type | `linux/mm_types` | - | - | 1 | 43 | 14 | 5 | - |
| `PAGE_ALIGN` | macro | `linux/mm` | - | - | 1 | 26 | 29 | 4 | - |
| `struct vm_area_struct` | type | `linux/mm_types` | - | - | 1 | - | 13 | 2 | 44 |
| `gfp_t` | type | `linux/gfp_types` | - | - | - | 45 | 2 | 3 | 5 |
| `struct mm_struct` | type | `linux/mm_types` | - | - | - | 3 | 4 | 36 | 11 |
| `resource_size` | inline | `linux/ioport` | - | 5 | - | 41 | 2 | - | 4 |
| `writel` | macro | `asm/io` | 2 | - | - | 8 | 3 | 39 | - |
| `page_offset` | inline | `linux/pagemap` | - | - | - | 15 | 10 | 2 | 9 |
| `memcpy_fromio` | macro | `asm/io` | - | - | - | 5 | 1 | 18 | 1 |
| `VM_FAULT_NOPAGE` | enumconst | `linux/mm_types` | - | - | - | 3 | 5 | 1 | 9 |
| `VM_FAULT_SIGBUS` | enumconst | `linux/mm_types` | - | - | - | 6 | 2 | 3 | 7 |
| `struct vm_fault` | type | `linux/mm` | - | - | - | 3 | 7 | 1 | 7 |
| `is_vmalloc_addr` | func | `linux/mm` | - | - | 1 | 2 | 1 | - | 11 |
| `memset_io` | macro | `asm/io` | - | 1 | - | 6 | - | 3 | 1 |
| `VM_DONTEXPAND` | macro | `linux/mm` | - | - | 1 | 2 | 3 | - | 4 |
| `kmem_cache_zalloc` | macro | `linux/slab` | - | - | 2 | 4 | 2 | - | 1 |
| `pgprot_noncached` | macro | `linux/pgtable` | - | - | 1 | 1 | 2 | - | 4 |
| `struct iosys_map` | type | `linux/iosys-map` | 2 | 3 | - | - | - | 2 | 1 |
| `iosys_map_set_vaddr_iomem` | inline | `linux/iosys-map` | - | 1 | - | 3 | 2 | 1 | - |
| `IORESOURCE_UNSET` | macro | `linux/ioport` | - | - | - | 1 | 1 | 2 | 2 |

### K1-dma — Memoria: DMA, dma-buf, IOMMU (104 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `sg_dma_len` | macro | `linux/scatterlist` | - | - | 4 | 50 | 4 | 1 | 4 |
| `struct dma_buf_attachment` | type | `linux/dma-buf` | - | - | 13 | 4 | 9 | 12 | 8 |
| `sg_dma_address` | macro | `linux/scatterlist` | - | - | 4 | 30 | 2 | 3 | 4 |
| `sg_free_table` | func | `linux/scatterlist` | - | - | 4 | 13 | 7 | 14 | 4 |
| `DMA_BIDIRECTIONAL` | enumconst | `linux/dma-direction` | - | - | 3 | 8 | 9 | 12 | 9 |
| `DMA_ATTR_SKIP_CPU_SYNC` | macro | `linux/dma-mapping` | - | - | 2 | 3 | 5 | 9 | 4 |
| `sg_alloc_table` | func | `linux/scatterlist` | - | - | 2 | 11 | 1 | 3 | 3 |
| `DMA_TO_DEVICE` | enumconst | `linux/dma-direction` | - | - | 4 | 1 | 2 | 6 | 3 |
| `sg_set_page` | inline | `linux/scatterlist` | - | - | 2 | 9 | 1 | 1 | 3 |
| `dma_buf_put` | func | `linux/dma-buf` | - | - | 1 | 1 | 1 | 2 | 6 |
| `DMA_FROM_DEVICE` | enumconst | `linux/dma-direction` | - | 2 | 1 | - | 3 | 1 | 3 |
| `dma_buf_unmap_attachment` | func | `linux/dma-buf` | - | - | 2 | 1 | 4 | 1 | 1 |
| `get_dma_buf` | inline | `linux/dma-buf` | - | - | 1 | 1 | 2 | 3 | 1 |
| `dma_buf_map_attachment` | func | `linux/dma-buf` | - | - | 1 | 1 | 1 | 1 | 1 |
| `dma_set_max_seg_size` | inline | `linux/dma-mapping` | - | - | 1 | 1 | 1 | 1 | 1 |
| `enum dma_data_direction` | type | `linux/dma-direction` | - | - | 6 | 3 | - | 13 | 8 |
| `struct scatterlist` | type | `linux/scatterlist` | - | - | 8 | - | 5 | 2 | 14 |
| `DMA_BIT_MASK` | macro | `linux/dma-mapping` | - | - | - | 3 | 2 | 7 | 3 |
| `sg_page` | inline | `linux/scatterlist` | - | - | - | 12 | 1 | 1 | 1 |
| `dma_mapping_error` | inline | `linux/dma-mapping` | - | - | 1 | - | 2 | 4 | 4 |
| `dma_unmap_resource` | func | `linux/dma-mapping` | - | - | 1 | - | 2 | 4 | 1 |
| `dma_map_resource` | func | `linux/dma-mapping` | - | - | 1 | - | 1 | 2 | 1 |
| `struct dma_buf_ops` | type | `linux/dma-buf` | - | - | - | 1 | 1 | 2 | 1 |
| `struct dma_buf` | type | `linux/dma-buf` | - | - | 7 | - | 13 | 24 | - |
| `sg_next` | inline | `linux/scatterlist` | - | - | - | 9 | 1 | - | 2 |
| `for_each_sgtable_sg` | macro | `linux/scatterlist` | - | - | 2 | - | 4 | 4 | - |
| `dma_map_sgtable` | func | `linux/dma-mapping` | - | - | - | 3 | 2 | 3 | - |
| `dma_unmap_sgtable` | inline | `linux/dma-mapping` | - | - | - | 1 | 3 | 4 | - |
| `struct dma_buf_attach_ops` | type | `linux/dma-buf` | - | - | 1 | - | 3 | 2 | - |
| `dma_set_mask` | func | `linux/dma-mapping` | - | - | - | 1 | 1 | - | 3 |
| `dma_buf_detach` | func | `linux/dma-buf` | - | - | 1 | 1 | - | - | 2 |
| `dma_free_coherent` | inline | `linux/dma-mapping` | - | - | - | 2 | - | 1 | 1 |
| `sg_alloc_table_from_pages_segment` | func | `linux/scatterlist` | - | - | - | 2 | 1 | - | 1 |
| `DEFINE_DMA_BUF_EXPORT_INFO` | macro | `linux/dma-buf` | - | - | 1 | 1 | - | - | 1 |
| `dma_alloc_coherent` | inline | `linux/dma-mapping` | - | - | - | 1 | - | 1 | 1 |
| `dma_buf_dynamic_attach` | func | `linux/dma-buf` | - | - | 1 | - | 1 | 1 | - |
| `dma_max_mapping_size` | func | `linux/dma-mapping` | - | - | 1 | 1 | 1 | - | - |
| `dma_set_coherent_mask` | func | `linux/dma-mapping` | - | - | - | 1 | 1 | - | 1 |
| `struct sg_table` | type | `linux/scatterlist` | - | - | 15 | - | - | 14 | - |
| `for_each_sg` | macro | `linux/scatterlist` | - | - | - | 2 | - | - | 7 |

### K2 — Sincronizacion, tiempo, trabajo diferido (452 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `mutex_unlock` | func | `linux/mutex` | - | - | 2 | 343 | 181 | 683 | 59 |
| `mutex_lock` | macro | `linux/mutex` | - | - | 2 | 274 | 164 | 630 | 46 |
| `spin_unlock_irqrestore` | inline | `linux/spinlock` | - | - | 2 | 138 | 15 | 161 | 32 |
| `spin_unlock` | inline | `linux/spinlock` | - | - | 26 | 132 | 51 | 106 | 9 |
| `spin_lock_irqsave` | macro | `linux/spinlock` | - | - | 2 | 124 | 14 | 134 | 27 |
| `spin_lock` | inline | `linux/spinlock` | - | - | 23 | 118 | 50 | 92 | 9 |
| `atomic_read` | inline | `linux/atomic/atomic-instrumented` | - | - | 3 | 132 | 28 | 75 | 29 |
| `mutex_init` | macro | `linux/mutex` | - | - | 1 | 48 | 13 | 82 | 8 |
| `spin_lock_init` | macro | `linux/spinlock` | - | - | 6 | 43 | 24 | 39 | 11 |
| `spinlock_t` | type | `linux/spinlock_types` | - | - | 6 | 42 | 26 | 36 | 10 |
| `current` | macro | `asm/current` | - | - | 1 | 22 | 16 | 28 | 46 |
| `atomic_set` | inline | `linux/atomic/atomic-instrumented` | - | - | 4 | 16 | 5 | 74 | 11 |
| `INIT_WORK` | macro | `linux/workqueue` | - | - | 3 | 35 | 30 | 37 | 1 |
| `MAX_SCHEDULE_TIMEOUT` | macro | `linux/sched` | - | - | 3 | 28 | 21 | 17 | 1 |
| `mutex_destroy` | func | `linux/mutex` | - | - | 1 | 20 | 14 | 33 | 2 |
| `atomic_inc` | inline | `linux/atomic/atomic-instrumented` | - | - | 2 | 41 | 1 | 16 | 8 |
| `atomic64_read` | inline | `linux/atomic/atomic-instrumented` | - | - | 1 | 3 | 1 | 36 | 14 |
| `schedule_work` | inline | `linux/workqueue` | - | - | 4 | 4 | 4 | 35 | 1 |
| `atomic64_set` | inline | `linux/atomic/atomic-instrumented` | - | - | 1 | 2 | 1 | 32 | 2 |
| `wait_queue_head_t` | type | `linux/wait` | - | - | 2 | 10 | 6 | 2 | 7 |
| `init_waitqueue_head` | macro | `linux/wait` | - | - | 2 | 9 | 6 | 2 | 6 |
| `wake_up_all` | macro | `linux/wait` | - | - | 3 | 7 | 13 | 1 | 1 |
| `wake_up` | macro | `linux/wait` | - | - | 5 | 3 | 4 | 2 | 2 |
| `struct ww_acquire_ctx` | type | `linux/ww_mutex` | - | - | 1 | 1 | 1 | 9 | 1 |
| `udelay` | inline | `asm-generic/delay` | - | - | - | 69 | 3 | 489 | 3 |
| `msleep` | func | `linux/delay` | - | - | - | 33 | 14 | 147 | 1 |
| `spin_unlock_irq` | inline | `linux/spinlock` | - | - | - | 129 | 25 | 12 | 3 |
| `spin_lock_irq` | inline | `linux/spinlock` | - | - | - | 125 | 25 | 12 | 3 |
| `struct mutex` | type | `linux/mutex_types` | - | - | 1 | - | 32 | 70 | 9 |
| `cancel_delayed_work_sync` | func | `linux/workqueue` | - | - | - | 17 | 3 | 72 | 3 |
| `flush_work` | func | `linux/workqueue` | - | - | 4 | 30 | 12 | 16 | - |
| `struct delayed_work` | type | `linux/workqueue` | - | - | - | 20 | 4 | 27 | 3 |
| `INIT_DELAYED_WORK` | macro | `linux/workqueue` | - | - | - | 17 | 4 | 28 | 3 |
| `kref_put` | inline | `linux/kref` | - | - | - | 15 | 9 | 15 | 6 |
| `kref_init` | inline | `linux/kref` | - | - | - | 18 | 8 | 14 | 2 |
| `atomic_dec` | inline | `linux/atomic/atomic-instrumented` | - | - | - | 24 | 1 | 8 | 6 |
| `kref_get` | inline | `linux/kref` | - | - | - | 18 | 7 | 10 | 3 |
| `mutex_lock_interruptible` | macro | `linux/mutex` | - | - | - | 23 | 4 | 2 | 7 |
| `kref_get_unless_zero` | inline | `linux/kref` | - | - | - | 25 | 4 | 2 | 1 |
| `struct timer_list` | type | `linux/timer_types` | - | - | - | 18 | 2 | 4 | 7 |

### K3 — Estructuras y utilidades (405 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `min` | macro | `linux/minmax` | 1 | 1 | 3 | 380 | 56 | 839 | 14 |
| `ARRAY_SIZE` | macro | `linux/array_size` | 2 | 3 | 4 | 538 | 223 | - | 22 |
| `memcpy` | macro | `linux/fortify-string` | - | 1 | 3 | 107 | 26 | 539 | 48 |
| `strlen` | macro | `linux/fortify-string` | - | 2 | 2 | 3 | 12 | 51 | 35 |
| `strscpy` | macro | `linux/string` | - | 1 | 1 | 3 | 6 | 41 | 1 |
| `max` | macro | `linux/minmax` | - | 2 | - | 486 | 51 | 769 | 16 |
| `memset` | macro | `linux/fortify-string` | - | - | 27 | 149 | 23 | 943 | 41 |
| `parent` | inline | `linux/min_heap` | - | - | 7 | 131 | 140 | 119 | 37 |
| `list_for_each_entry` | macro | `linux/list` | - | - | 5 | 89 | 33 | 147 | 10 |
| `INIT_LIST_HEAD` | inline | `linux/list` | - | - | 5 | 72 | 51 | 72 | 33 |
| `list_empty` | inline | `linux/list` | - | - | 1 | 75 | 40 | 54 | 26 |
| `list_for_each_entry_safe` | macro | `linux/list` | - | - | 5 | 53 | 24 | 56 | 7 |
| `list_del` | inline | `linux/list` | - | - | 5 | 33 | 17 | 44 | 21 |
| `list_add_tail` | inline | `linux/list` | - | - | 4 | 40 | 19 | 34 | 16 |
| `strcmp` | func | `linux/string` | - | 1 | - | 18 | 8 | 19 | 27 |
| `array_size` | macro | `linux/overflow` | - | - | 6 | 1 | 4 | 54 | 2 |
| `list_first_entry` | macro | `linux/list` | - | - | 1 | 5 | 2 | 14 | 9 |
| `str_yes_no` | inline | `linux/string_choices` | - | - | 1 | 193 | 27 | 12 | - |
| `DIV_ROUND_UP` | macro | `linux/math` | - | - | 1 | 157 | 15 | 26 | - |
| `min_t` | macro | `linux/minmax` | - | - | 2 | 71 | 28 | 79 | - |
| `memcmp` | func | `linux/fortify-string` | - | - | - | 34 | 3 | 89 | 11 |
| `ilog2` | macro | `linux/log2` | - | - | - | 37 | 46 | 22 | 1 |
| `struct_size` | macro | `linux/overflow` | - | - | 1 | 9 | 3 | 88 | - |
| `max_t` | macro | `linux/minmax` | - | 2 | - | 31 | 9 | 19 | - |
| `list_del_init` | inline | `linux/list` | - | - | - | 24 | 25 | 5 | 6 |
| `list_add` | inline | `linux/list` | - | - | - | 19 | 7 | 17 | 8 |
| `list_move_tail` | inline | `linux/list` | - | - | - | 26 | 10 | 4 | 2 |
| `list_entry` | macro | `linux/list` | - | - | - | 5 | 4 | 10 | 21 |
| `struct rb_node` | type | `linux/rbtree_types` | - | - | - | 32 | 2 | 1 | 5 |
| `list_first_entry_or_null` | macro | `linux/list` | - | - | - | 13 | 10 | 7 | 2 |
| `strncmp` | func | `linux/string` | - | - | - | 4 | 2 | 23 | 3 |
| `idr_for_each_entry` | macro | `linux/idr` | - | - | - | 5 | 2 | 19 | 3 |
| `strsep` | func | `linux/string` | - | - | - | 2 | 1 | 9 | 12 |
| `crc32` | inline | `linux/crc32` | - | - | - | 3 | 4 | 2 | 11 |
| `DIV_ROUND_DOWN_ULL` | macro | `linux/math` | 1 | - | - | 12 | 1 | 1 | - |
| `strstr` | func | `linux/string` | - | 2 | - | - | 1 | 2 | 10 |
| `roundup` | macro | `linux/math` | - | - | 1 | 4 | - | 5 | 2 |
| `RB_EMPTY_ROOT` | macro | `linux/rbtree` | - | - | - | 4 | 1 | 1 | 2 |
| `lower_32_bits` | macro | `linux/wordpart` | - | - | - | 67 | 47 | 1050 | - |
| `upper_32_bits` | macro | `linux/wordpart` | - | - | - | 66 | 47 | 1014 | - |

### K4 — Modelo de dispositivos (235 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `MODULE_DESCRIPTION` | macro | `linux/module` | 1 | 1 | 1 | 1 | 1 | 2 | 3 |
| `MODULE_LICENSE` | macro | `linux/module` | 1 | 1 | 1 | 1 | 1 | 2 | 3 |
| `MODULE_DEVICE_TABLE` | macro | `linux/module` | 1 | 1 | 1 | 1 | 1 | 1 | 3 |
| `MODULE_PARM_DESC` | macro | `linux/moduleparam` | 3 | - | 1 | 5 | 11 | 99 | 14 |
| `module_param_named` | macro | `linux/moduleparam` | 1 | - | 2 | 2 | 5 | 76 | 20 |
| `struct device` | type | `linux/device` | 2 | 2 | 4 | - | - | 212 | 74 |
| `dev_get_drvdata` | inline | `linux/device` | 2 | - | - | 9 | 7 | 139 | 2 |
| `__init` | macro | `linux/init` | - | - | 1 | 9 | 7 | 1 | 14 |
| `dev_name` | inline | `linux/device` | - | - | 2 | 10 | 8 | 4 | 7 |
| `MODULE_IMPORT_NS` | macro | `linux/module` | - | - | 1 | 1 | 3 | 1 | 5 |
| `module_exit` | macro | `linux/module` | - | - | 1 | 1 | 1 | 2 | 6 |
| `MODULE_AUTHOR` | macro | `linux/module` | 1 | - | 3 | 2 | 1 | 2 | - |
| `__exit` | macro | `linux/init` | - | - | 1 | 1 | 1 | 2 | 3 |
| `module_init` | macro | `linux/module` | - | - | 1 | 1 | 1 | 1 | 3 |
| `MODULE_FIRMWARE` | macro | `linux/module` | - | - | - | 18 | 1 | 670 | 1 |
| `PCI_ANY_ID` | macro | `linux/mod_devicetable` | 4 | - | - | 4 | - | 619 | 38 |
| `THIS_MODULE` | macro | `linux/init` | - | - | - | 39 | 14 | 74 | 18 |
| `DMI_PRODUCT_NAME` | enumconst | `linux/mod_devicetable` | - | - | - | 25 | 1 | 17 | 1 |
| `struct platform_device` | type | `linux/platform_device` | - | 5 | - | 3 | - | 9 | 16 |
| `struct pci_device_id` | type | `linux/mod_devicetable` | 2 | - | - | - | 6 | 4 | 15 |
| `dev_pm_set_driver_flags` | inline | `linux/device` | - | - | - | 1 | 1 | 2 | 2 |
| `device_link_add` | func | `linux/device` | - | 1 | - | 2 | 1 | 1 | - |
| `struct device_link` | type | `linux/device` | - | 1 | - | 1 | 1 | 2 | - |
| `sysfs_emit_at` | func | `linux/sysfs` | - | - | - | 2 | 2 | 392 | - |
| `struct kobject` | type | `linux/kobject` | - | - | - | 80 | 85 | 61 | - |
| `struct device_attribute` | type | `linux/device` | - | - | - | 12 | 15 | 176 | - |
| `sysfs_emit` | func | `linux/sysfs` | - | - | - | 35 | 49 | 119 | - |
| `struct kobj_attribute` | type | `linux/kobject` | - | - | - | 74 | 96 | 26 | - |
| `EXPORT_SYMBOL` | macro | `linux/export` | - | 12 | - | - | - | 7 | 90 |
| `struct attribute` | type | `linux/sysfs` | - | - | - | 29 | 20 | 36 | - |
| `struct attribute_group` | type | `linux/sysfs` | - | - | - | 10 | 13 | 29 | - |
| `__ATTR` | macro | `linux/sysfs` | - | - | - | 28 | 18 | 2 | - |
| `kobj_to_dev` | macro | `linux/device` | - | - | - | 8 | 27 | 12 | - |
| `kobject_put` | func | `linux/kobject` | - | - | - | 8 | 17 | 17 | - |
| `__ATTR_RO` | macro | `linux/sysfs` | - | - | - | 2 | 18 | 12 | - |
| `module_param_named_unsafe` | macro | `linux/moduleparam` | - | - | - | 2 | 6 | 9 | - |
| `struct kobj_type` | type | `linux/kobject` | - | - | - | 2 | 4 | 10 | - |
| `module_param` | macro | `linux/moduleparam` | 2 | - | - | - | - | 8 | 4 |
| `sysfs_create_files` | func | `linux/sysfs` | - | - | - | 7 | 5 | 2 | - |
| `sysfs_create_file` | inline | `linux/sysfs` | - | - | - | 8 | 1 | 4 | - |

### K5 — PCI, IRQ, energia (425 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `to_pci_dev` | macro | `linux/pci` | 1 | - | 1 | 63 | 66 | 4 | 21 |
| `aperture_remove_conflicting_pci_devices` | func | `linux/aperture` | 1 | - | 1 | 1 | 1 | 1 | 1 |
| `pci_get_drvdata` | inline | `linux/pci` | 2 | - | - | 3 | 2 | 15 | 25 |
| `pci_resource_len` | macro | `linux/pci` | 2 | - | - | 10 | 7 | 13 | 4 |
| `pci_resource_start` | macro | `linux/pci` | 2 | - | - | 7 | 5 | 11 | 3 |
| `pci_dev_put` | macro | `linux/pci` | - | - | 2 | 3 | 5 | 7 | 3 |
| `pci_set_drvdata` | inline | `linux/pci` | 1 | - | - | 2 | 1 | 2 | 5 |
| `struct dev_pm_ops` | type | `linux/pm` | 1 | - | - | 2 | 1 | 1 | 3 |
| `struct pci_driver` | type | `linux/pci` | 1 | - | - | 1 | 2 | 1 | 2 |
| `irqreturn_t` | type | `linux/irqreturn` | - | - | - | 14 | 4 | 2 | 20 |
| `IRQ_NONE` | enumconst | `linux/irqreturn` | - | - | - | 21 | 6 | 1 | 4 |
| `PCI_FUNC` | macro | `linux/pci` | - | - | - | 1 | 1 | 1 | 29 |
| `pci_read_config_dword` | func | `linux/pci` | - | - | - | 10 | 1 | 6 | 13 |
| `IRQ_HANDLED` | enumconst | `linux/irqreturn` | - | - | - | 17 | 5 | 2 | 2 |
| `pci_read_config_word` | func | `linux/pci` | - | - | - | 11 | 1 | 3 | 9 |
| `pci_set_master` | func | `linux/pci` | - | - | - | 2 | 3 | 16 | 2 |
| `PCI_COMMAND` | macro | `linux/pci_regs` | - | - | - | 3 | 4 | 4 | 6 |
| `free_irq` | func | `linux/interrupt` | - | - | - | 1 | 2 | 1 | 12 |
| `pci_disable_device` | func | `linux/pci` | - | - | - | 3 | 2 | 6 | 3 |
| `pci_domain_nr` | inline | `linux/pci` | - | - | - | 2 | 4 | 6 | 2 |
| `pci_write_config_dword` | func | `linux/pci` | - | - | - | 8 | 2 | 3 | 1 |
| `pci_get_domain_bus_and_slot` | func | `linux/pci` | - | - | - | 1 | 5 | 4 | 1 |
| `pci_resource_flags` | macro | `linux/pci` | - | - | - | 2 | 2 | 2 | 4 |
| `pm_runtime_get_noresume` | inline | `linux/pm_runtime` | - | - | - | 1 | 4 | 3 | 2 |
| `pci_enable_device` | func | `linux/pci` | - | - | - | 2 | 2 | 3 | 2 |
| `pci_save_state` | func | `linux/pci` | - | - | - | 1 | 2 | 3 | 2 |
| `pm_runtime_suspended` | inline | `linux/pm_runtime` | - | - | - | 2 | 2 | 1 | 2 |
| `struct pci_bus` | type | `linux/pci` | - | - | - | 1 | 1 | 1 | 4 |
| `PCI_COMMAND_MEMORY` | macro | `linux/pci_regs` | - | - | - | 1 | 2 | 1 | 2 |
| `pci_rebar_bytes_to_size` | inline | `linux/pci` | - | - | - | 2 | 2 | 1 | 1 |
| `pci_release_resource` | func | `linux/pci` | - | - | - | 1 | 1 | 2 | 2 |
| `pci_unregister_driver` | func | `linux/pci` | - | - | - | 1 | 1 | 1 | 3 |
| `DPM_FLAG_NO_DIRECT_COMPLETE` | macro | `linux/pm` | - | - | - | 1 | 1 | 1 | 2 |
| `pci_register_driver` | macro | `linux/pci` | - | - | - | 1 | 1 | 1 | 2 |
| `pci_resize_resource` | func | `linux/pci` | - | - | - | 1 | 1 | 1 | 2 |
| `pci_assign_unassigned_bus_resources` | func | `linux/pci` | - | - | - | 1 | 1 | 1 | 1 |
| `pci_rebar_get_possible_sizes` | func | `linux/pci` | - | - | - | 1 | 1 | 1 | 1 |
| `pm_runtime_allow` | func | `linux/pm_runtime` | - | - | - | 1 | 1 | 1 | 1 |
| `pm_runtime_set_autosuspend_delay` | func | `linux/pm_runtime` | - | - | - | 1 | 1 | 1 | 1 |
| `pm_runtime_mark_last_busy` | inline | `linux/pm_runtime` | - | - | - | 2 | 2 | 47 | - |

### K6 — Firmware, I2C, ACPI, backlight, debugfs... (501 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `seq_printf` | func | `linux/seq_file` | - | - | 4 | 267 | 1 | 149 | 58 |
| `seq_puts` | inline | `linux/seq_file` | - | - | 1 | 53 | - | 29 | 2 |
| `s64` | type | `acpi/actypes` | - | 1 | - | 24 | 26 | 17 | - |
| `s32` | type | `acpi/actypes` | - | - | - | 11 | 15 | 12 | 4 |
| `release_firmware` | func | `linux/firmware` | - | - | - | 5 | 5 | 7 | 2 |
| `struct va_format` | type | `linux/printk` | - | - | - | 3 | 10 | 2 | 1 |
| `request_firmware` | func | `linux/firmware` | - | - | - | 3 | 1 | 3 | 4 |
| `debug` | macro | `video/trident` | - | - | - | 102 | 2 | 1270 | - |
| `output` | macro | `video/trident` | - | - | 58 | - | - | 809 | 38 |
| `debugfs_create_file` | macro | `linux/debugfs` | - | - | - | 53 | 13 | 59 | - |
| `I2C_M_RD` | macro | `linux/i2c` | - | - | - | 14 | - | 45 | 4 |
| `union acpi_object` | type | `acpi/actypes` | - | - | - | 5 | - | 27 | 23 |
| `DEFINE_SHOW_ATTRIBUTE` | macro | `linux/seq_file` | - | - | - | 22 | 1 | 28 | - |
| `acpi_handle` | type | `acpi/actypes` | - | - | - | 7 | - | 13 | 31 |
| `struct i2c_msg` | type | `linux/i2c` | - | - | - | 25 | - | 20 | 3 |
| `single_release` | func | `linux/seq_file` | - | - | - | 25 | - | 7 | 15 |
| `DEFINE_DEBUGFS_ATTRIBUTE` | macro | `linux/debugfs` | - | - | - | 7 | 5 | 34 | - |
| `seq_read` | func | `linux/seq_file` | - | - | - | 25 | - | 7 | 9 |
| `debugfs_create_file_unsafe` | func | `linux/debugfs` | - | - | - | 6 | 11 | 20 | - |
| `single_open` | func | `linux/seq_file` | - | - | - | 20 | - | 7 | 10 |
| `acpi_status` | type | `acpi/actypes` | - | - | - | 1 | - | 14 | 16 |
| `seq_lseek` | func | `linux/seq_file` | - | - | - | 15 | - | 7 | 9 |
| `struct acpi_buffer` | type | `acpi/actypes` | - | - | - | 1 | - | 18 | 9 |
| `ACPI_FAILURE` | macro | `acpi/acexcep` | - | - | - | 1 | - | 8 | 15 |
| `i2c_transfer` | func | `linux/i2c` | - | - | - | 17 | - | 6 | 1 |
| `struct backlight_device` | type | `linux/backlight` | - | - | - | 4 | - | 10 | 6 |
| `debugfs_create_dir` | func | `linux/debugfs` | - | - | - | 5 | 8 | 3 | - |
| `struct firmware` | type | `linux/firmware` | - | - | - | 8 | 6 | - | 2 |
| `ACPI_HANDLE` | macro | `linux/acpi` | - | - | - | 2 | - | 5 | 8 |
| `ACPI_TYPE_BUFFER` | macro | `acpi/actypes` | - | - | - | 1 | - | 4 | 9 |
| `struct i2c_algorithm` | type | `linux/i2c` | - | - | - | 2 | - | 10 | 2 |
| `I2C_FUNC_I2C` | macro | `linux/i2c` | - | - | - | 1 | - | 10 | 2 |
| `KERN_DEBUG` | macro | `linux/kern_levels` | - | - | - | 3 | - | 3 | 3 |
| `bl_get_data` | inline | `linux/backlight` | - | - | - | 2 | - | 5 | 2 |
| `struct acpi_device` | type | `acpi/acpi_bus` | - | - | - | 2 | - | 6 | 1 |
| `ACPI_TYPE_PACKAGE` | macro | `acpi/actypes` | - | - | - | 2 | - | 1 | 5 |
| `DEFINE_SHOW_STORE_ATTRIBUTE` | macro | `linux/seq_file` | - | - | - | 3 | 3 | 1 | - |
| `LOCKDEP_STILL_OK` | enumconst | `linux/panic` | - | - | - | 3 | 2 | 2 | - |
| `add_taint` | func | `linux/panic` | - | - | - | 3 | 2 | 2 | - |
| `component_del` | func | `linux/component` | - | - | - | 4 | 2 | 1 | - |

### K7 — Fences/sync (103 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `dma_fence_put` | inline | `linux/dma-fence` | - | - | 13 | 44 | 60 | 172 | 16 |
| `dma_fence_get` | inline | `linux/dma-fence` | - | - | 1 | 11 | 29 | 53 | 2 |
| `dma_resv_add_fence` | func | `linux/dma-resv` | - | - | 1 | 5 | 11 | 15 | 2 |
| `dma_resv_unlock` | inline | `linux/dma-resv` | - | - | 8 | 6 | 7 | 10 | 1 |
| `dma_resv_reserve_fences` | func | `linux/dma-resv` | - | - | 2 | 7 | 6 | 12 | 1 |
| `struct dma_fence_ops` | type | `linux/dma-fence` | - | - | 1 | 5 | 4 | 10 | 2 |
| `drm_syncobj_put` | inline | `drm/drm_syncobj` | - | - | 2 | 8 | 4 | 5 | 2 |
| `dma_fence_context_alloc` | func | `linux/dma-fence` | - | - | 2 | 3 | 4 | 6 | 2 |
| `dma_fence_init` | func | `linux/dma-fence` | - | - | 1 | 4 | 4 | 3 | 2 |
| `dma_resv_lock` | inline | `linux/dma-resv` | - | - | 5 | 2 | 3 | 3 | 1 |
| `drm_syncobj_find` | func | `drm/drm_syncobj` | - | - | 2 | 2 | 2 | 4 | 2 |
| `dma_fence_chain_alloc` | macro | `linux/dma-fence-chain` | - | - | 1 | 2 | 3 | 2 | 1 |
| `drm_syncobj_add_point` | func | `drm/drm_syncobj` | - | - | 1 | 1 | 2 | 2 | 1 |
| `dma_fence_wait` | inline | `linux/dma-fence` | - | - | 3 | 11 | 17 | 30 | - |
| `dma_fence_wait_timeout` | func | `linux/dma-fence` | - | - | 1 | 3 | 7 | 33 | - |
| `struct dma_fence_cb` | type | `linux/dma-fence` | - | - | - | 20 | 8 | 8 | 4 |
| `DMA_RESV_USAGE_KERNEL` | enumconst | `linux/dma-resv` | - | - | 1 | 3 | 24 | 7 | - |
| `dma_resv_wait_timeout` | func | `linux/dma-resv` | - | - | 4 | 2 | 14 | 5 | - |
| `dma_fence_set_error` | inline | `linux/dma-fence` | - | - | - | 1 | 4 | 14 | 2 |
| `dma_fence_signal` | func | `linux/dma-fence` | - | - | - | 4 | 3 | 10 | 4 |
| `dma_resv_assert_held` | macro | `linux/dma-resv` | - | - | 2 | 3 | 5 | 11 | - |
| `struct drm_syncobj` | type | `drm/drm_syncobj` | - | - | 5 | 5 | - | 5 | 2 |
| `dma_fence_add_callback` | func | `linux/dma-fence` | - | - | - | 6 | 4 | 4 | 2 |
| `DMA_RESV_USAGE_READ` | enumconst | `linux/dma-resv` | - | - | 3 | 4 | - | 7 | 1 |
| `DMA_RESV_USAGE_WRITE` | enumconst | `linux/dma-resv` | - | - | 2 | 3 | - | 6 | 1 |
| `dma_resv_test_signaled` | func | `linux/dma-resv` | - | - | 1 | 2 | 7 | 1 | - |
| `struct sync_file` | type | `linux/sync_file` | - | - | 1 | 7 | - | 1 | 2 |
| `struct dma_fence_array` | type | `linux/dma-fence-array` | - | - | - | 5 | 3 | 1 | 1 |
| `struct dma_fence_chain` | type | `linux/dma-fence-chain` | - | - | 1 | 3 | - | 3 | 2 |
| `drm_syncobj_replace_fence` | func | `drm/drm_syncobj` | - | - | 2 | 2 | 1 | 3 | - |
| `sync_file_create` | func | `linux/sync_file` | - | - | 1 | 2 | - | 1 | 2 |
| `DMA_RESV_USAGE_BOOKKEEP` | enumconst | `linux/dma-resv` | - | - | - | 2 | 32 | 20 | - |
| `dma_fence_is_signaled` | inline | `linux/dma-fence` | - | - | - | 11 | 8 | 33 | - |
| `struct dma_resv_iter` | type | `linux/dma-resv` | - | - | - | 7 | 3 | 11 | - |
| `dma_resv_for_each_fence` | macro | `linux/dma-resv` | - | - | - | 2 | 1 | 9 | - |
| `struct dma_resv` | type | `linux/dma-resv` | - | - | 3 | - | 8 | - | 1 |
| `dma_resv_iter_end` | inline | `linux/dma-resv` | - | - | - | 5 | 2 | 3 | - |
| `dma_resv_trylock` | inline | `linux/dma-resv` | - | - | - | 1 | 4 | 5 | - |
| `dma_resv_for_each_fence_unlocked` | macro | `linux/dma-resv` | - | - | - | 5 | 2 | 2 | - |
| `dma_resv_iter_begin` | inline | `linux/dma-resv` | - | - | - | 5 | 2 | 2 | - |

### K8 — Entorno de usuario / VFS (137 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `bytes` | macro | `linux/netfilter/xt_sctp` | - | - | - | 49 | 2 | 128 | 11 |
| `struct file_operations` | type | `linux/fs` | - | - | - | 37 | 10 | 67 | 11 |
| `struct inode` | type | `linux/fs` | - | - | - | 32 | 4 | 16 | 52 |
| `file_inode` | inline | `linux/fs` | - | - | - | 4 | 6 | 69 | 5 |
| `capable` | func | `linux/capability` | - | - | - | 25 | 11 | 11 | 3 |
| `CAP_SYS_NICE` | macro | `linux/capability` | - | - | - | 1 | 3 | 2 | 1 |
| `PIDTYPE_PID` | enumconst | `linux/pid_types` | - | - | - | 2 | 1 | 2 | 2 |
| `get_unused_fd_flags` | func | `linux/file` | - | - | 1 | 1 | - | 1 | 3 |
| `FOP_UNSIGNED_OFFSET` | macro | `linux/fs` | - | - | - | 1 | 1 | 1 | 2 |
| `put_unused_fd` | func | `linux/file` | - | - | 1 | 1 | - | 1 | 1 |
| `struct file` | type | `linux/fs` | - | - | - | - | 33 | 110 | 89 |
| `S_IRUGO` | macro | `linux/stat` | - | - | - | 2 | - | 74 | 6 |
| `default_llseek` | func | `linux/fs` | - | - | - | 13 | 3 | 53 | - |
| `S_IWUSR` | macro | `linux/stat` | - | - | - | 4 | - | 22 | 1 |
| `fput` | func | `linux/file` | - | - | 1 | 10 | - | - | 11 |
| `S_IRUSR` | macro | `linux/stat` | - | - | - | 4 | - | 9 | 1 |
| `poll_table` | type | `linux/poll` | - | - | - | 4 | 4 | - | 5 |
| `poll_wait` | inline | `linux/poll` | - | - | - | 4 | 2 | - | 3 |
| `task_pid_nr` | inline | `linux/pid` | - | - | - | 1 | - | 3 | 3 |
| `get_random_bytes` | func | `linux/random` | - | - | - | 2 | 1 | - | 3 |
| `get_task_pid` | func | `linux/pid` | - | - | - | 1 | - | 1 | 2 |
| `noop_llseek` | func | `linux/fs` | - | - | - | 1 | 2 | - | 1 |
| `put_pid` | func | `linux/pid` | - | - | - | 1 | - | 2 | 1 |
| `pid_task` | func | `linux/pid` | - | - | - | 1 | - | 1 | 1 |
| `simple_open` | func | `linux/fs` | - | - | - | 1 | 1 | 1 | - |
| `block_size` | func | `linux/blkdev` | - | - | - | 28 | - | 78 | - |
| `d_inode` | inline | `linux/dcache` | - | - | - | - | 13 | 2 | - |
| `struct address_space` | type | `linux/fs` | - | - | - | 11 | - | - | 1 |
| `struct workqueue_struct` | type | `linux/fs` | - | - | - | 8 | - | 4 | - |
| `_RET_IP_` | macro | `linux/instruction_pointer` | - | - | - | 6 | - | - | 4 |
| `perfmon_capable` | inline | `linux/capability` | - | - | - | 5 | 5 | - | - |
| `S_IWUGO` | macro | `linux/stat` | - | - | - | - | - | 8 | 1 |
| `simple_read_from_buffer` | func | `linux/fs` | - | - | - | - | 6 | 3 | - |
| `S_IFREG` | macro | `linux/stat` | - | - | - | - | - | 6 | 2 |
| `_THIS_IP_` | macro | `linux/instruction_pointer` | - | - | - | 4 | 3 | - | - |
| `CAP_SYS_ADMIN` | macro | `linux/capability` | - | - | - | 5 | - | - | 1 |
| `pid_nr` | inline | `linux/pid` | - | - | - | 2 | - | 4 | - |
| `struct pid` | type | `linux/pid` | - | - | - | - | - | 2 | 2 |
| `EPOLLIN` | macro | `linux/eventpoll` | - | - | - | 1 | 2 | - | - |
| `SYSCTL_ZERO` | macro | `linux/sysctl` | - | - | - | 2 | 1 | - | - |

### K9 — Miscelaneo (stubs: trace, kasan, hmm...) (193 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `u32` | type | `linux/raid/pq` | 2 | 23 | 36 | 5474 | 2916 | 4167 | 68 |
| `u64` | type | `linux/raid/pq` | 2 | 6 | 12 | 1008 | 1073 | 1128 | 23 |
| `u16` | type | `linux/raid/pq` | 11 | 1 | - | 867 | 283 | 368 | 35 |
| `__entry` | macro | `trace/stages/stage3_trace_output` | - | - | 16 | 440 | 161 | 521 | 10 |
| `PAGE_SIZE` | macro | `linux/raid/pq` | - | - | 5 | 196 | 64 | 329 | 71 |
| `__field` | macro | `linux/genl_magic_func` | - | - | 8 | 197 | 75 | 256 | 2 |
| `PAGE_SHIFT` | macro | `linux/raid/pq` | - | - | 2 | 151 | 48 | 80 | 30 |
| `TP_ARGS` | macro | `linux/tracepoint` | - | - | 3 | 74 | 87 | 50 | 2 |
| `TP_PROTO` | macro | `linux/tracepoint` | - | - | 3 | 74 | 87 | 50 | 2 |
| `TP_printk` | macro | `trace/stages/stage3_trace_output` | - | - | 1 | 47 | 18 | 38 | 2 |
| `TP_STRUCT__entry` | macro | `trace/stages/stage1_struct_define` | - | - | 1 | 47 | 16 | 38 | 2 |
| `TP_fast_assign` | macro | `trace/stages/stage6_event_callback` | - | - | 1 | 47 | 16 | 38 | 2 |
| `HZ` | macro | `linux/raid/pq` | - | - | 5 | 23 | 24 | 30 | 8 |
| `__assign_str` | macro | `trace/stages/stage6_event_callback` | - | - | 1 | 35 | 19 | 9 | 8 |
| `__get_str` | macro | `trace/stages/stage3_trace_output` | - | - | 1 | 35 | 21 | 9 | 4 |
| `__string` | macro | `trace/stages/stage1_struct_define` | - | - | 1 | 35 | 19 | 9 | 4 |
| `struct kmem_cache` | type | `linux/fault-inject` | - | - | 1 | 12 | 3 | 3 | 3 |
| `TRACE_HEADER_MULTI_READ` | macro | `trace/define_trace` | - | - | 1 | 4 | 4 | 2 | 2 |
| `DEFINE_EVENT` | macro | `linux/tracepoint` | - | - | 2 | 27 | 69 | 12 | - |
| `TRACE_EVENT` | macro | `linux/tracepoint` | - | - | - | 41 | 5 | 34 | 2 |
| `cleanup` | type | `net/flow_offload` | - | - | 6 | 20 | - | 28 | 13 |
| `DECLARE_EVENT_CLASS` | macro | `linux/tracepoint` | - | - | 1 | 5 | 11 | 4 | - |
| `struct perf_event` | type | `linux/perf_event` | - | - | - | 17 | 16 | 6 | - |
| `after` | macro | `net/tcp` | - | - | - | 8 | 3 | 20 | - |
| `before` | inline | `net/tcp` | - | - | - | 6 | 3 | 19 | - |
| `BITS_PER_LONG` | macro | `linux/raid/pq` | - | - | - | 16 | 3 | 2 | - |
| `struct mmu_interval_notifier` | type | `linux/mmu_notifier` | - | - | - | 2 | 2 | 8 | - |
| `PERF_HES_STOPPED` | macro | `linux/perf_event` | - | - | - | 2 | 2 | 4 | - |
| `struct hw_perf_event` | type | `linux/perf_event` | - | - | - | 1 | 1 | 6 | - |
| `struct mmu_notifier_range` | type | `linux/mmu_notifier` | - | - | - | 1 | 5 | 2 | - |
| `PERF_EF_UPDATE` | macro | `linux/perf_event` | - | - | - | 2 | 2 | 1 | - |
| `mmu_interval_read_retry` | inline | `linux/mmu_notifier` | - | - | - | 2 | 2 | 1 | - |
| `outb` | macro | `linux/logic_pio` | 1 | - | - | 3 | - | - | 1 |
| `inb` | macro | `linux/logic_pio` | 1 | - | - | 2 | - | - | 1 |
| `mmu_interval_notifier_insert` | func | `linux/mmu_notifier` | - | - | - | 1 | 1 | 2 | - |
| `mmu_interval_set_seq` | inline | `linux/mmu_notifier` | - | - | - | 1 | 1 | 2 | - |
| `struct mmu_interval_notifier_ops` | type | `linux/mmu_notifier` | - | - | - | 1 | 1 | 2 | - |
| `struct pmu` | type | `linux/perf_event` | - | - | - | 1 | 1 | 2 | - |
| `PERF_EF_START` | macro | `linux/perf_event` | - | - | - | 1 | 1 | 1 | - |
| `mmu_interval_notifier_remove` | func | `linux/mmu_notifier` | - | - | - | 1 | 1 | 1 | - |

### K9-drm — DRM core y helpers (shim propio con la opcion A) (2325 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `DRIVER_MODESET` | enumconst | `drm/drm_drv` | 1 | 1 | 2 | 2 | 2 | 1 | 18 |
| `DRIVER_ATOMIC` | enumconst | `drm/drm_drv` | 1 | 1 | 3 | 3 | 2 | 3 | 1 |
| `drm_dev_unplug` | func | `drm/drm_drv` | 1 | 1 | 2 | 1 | 1 | 3 | 1 |
| `DRIVER_GEM` | enumconst | `drm/drm_drv` | 1 | 1 | 1 | 1 | 1 | 3 | 1 |
| `drm_dev_register` | func | `drm/drm_drv` | 1 | 1 | 1 | 1 | 1 | 2 | 1 |
| `drm_mode_config_reset` | func | `drm/drm_mode_config` | 1 | 1 | 1 | 1 | 2 | 1 | 1 |
| `struct drm_plane_state` | type | `drm/drm_plane` | 3 | 4 | 10 | 13 | - | 57 | 25 |
| `struct drm_framebuffer` | type | `drm/drm_framebuffer` | 1 | 2 | 4 | - | 8 | 39 | 9 |
| `drm_dev_exit` | func | `drm/drm_drv` | 3 | 2 | 2 | 1 | 7 | 41 | - |
| `DRM_FORMAT_XRGB8888` | macro | `drm/drm_fourcc` | 3 | 2 | 1 | 38 | - | 9 | 1 |
| `drm_dev_enter` | func | `drm/drm_drv` | 3 | 2 | 2 | 1 | 7 | 35 | - |
| `drm_atomic_get_new_crtc_state` | inline | `drm/drm_atomic` | 2 | 4 | 1 | 8 | - | 15 | 2 |
| `drm_connector_helper_add` | inline | `drm/drm_modeset_helper_vtables` | 1 | 1 | 1 | 10 | - | 14 | 1 |
| `struct drm_connector_helper_funcs` | type | `drm/drm_modeset_helper_vtables` | 1 | 1 | 1 | 10 | - | 12 | 1 |
| `drm_helper_probe_single_connector_modes` | func | `drm/drm_probe_helper` | 1 | 1 | 1 | 10 | - | 9 | 1 |
| `struct drm_connector_funcs` | type | `drm/drm_connector` | 1 | 1 | 1 | 10 | - | 9 | 1 |
| `DRM_PLANE_TYPE_PRIMARY` | enumconst | `drm/drm_plane` | 1 | 1 | 3 | 7 | - | 6 | 3 |
| `drm_connector_cleanup` | func | `drm/drm_connector` | 1 | 1 | 1 | 6 | - | 7 | 2 |
| `struct drm_plane_funcs` | type | `drm/drm_plane` | 1 | 1 | 1 | 12 | - | 2 | 1 |
| `struct drm_crtc_funcs` | type | `drm/drm_crtc` | 1 | 1 | 1 | 8 | - | 5 | 1 |
| `struct drm_driver` | type | `drm/drm_drv` | 1 | 2 | 2 | 2 | - | 8 | 1 |
| `drm_atomic_helper_disable_plane` | func | `drm/drm_atomic_helper` | 1 | 1 | 1 | 9 | - | 2 | 1 |
| `drm_atomic_helper_update_plane` | func | `drm/drm_atomic_helper` | 1 | 1 | 1 | 9 | - | 2 | 1 |
| `drm_atomic_get_new_plane_state` | inline | `drm/drm_atomic` | 1 | 3 | 1 | 1 | - | 7 | 1 |
| `struct drm_plane_helper_funcs` | type | `drm/drm_modeset_helper_vtables` | 1 | 1 | 3 | 2 | - | 6 | 1 |
| `drm_plane_helper_add` | inline | `drm/drm_modeset_helper_vtables` | 1 | 1 | 1 | 2 | - | 6 | 1 |
| `drm_crtc_cleanup` | func | `drm/drm_crtc` | 1 | 1 | 1 | 1 | - | 6 | 1 |
| `drm_connector_attach_encoder` | func | `drm/drm_connector` | 1 | 1 | 1 | 2 | - | 4 | 1 |
| `struct drm_mode_config_funcs` | type | `drm/drm_mode_config` | 1 | 1 | 1 | 1 | - | 4 | 1 |
| `drm_connector_init` | func | `drm/drm_connector` | 1 | 1 | 1 | 3 | - | 1 | 1 |
| `drm_atomic_helper_page_flip` | func | `drm/drm_atomic_helper` | 1 | 1 | 1 | 1 | - | 2 | 1 |
| `drm_atomic_helper_set_config` | func | `drm/drm_atomic_helper` | 1 | 1 | 1 | 1 | - | 2 | 1 |
| `drm_crtc_init_with_planes` | func | `drm/drm_crtc` | 1 | 1 | 1 | 1 | - | 2 | 1 |
| `drm_err` | macro | `drm/drm_print` | 1 | 11 | - | 478 | 90 | 195 | - |
| `drm_dbg` | macro | `drm/drm_print` | 1 | 7 | - | 336 | 122 | 53 | - |
| `DRM_DEBUG` | macro | `drm/drm_print` | 1 | - | 10 | 20 | - | 390 | 1 |
| `struct drm_connector` | type | `drm/drm_connector` | 4 | 4 | 6 | - | - | 247 | 38 |
| `struct drm_encoder` | type | `drm/drm_encoder` | 2 | 2 | 5 | - | - | 183 | 20 |
| `DRM_INFO` | macro | `drm/drm_print` | 2 | - | 8 | 5 | - | 132 | 1 |
| `struct drm_plane` | type | `drm/drm_plane` | 5 | 10 | 16 | - | - | 56 | 46 |

### K9-ttm — TTM, drm_buddy, drm_exec, gpuvm (190 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `TTM_PL_VRAM` | macro | `drm/ttm/ttm_placement` | - | - | - | 1 | 1 | 51 | - |
| `ttm_bo_type_kernel` | enumconst | `drm/ttm/ttm_bo` | - | - | - | 2 | 28 | 23 | - |
| `TTM_PL_TT` | macro | `drm/ttm/ttm_placement` | - | - | - | 1 | 1 | 40 | - |
| `ttm_bo_validate` | func | `drm/ttm/ttm_bo` | - | - | - | 7 | 5 | 28 | - |
| `ttm_manager_type` | inline | `drm/ttm/ttm_device` | - | - | - | 5 | 21 | 11 | - |
| `TTM_PL_SYSTEM` | macro | `drm/ttm/ttm_placement` | - | - | - | 4 | 1 | 25 | - |
| `TTM_PL_FLAG_CONTIGUOUS` | macro | `drm/ttm/ttm_placement` | - | - | - | 3 | 11 | 13 | - |
| `struct ttm_placement` | type | `drm/ttm/ttm_placement` | - | - | - | 11 | 11 | 5 | - |
| `enum ttm_bo_type` | type | `drm/ttm/ttm_bo` | - | - | - | 1 | 15 | 4 | - |
| `ttm_bo_type_device` | enumconst | `drm/ttm/ttm_bo` | - | - | - | 1 | 8 | 8 | - |
| `ttm_bo_move_null` | inline | `drm/ttm/ttm_bo` | - | - | - | 2 | 10 | 3 | - |
| `TTM_TT_FLAG_EXTERNAL` | macro | `drm/ttm/ttm_tt` | - | - | - | 2 | 6 | 4 | - |
| `ttm_cached` | enumconst | `drm/ttm/ttm_caching` | - | - | - | 4 | 3 | 5 | - |
| `ttm_resource_manager_set_used` | inline | `drm/ttm/ttm_resource` | - | - | - | 2 | 4 | 6 | - |
| `ttm_set_driver_manager` | inline | `drm/ttm/ttm_device` | - | - | - | 2 | 4 | 6 | - |
| `ttm_resource_fini` | func | `drm/ttm/ttm_resource` | - | - | - | 2 | 4 | 5 | - |
| `ttm_resource_manager_evict_all` | func | `drm/ttm/ttm_resource` | - | - | - | 1 | 4 | 4 | - |
| `ttm_write_combined` | enumconst | `drm/ttm/ttm_caching` | - | - | - | 2 | 5 | 2 | - |
| `TTM_PL_FLAG_TOPDOWN` | macro | `drm/ttm/ttm_placement` | - | - | - | 3 | 2 | 2 | - |
| `ttm_resource_manager_cleanup` | inline | `drm/ttm/ttm_resource` | - | - | - | 2 | 2 | 3 | - |
| `struct ttm_range_mgr_node` | type | `drm/ttm/ttm_range_manager` | - | - | - | 1 | 1 | 4 | - |
| `struct ttm_resource_manager_func` | type | `drm/ttm/ttm_resource` | - | - | - | 1 | 2 | 3 | - |
| `ttm_bo_wait_ctx` | func | `drm/ttm/ttm_bo` | - | - | - | 3 | 1 | 2 | - |
| `ttm_resource_init` | func | `drm/ttm/ttm_resource` | - | - | - | 1 | 2 | 3 | - |
| `ttm_resource_manager_init` | func | `drm/ttm/ttm_resource` | - | - | - | 1 | 2 | 3 | - |
| `TTM_PL_FLAG_TEMPORARY` | macro | `drm/ttm/ttm_placement` | - | - | - | 1 | 2 | 2 | - |
| `TTM_PL_FLAG_DESIRED` | macro | `drm/ttm/ttm_placement` | - | - | - | 1 | 1 | 2 | - |
| `TTM_PL_FLAG_FALLBACK` | macro | `drm/ttm/ttm_placement` | - | - | - | 1 | 2 | 1 | - |
| `enum ttm_caching` | type | `drm/ttm/ttm_caching` | - | - | - | 2 | 1 | 1 | - |
| `ttm_bo_eviction_valuable` | func | `drm/ttm/ttm_bo` | - | - | - | 1 | 1 | 2 | - |
| `ttm_bo_move_accel_cleanup` | func | `drm/ttm/ttm_bo` | - | - | - | 1 | 1 | 2 | - |
| `ttm_tt_fini` | func | `drm/ttm/ttm_tt` | - | - | - | 1 | 2 | 1 | - |
| `TTM_BO_VM_NUM_PREFAULT` | macro | `drm/ttm/ttm_bo` | - | - | - | 1 | 1 | 1 | - |
| `drm_buddy_print` | func | `drm/drm_buddy` | - | - | - | 1 | 1 | 1 | - |
| `ttm_bo_init_reserved` | func | `drm/ttm/ttm_bo` | - | - | - | 1 | 1 | 1 | - |
| `ttm_bo_put` | func | `drm/ttm/ttm_bo` | - | - | - | 1 | 1 | 1 | - |
| `ttm_bo_vm_access` | func | `drm/ttm/ttm_bo` | - | - | - | 1 | 1 | 1 | - |
| `ttm_bo_vm_dummy_page` | func | `drm/ttm/ttm_bo` | - | - | - | 1 | 1 | 1 | - |
| `ttm_bo_vm_fault_reserved` | func | `drm/ttm/ttm_bo` | - | - | - | 1 | 1 | 1 | - |
| `ttm_device_fini` | func | `drm/ttm/ttm_device` | - | - | - | 1 | 1 | 1 | - |

### K9-sched — drm/gpu_scheduler (46 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `struct drm_gpu_scheduler` | type | `drm/gpu_scheduler` | - | - | - | - | 6 | 22 | - |
| `struct drm_sched_job` | type | `drm/gpu_scheduler` | - | - | - | - | 11 | 11 | - |
| `drm_sched_entity_init` | func | `drm/gpu_scheduler` | - | - | - | - | 3 | 8 | - |
| `struct drm_sched_fence` | type | `drm/gpu_scheduler` | - | - | - | - | 1 | 9 | - |
| `drm_sched_wqueue_stop` | func | `drm/gpu_scheduler` | - | - | - | - | 2 | 7 | - |
| `drm_sched_job_add_dependency` | func | `drm/gpu_scheduler` | - | - | - | - | 5 | 3 | - |
| `drm_sched_wqueue_start` | func | `drm/gpu_scheduler` | - | - | - | - | 1 | 7 | - |
| `struct drm_sched_backend_ops` | type | `drm/gpu_scheduler` | - | - | - | - | 5 | 2 | - |
| `drm_sched_fini` | func | `drm/gpu_scheduler` | - | - | - | - | 5 | 1 | - |
| `drm_sched_job_cleanup` | func | `drm/gpu_scheduler` | - | - | - | - | 3 | 3 | - |
| `drm_sched_entity_fini` | func | `drm/gpu_scheduler` | - | - | - | - | 3 | 2 | - |
| `drm_sched_entity_push_job` | func | `drm/gpu_scheduler` | - | - | - | - | 2 | 2 | - |
| `drm_sched_init` | func | `drm/gpu_scheduler` | - | - | - | - | 3 | 1 | - |
| `drm_sched_job_arm` | func | `drm/gpu_scheduler` | - | - | - | - | 2 | 2 | - |
| `struct drm_sched_init_args` | type | `drm/gpu_scheduler` | - | - | - | - | 3 | 1 | - |
| `drm_sched_job_add_resv_dependencies` | func | `drm/gpu_scheduler` | - | - | - | - | 1 | 2 | - |
| `drm_sched_job_init` | func | `drm/gpu_scheduler` | - | - | - | - | 2 | 1 | - |
| `drm_sched_stop` | func | `drm/gpu_scheduler` | - | - | - | - | 1 | 2 | - |
| `DRM_GPU_SCHED_STAT_RESET` | enumconst | `drm/gpu_scheduler` | - | - | - | - | 1 | 1 | - |
| `enum drm_gpu_sched_stat` | type | `drm/gpu_scheduler` | - | - | - | - | 1 | 1 | - |
| `struct drm_sched_entity` | type | `drm/gpu_scheduler` | - | - | - | - | - | 34 | - |
| `drm_sched_fault` | func | `drm/gpu_scheduler` | - | - | - | - | - | 19 | - |
| `drm_sched_entity_destroy` | func | `drm/gpu_scheduler` | - | - | - | - | - | 10 | - |
| `DRM_SCHED_PRIORITY_NORMAL` | enumconst | `drm/gpu_scheduler` | - | - | - | - | - | 9 | - |
| `to_drm_sched_fence` | func | `drm/gpu_scheduler` | - | - | - | - | - | 6 | - |
| `drm_sched_entity_error` | func | `drm/gpu_scheduler` | - | - | - | - | - | 3 | - |
| `drm_sched_entity_flush` | func | `drm/gpu_scheduler` | - | - | - | - | - | 3 | - |
| `drm_sched_entity_modify_sched` | func | `drm/gpu_scheduler` | - | - | - | - | - | 3 | - |
| `DRM_GPU_SCHED_STAT_NO_HANG` | enumconst | `drm/gpu_scheduler` | - | - | - | - | 2 | - | - |
| `DRM_SCHED_PRIORITY_HIGH` | enumconst | `drm/gpu_scheduler` | - | - | - | - | - | 2 | - |
| `DRM_SCHED_PRIORITY_KERNEL` | enumconst | `drm/gpu_scheduler` | - | - | - | - | - | 2 | - |
| `DRM_SCHED_PRIORITY_LOW` | enumconst | `drm/gpu_scheduler` | - | - | - | - | - | 2 | - |
| `drm_sched_start` | func | `drm/gpu_scheduler` | - | - | - | - | - | 2 | - |
| `drm_sched_wqueue_ready` | func | `drm/gpu_scheduler` | - | - | - | - | - | 2 | - |
| `DRM_GPU_SCHED_STAT_ENODEV` | enumconst | `drm/gpu_scheduler` | - | - | - | - | - | 1 | - |
| `DRM_SCHED_FENCE_DONT_PIPELINE` | macro | `drm/gpu_scheduler` | - | - | - | - | - | 1 | - |
| `DRM_SCHED_PRIORITY_COUNT` | enumconst | `drm/gpu_scheduler` | - | - | - | - | - | 1 | - |
| `MAX_WAIT_SCHED_ENTITY_Q_EMPTY` | macro | `drm/gpu_scheduler` | - | - | - | - | - | 1 | - |
| `drm_sched_entity_set_priority` | func | `drm/gpu_scheduler` | - | - | - | - | - | 1 | - |
| `drm_sched_increase_karma` | func | `drm/gpu_scheduler` | - | - | - | - | - | 1 | - |

### K? — Sin clasificar (77 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `free` | macro | `linux/decompress/mm` | - | - | 3 | 21 | 14 | 14 | 15 |
| `mode_set` | inline | `linux/hisi_acc_qm` | - | - | 1 | 8 | - | 12 | 1 |
| `struct gpu_buddy_block` | type | `linux/gpu_buddy` | - | - | - | 5 | 16 | 24 | - |
| `gpu_buddy_block_offset` | inline | `linux/gpu_buddy` | - | - | - | 4 | 9 | 1 | - |
| `gpu_buddy_free_list` | func | `linux/gpu_buddy` | - | - | - | 3 | 1 | 3 | - |
| `GPU_BUDDY_RANGE_ALLOCATION` | macro | `linux/gpu_buddy` | - | - | - | 2 | 1 | 2 | - |
| `gpu_buddy_alloc_blocks` | func | `linux/gpu_buddy` | - | - | - | 2 | 1 | 2 | - |
| `GPU_BUDDY_TOPDOWN_ALLOCATION` | macro | `linux/gpu_buddy` | - | - | - | 1 | 1 | 1 | - |
| `gpu_buddy_fini` | func | `linux/gpu_buddy` | - | - | - | 1 | 1 | 1 | - |
| `gpu_buddy_init` | func | `linux/gpu_buddy` | - | - | - | 1 | 1 | 1 | - |
| `struct gpu_buddy` | type | `linux/gpu_buddy` | - | - | - | - | 12 | 6 | - |
| `gpu_buddy_block_size` | inline | `linux/gpu_buddy` | - | - | - | 4 | 11 | - | - |
| `__u32` | enumconst | `linux/android/binder` | - | - | - | - | - | 8 | 4 |
| `struct clk` | type | `linux/sh_clk` | - | 2 | - | - | - | - | 5 |
| `struct sysinfo` | type | `linux/sysinfo` | - | - | - | - | 2 | 5 | - |
| `struct gpio_desc` | type | `linux/synclink` | - | - | - | 4 | - | - | 1 |
| `GPU_BUDDY_CONTIGUOUS_ALLOCATION` | macro | `linux/gpu_buddy` | - | - | - | 1 | - | 2 | - |
| `gpu_buddy_block_trim` | func | `linux/gpu_buddy` | - | - | - | - | 1 | 1 | - |
| `dout` | macro | `linux/ceph/ceph_debug` | - | - | - | - | - | 147 | - |
| `sys_info` | func | `linux/sys_info` | - | - | - | - | - | 81 | - |
| `RESERVED` | enumconst | `linux/habanalabs/hl_boot_if` | - | - | - | - | - | 65 | - |
| `OFFSET` | macro | `linux/kbuild` | - | - | - | - | - | 45 | - |
| `MASK` | macro | `linux/soc/ti/knav_dma` | - | - | - | - | - | 19 | - |
| `struct config_item` | type | `linux/configfs` | - | - | - | - | 15 | - | - |
| `DW_IC_ENABLE_ENABLE` | macro | `linux/designware_i2c` | - | - | - | - | 6 | - | - |
| `config_group_put` | inline | `linux/configfs` | - | - | - | - | 6 | - | - |
| `CONFIGFS_ATTR` | macro | `linux/configfs` | - | - | - | - | 5 | - | - |
| `PSI` | enumconst | `linux/scmi_protocol` | - | - | - | - | - | 5 | - |
| `VFM_MODEL` | macro | `linux/peci-cpu` | - | - | - | - | - | 5 | - |
| `DW_IC_ENABLE` | macro | `linux/designware_i2c` | - | - | - | - | 4 | - | - |
| `struct reset_control` | type | `linux/mdio` | - | - | - | - | - | - | 4 |
| `DECLARE_CRYPTO_WAIT` | macro | `linux/crypto` | - | - | - | - | - | - | 3 |
| `crypto_wait_req` | inline | `linux/crypto` | - | - | - | - | - | - | 3 |
| `signalled` | macro | `linux/sunrpc/types` | - | - | - | - | 3 | - | - |
| `struct config_group` | type | `linux/configfs` | - | - | - | - | 3 | - | - |
| `struct key_preparsed_payload` | type | `linux/key-type` | - | - | - | - | - | - | 3 |
| `CRYPTO_ALG_TYPE_AEAD` | macro | `linux/crypto` | - | - | - | - | - | - | 2 |
| `CRYPTO_TFM_REQ_MAY_BACKLOG` | macro | `linux/crypto` | - | - | - | - | - | - | 2 |
| `CRYPTO_TFM_REQ_MAY_SLEEP` | macro | `linux/crypto` | - | - | - | - | - | - | 2 |
| `DW_IC_ENABLE_STATUS` | macro | `linux/designware_i2c` | - | - | - | - | 2 | - | - |

