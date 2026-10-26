# API de Linux que consumen los drivers objetivo (LinuxKPI, L0.3)

Generado por `scripts/linuxkpi-scan.py` (no editar a mano; la tabla completa esta en `linuxkpi_api.tsv`).

**Fuentes analizadas:** **PROVISIONAL** — Linux 6.8.0 de Ubuntu (`linux-source-6.8.0`, con parches de Ubuntu) y NVIDIA open 615.71.09 de Ubuntu (`nvidia-kernel-source-615-open`), obtenidos con `scripts/fetch-linux-drivers.sh --ubuntu` porque kernel.org/GitHub no eran accesibles desde el entorno. **No es la versión fijada (6.18)**: repetir con `scripts/linuxkpi-scan.py --linux build/linuxkpi_src/linux --nvidia build/linuxkpi_src/nvidia-open`.

Indice: 6503 cabeceras de Linux, 362780 nombres definidos. Un simbolo cuenta si un fichero del driver lo usa, lo define una cabecera de Linux y no lo define el propio driver. **Analisis lexico**: incluye todas las ramas `#if`, macros e *inlines* (que la capa tambien tiene que dar) y algo de ruido; no ve nombres creados por macros generadoras.

## Drivers

| Driver | Ficheros | Lineas | Simbolos externos distintos |
|---|---:|---:|---:|
| `bochs` | 1 | 739 | 135 |
| `simpledrm` | 1 | 1060 | 192 |
| `virtio-gpu` | 16 | 5761 | 546 |
| `i915` | 666 | 321670 | 3589 |
| `xe` | 303 | 57457 | 1136 |
| `amdgpu` | 1769 | 1041810 | 2694 |
| `nvidia` | 215 | 90146 | 1672 |

## Simbolos distintos por nivel y driver

| Nivel | `bochs` | `simpledrm` | `virtio-gpu` | `i915` | `xe` | `amdgpu` | `nvidia` | Union | de ellos funciones (no inline) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| K0 Base C | 14 | 15 | 40 | 287 | 176 | 223 | 150 | **404** | 35 |
| K1 Memoria (sin DMA) | 13 | 16 | 37 | 252 | 90 | 113 | 217 | **430** | 97 |
| K1-dma Memoria: DMA, dma-buf, IOMMU | 0 | 1 | 23 | 44 | 34 | 46 | 69 | **97** | 39 |
| K2 Sincronizacion, tiempo, trabajo diferido | 0 | 0 | 31 | 336 | 110 | 161 | 162 | **454** | 98 |
| K3 Estructuras y utilidades | 2 | 7 | 25 | 232 | 108 | 133 | 118 | **349** | 89 |
| K4 Modelo de dispositivos | 10 | 30 | 12 | 98 | 48 | 81 | 93 | **210** | 80 |
| K5 PCI, IRQ, energia | 14 | 3 | 31 | 133 | 75 | 212 | 141 | **386** | 140 |
| K6 Firmware, I2C, ACPI, backlight, debugfs... | 5 | 11 | 4 | 208 | 37 | 178 | 196 | **458** | 135 |
| K7 Fences/sync | 0 | 0 | 33 | 79 | 54 | 60 | 33 | **96** | 37 |
| K8 Entorno de usuario / VFS | 0 | 0 | 3 | 59 | 8 | 39 | 68 | **116** | 46 |
| K9 Miscelaneo (stubs: trace, kasan, hmm...) | 7 | 1 | 20 | 101 | 38 | 74 | 68 | **181** | 59 |
| K9-drm DRM core y helpers (shim propio con la opcion A) | 70 | 107 | 208 | 1669 | 192 | 917 | 340 | **2292** | 578 |
| K9-ttm TTM, drm_buddy, drm_exec, gpuvm | 0 | 0 | 0 | 77 | 138 | 110 | 0 | **174** | 83 |
| K9-sched drm/gpu_scheduler | 0 | 0 | 0 | 0 | 24 | 41 | 0 | **45** | 27 |
| K? Sin clasificar | 0 | 1 | 2 | 3 | 4 | 10 | 17 | **25** | 2 |
| **Total** | 135 | 192 | 469 | 3578 | 1136 | 2398 | 1672 | **5717** | 1545 |

Union acumulada siguiendo el orden de trabajo (cuanto anade cada driver a lo que ya necesitan los anteriores):

| Driver | Simbolos nuevos | Acumulado | de ellos K9-drm |
|---|---:|---:|---:|
| `bochs` | 135 | 135 | 70 |
| `simpledrm` | 142 | 277 | 75 |
| `virtio-gpu` | 357 | 634 | 128 |
| `i915` | 3243 | 3877 | 1555 |
| `xe` | 321 | 4198 | 93 |
| `amdgpu` | 833 | 5031 | 334 |
| `nvidia` | 686 | 5717 | 37 |

## Por tipo de definicion

| Nivel | func | inline | macro | type | enumconst | var |
|---|---:|---:|---:|---:|---:|---:|
| K0 | 35 | 67 | 260 | 36 | 4 | 2 |
| K1 | 97 | 117 | 170 | 21 | 22 | 3 |
| K1-dma | 39 | 24 | 20 | 10 | 4 | 0 |
| K2 | 98 | 128 | 169 | 39 | 13 | 7 |
| K3 | 89 | 94 | 149 | 16 | 1 | 0 |
| K4 | 80 | 30 | 55 | 29 | 15 | 1 |
| K5 | 140 | 48 | 124 | 37 | 36 | 1 |
| K6 | 135 | 36 | 159 | 72 | 52 | 4 |
| K7 | 37 | 31 | 6 | 12 | 8 | 2 |
| K8 | 46 | 19 | 25 | 22 | 3 | 1 |
| K9 | 59 | 17 | 66 | 30 | 8 | 1 |
| K9-drm | 578 | 130 | 1250 | 159 | 173 | 2 |
| K9-ttm | 83 | 20 | 28 | 29 | 13 | 1 |
| K9-sched | 27 | 1 | 2 | 7 | 7 | 1 |
| K? | 2 | 2 | 12 | 6 | 3 | 0 |

## Estimacion heuristica de tamano de la capa

Formula (orientativa, no medida): funcion 15 lineas, inline 6, tipo 8, macro 2, variable 2, constante de enum 1. Con la **opcion A** de DRM, K9-drm/ttm/sched es el *shim* propio sobre `kernel/drm/`; los «stubs» de K9 cuentan igual aunque muchos sean triviales.

| Nivel | QEMU (bochs, simpledrm, virtio-gpu) | Todos | kLOC QEMU | kLOC todos |
|---|---:|---:|---:|---:|
| K0 | 45 | 404 | 0.2 | 1.7 |
| K1 | 61 | 430 | 0.4 | 2.7 |
| K1-dma | 24 | 97 | 0.2 | 0.9 |
| K2 | 31 | 454 | 0.2 | 2.9 |
| K3 | 28 | 349 | 0.1 | 2.3 |
| K4 | 42 | 210 | 0.3 | 1.7 |
| K5 | 46 | 386 | 0.4 | 3.0 |
| K6 | 20 | 458 | 0.2 | 3.2 |
| K7 | 33 | 96 | 0.3 | 0.9 |
| K8 | 3 | 116 | 0.0 | 1.0 |
| K9 | 25 | 181 | 0.1 | 1.4 |
| K9-drm | 273 | 2292 | 2.4 | 13.4 |
| K9-ttm | 0 | 174 | 0.0 | 1.7 |
| K9-sched | 0 | 45 | 0.0 | 0.5 |
| K? | 3 | 25 | 0.0 | 0.1 |
| **Total** | | | **4.8** | **37.4** |

## Lista por nivel (los 40 mas usados; ordenados por numero de drivers y usos)

Columnas por driver: numero de usos. `-` = no lo usa.

### K0 — Base C (404 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `size_t` | type | `linux/types` | 2 | 3 | 7 | 250 | 130 | 282 | 551 |
| `ENOMEM` | macro | `asm-generic/errno-base` | 4 | 7 | 31 | 212 | 82 | 422 | 142 |
| `container_of` | macro | `linux/container_of` | 1 | 1 | 12 | 281 | 103 | 300 | 47 |
| `IS_ERR` | inline | `linux/err` | 1 | 9 | 13 | 475 | 112 | 45 | 82 |
| `PTR_ERR` | inline | `linux/err` | 1 | 4 | 9 | 326 | 78 | 34 | 44 |
| `ERR_PTR` | inline | `linux/err` | 1 | 27 | 8 | 284 | 98 | 17 | 41 |
| `ENODEV` | macro | `asm-generic/errno-base` | 3 | 1 | 4 | 264 | 23 | 57 | 61 |
| `EINVAL` | macro | `asm-generic/errno-base` | 1 | 7 | 52 | - | 162 | 2460 | 198 |
| `WARN_ON` | macro | `asm-generic/bug` | 2 | - | 9 | 99 | 26 | 152 | 132 |
| `uint32_t` | type | `linux/types` | 1 | 3 | 145 | 1 | 6 | - | 132 |
| `EBUSY` | macro | `asm-generic/errno-base` | 2 | - | 3 | 50 | 14 | 65 | 29 |
| `__iomem` | macro | `linux/compiler_types` | 2 | 1 | - | 82 | 12 | 59 | 1 |
| `WARN_ON_ONCE` | macro | `asm-generic/bug` | 1 | - | 2 | 19 | 6 | 48 | 6 |
| `uint64_t` | type | `linux/types` | - | 1 | 22 | - | 3 | 2334 | 102 |
| `IS_ENABLED` | macro | `linux/kconfig` | - | - | 1 | 183 | 38 | 21 | 20 |
| `EIO` | macro | `asm-generic/errno-base` | - | - | 1 | 110 | 14 | 67 | 29 |
| `EFAULT` | macro | `asm-generic/errno-base` | - | - | 5 | 98 | 21 | 64 | 17 |
| `ENOENT` | macro | `asm-generic/errno-base` | - | - | 7 | 62 | 26 | 78 | 14 |
| `snprintf` | func | `linux/sprintf` | - | - | 2 | 29 | 1 | 123 | 14 |
| `ALIGN` | macro | `linux/align` | - | - | 1 | 28 | 18 | 111 | 4 |
| `fallthrough` | macro | `linux/compiler_attributes` | - | - | 1 | 88 | 3 | 22 | 2 |
| `ENOSPC` | macro | `asm-generic/errno-base` | - | - | 1 | 61 | 7 | 21 | 4 |
| `dma_addr_t` | type | `linux/types` | - | - | 1 | 56 | 8 | 17 | 7 |
| `msecs_to_jiffies` | inline | `linux/jiffies` | - | - | 1 | 22 | 6 | 52 | 4 |
| `BUG_ON` | macro | `asm-generic/bug` | - | - | 3 | 4 | 1 | 50 | 22 |
| `UINT_MAX` | macro | `vdso/limits` | - | - | 1 | 12 | 2 | 37 | 5 |
| `ERR_CAST` | inline | `linux/err` | - | 4 | 1 | 30 | 8 | 1 | - |
| `dev_info` | macro | `linux/dev_printk` | - | - | - | 3 | 2 | 1267 | 2 |
| `ssize_t` | type | `linux/types` | - | - | - | 164 | 62 | 355 | 15 |
| `unlikely` | macro | `linux/compiler` | - | - | - | 308 | 56 | 146 | 19 |
| `offsetof` | macro | `linux/stddef` | - | - | - | 33 | 18 | 345 | 1 |
| `EOPNOTSUPP` | macro | `asm-generic/errno` | - | - | - | 25 | 19 | 254 | 26 |
| `struct list_head` | type | `linux/types` | - | - | 10 | 135 | - | 107 | 47 |
| `__user` | macro | `linux/compiler_types` | - | - | - | 89 | 18 | 111 | 8 |
| `ETIMEDOUT` | macro | `asm-generic/errno` | - | - | - | 49 | 5 | 146 | 3 |
| `BUILD_BUG_ON` | macro | `linux/build_bug` | - | - | - | 146 | 36 | 2 | 5 |
| `sprintf` | func | `linux/sprintf` | - | - | - | 3 | 29 | 155 | 1 |
| `GENMASK` | macro | `linux/bits` | - | - | - | 60 | 83 | 19 | 6 |
| `resource_size_t` | type | `linux/types` | - | - | - | 132 | 13 | 10 | 11 |
| `EPERM` | macro | `asm-generic/errno-base` | - | - | - | 20 | 3 | 118 | 12 |

### K1 — Memoria (sin DMA) (430 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `GFP_KERNEL` | macro | `linux/gfp_types` | 1 | 6 | 29 | 262 | 93 | 963 | 35 |
| `kfree` | func | `linux/slab` | 2 | - | 27 | 311 | 87 | 1203 | 74 |
| `IORESOURCE_MEM` | macro | `linux/ioport` | 2 | 1 | - | 4 | 2 | 7 | 6 |
| `kzalloc` | inline | `linux/slab` | - | - | 15 | 106 | 38 | 792 | 8 |
| `kmalloc` | inline | `linux/slab` | - | - | 3 | 43 | 10 | 41 | 22 |
| `vmap` | func | `linux/vmalloc` | - | - | 2 | 3 | 69 | 2 | 4 |
| `copy_to_user` | inline | `linux/uaccess` | - | - | 2 | 18 | 8 | 45 | 6 |
| `kvfree` | func | `linux/rcutiny` | - | - | 8 | 12 | 9 | 46 | 2 |
| `GFP_ATOMIC` | macro | `linux/gfp_types` | - | - | 2 | 12 | 8 | 41 | 4 |
| `copy_from_user` | inline | `linux/uaccess` | - | - | 3 | 26 | 3 | 22 | 12 |
| `readl` | macro | `asm/io` | 1 | - | - | 3 | 3 | 39 | 9 |
| `struct resource` | type | `linux/ioport` | - | 3 | - | 14 | 2 | 4 | 9 |
| `kmem_cache_free` | func | `linux/slab` | - | - | 1 | 14 | 2 | 5 | 1 |
| `kmem_cache_destroy` | func | `linux/slab` | - | - | 1 | 12 | 4 | 3 | 1 |
| `iounmap` | macro | `asm/io` | 2 | - | - | 5 | 1 | 7 | 4 |
| `kvmalloc_array` | inline | `linux/slab` | - | - | 2 | 6 | 2 | 6 | 1 |
| `struct vm_operations_struct` | type | `linux/mm` | - | - | 1 | 4 | 1 | 1 | 4 |
| `vunmap` | func | `linux/vmalloc` | - | - | 2 | 2 | 1 | 2 | 4 |
| `kmem_cache_create` | func | `linux/slab` | - | - | 1 | 1 | 3 | 3 | 1 |
| `u64_to_user_ptr` | macro | `linux/kernel` | - | - | 9 | 48 | 16 | 6 | - |
| `struct page` | type | `linux/mm_types` | - | - | 1 | 42 | 1 | 14 | - |
| `writel` | macro | `asm/io` | 2 | - | - | 8 | 3 | 39 | - |
| `struct mm_struct` | type | `linux/mm_types` | - | - | - | 3 | 1 | 36 | 11 |
| `PAGE_ALIGN` | macro | `linux/mm` | - | - | 1 | 26 | 19 | 4 | - |
| `struct vm_area_struct` | type | `linux/mm_types` | - | - | 1 | - | 1 | 2 | 44 |
| `page_offset` | inline | `linux/pagemap` | - | - | - | 15 | 3 | 2 | 9 |
| `resource_size` | inline | `linux/ioport` | - | 5 | - | 18 | 1 | - | 4 |
| `PAGE_MASK` | macro | `asm/page_types` | - | - | - | 6 | 6 | 7 | 3 |
| `memcpy_fromio` | macro | `asm/io` | - | - | - | 5 | 1 | 15 | 1 |
| `struct vm_fault` | type | `linux/mm` | - | - | - | 3 | 1 | 1 | 7 |
| `ioremap` | macro | `asm/io` | 2 | - | - | 2 | - | 2 | 2 |
| `ioremap_wc` | macro | `asm/io` | - | - | - | 3 | 1 | 2 | 2 |
| `kmem_cache_zalloc` | inline | `linux/slab` | - | - | 1 | 4 | 2 | - | 1 |
| `IORESOURCE_UNSET` | macro | `linux/ioport` | - | - | - | 1 | 1 | 2 | 2 |
| `kcalloc` | inline | `linux/slab` | - | - | - | 14 | 2 | 83 | - |
| `page_size` | inline | `linux/mm` | - | - | - | 47 | 24 | - | 25 |
| `gfp_t` | type | `linux/gfp_types` | - | - | - | 45 | - | 1 | 5 |
| `put_user` | macro | `asm/uaccess` | - | - | - | 3 | 4 | 29 | - |
| `offset_in_page` | macro | `linux/mm` | - | - | - | 31 | - | 3 | 1 |
| `kmalloc_array` | inline | `linux/slab` | - | - | - | 10 | 8 | 16 | - |

### K1-dma — Memoria: DMA, dma-buf, IOMMU (97 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `sg_dma_len` | macro | `linux/scatterlist` | - | - | 3 | 50 | 4 | 1 | 4 |
| `sg_free_table` | func | `linux/scatterlist` | - | - | 4 | 13 | 9 | 14 | 4 |
| `sg_dma_address` | macro | `linux/scatterlist` | - | - | 3 | 30 | 2 | 3 | 4 |
| `struct dma_buf_attachment` | type | `linux/dma-buf` | - | - | 4 | 4 | 9 | 12 | 8 |
| `DMA_ATTR_SKIP_CPU_SYNC` | macro | `linux/dma-mapping` | - | - | 2 | 3 | 6 | 7 | 4 |
| `sg_alloc_table` | func | `linux/scatterlist` | - | - | 2 | 11 | 1 | 3 | 3 |
| `sg_set_page` | inline | `linux/scatterlist` | - | - | 2 | 9 | 1 | 1 | 3 |
| `DMA_TO_DEVICE` | enumconst | `linux/dma-direction` | - | - | 2 | 1 | 3 | 6 | 3 |
| `dma_set_max_seg_size` | inline | `linux/dma-mapping` | - | - | 1 | 1 | 1 | 1 | 1 |
| `DMA_BIDIRECTIONAL` | enumconst | `linux/dma-direction` | - | - | - | 8 | 9 | 12 | 9 |
| `enum dma_data_direction` | type | `linux/dma-direction` | - | - | 6 | 3 | - | 13 | 8 |
| `struct scatterlist` | type | `linux/scatterlist` | - | - | 5 | - | 5 | 2 | 14 |
| `sg_page` | inline | `linux/scatterlist` | - | - | - | 12 | 1 | 1 | 1 |
| `DMA_BIT_MASK` | macro | `linux/dma-mapping` | - | - | - | 3 | 2 | 6 | 3 |
| `dma_mapping_error` | inline | `linux/dma-mapping` | - | - | 1 | - | 1 | 4 | 4 |
| `dma_unmap_resource` | func | `linux/dma-mapping` | - | - | 1 | - | 2 | 4 | 1 |
| `DMA_FROM_DEVICE` | enumconst | `linux/dma-direction` | - | 2 | - | - | 1 | 1 | 3 |
| `get_dma_buf` | inline | `linux/dma-buf` | - | - | - | 1 | 1 | 3 | 1 |
| `dma_buf_unmap_attachment` | func | `linux/dma-buf` | - | - | - | 1 | 2 | 1 | 1 |
| `dma_map_resource` | func | `linux/dma-mapping` | - | - | 1 | - | 1 | 2 | 1 |
| `struct dma_buf_ops` | type | `linux/dma-buf` | - | - | - | 1 | 1 | 2 | 1 |
| `dma_buf_map_attachment` | func | `linux/dma-buf` | - | - | - | 1 | 1 | 1 | 1 |
| `struct dma_buf` | type | `linux/dma-buf` | - | - | 6 | - | 10 | 19 | - |
| `sg_next` | func | `linux/scatterlist` | - | - | - | 9 | 1 | - | 2 |
| `for_each_sgtable_sg` | macro | `linux/scatterlist` | - | - | 2 | - | 4 | 4 | - |
| `dma_buf_put` | func | `linux/dma-buf` | - | - | - | 1 | - | 2 | 6 |
| `dma_map_sgtable` | func | `linux/dma-mapping` | - | - | - | 3 | 3 | 3 | - |
| `dma_unmap_sgtable` | inline | `linux/dma-mapping` | - | - | - | 1 | 4 | 4 | - |
| `dma_set_mask` | func | `linux/dma-mapping` | - | - | - | 1 | 1 | - | 3 |
| `sg_alloc_table_from_pages_segment` | func | `linux/scatterlist` | - | - | - | 2 | 2 | - | 1 |
| `dma_free_coherent` | inline | `linux/dma-mapping` | - | - | - | 2 | - | 1 | 1 |
| `DEFINE_DMA_BUF_EXPORT_INFO` | macro | `linux/dma-buf` | - | - | 1 | 1 | - | - | 1 |
| `dma_alloc_coherent` | inline | `linux/dma-mapping` | - | - | - | 1 | - | 1 | 1 |
| `dma_max_mapping_size` | func | `linux/dma-mapping` | - | - | 1 | 1 | 1 | - | - |
| `dma_set_coherent_mask` | func | `linux/dma-mapping` | - | - | - | 1 | 1 | - | 1 |
| `struct sg_table` | type | `linux/scatterlist` | - | - | 13 | - | - | 14 | - |
| `for_each_sg` | macro | `linux/scatterlist` | - | - | - | 2 | - | - | 7 |
| `dma_set_mask_and_coherent` | inline | `linux/dma-mapping` | - | - | - | - | - | 6 | 1 |
| `sg_init_one` | func | `linux/scatterlist` | - | - | 4 | - | - | - | 1 |
| `struct dma_buf_attach_ops` | type | `linux/dma-buf` | - | - | - | - | 3 | 2 | - |

### K2 — Sincronizacion, tiempo, trabajo diferido (454 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `mutex_unlock` | func | `linux/mutex` | - | - | 2 | 338 | 68 | 539 | 59 |
| `mutex_lock` | macro | `linux/mutex` | - | - | 2 | 269 | 58 | 508 | 46 |
| `spin_unlock_irqrestore` | inline | `linux/spinlock` | - | - | 2 | 128 | 7 | 139 | 32 |
| `spin_unlock` | inline | `linux/spinlock` | - | - | 26 | 131 | 53 | 88 | 9 |
| `spin_lock` | inline | `linux/spinlock` | - | - | 23 | 117 | 50 | 75 | 9 |
| `spin_lock_irqsave` | macro | `linux/spinlock` | - | - | 2 | 114 | 6 | 121 | 27 |
| `atomic_read` | inline | `linux/atomic/atomic-instrumented` | - | - | 2 | 132 | 15 | 70 | 29 |
| `mutex_init` | macro | `linux/mutex` | - | - | 1 | 49 | 8 | 61 | 8 |
| `spin_lock_init` | macro | `linux/spinlock` | - | - | 6 | 42 | 17 | 30 | 11 |
| `spinlock_t` | type | `linux/spinlock_types` | - | - | 6 | 41 | 17 | 27 | 10 |
| `current` | macro | `asm/current` | - | - | 1 | 18 | 9 | 19 | 46 |
| `INIT_WORK` | macro | `linux/workqueue` | - | - | 3 | 33 | 15 | 30 | 1 |
| `mutex_destroy` | func | `linux/mutex` | - | - | 1 | 18 | 8 | 27 | 2 |
| `wait_queue_head_t` | type | `linux/wait` | - | - | 2 | 10 | 5 | 1 | 7 |
| `wake_up_all` | macro | `linux/wait` | - | - | 3 | 7 | 12 | 1 | 1 |
| `init_waitqueue_head` | macro | `linux/wait` | - | - | 2 | 9 | 5 | 1 | 6 |
| `udelay` | macro | `asm-generic/delay` | - | - | - | 69 | 2 | 411 | 3 |
| `msleep` | func | `linux/delay` | - | - | - | 32 | 7 | 127 | 1 |
| `spin_unlock_irq` | inline | `linux/spinlock` | - | - | - | 125 | 17 | 13 | 3 |
| `spin_lock_irq` | inline | `linux/spinlock` | - | - | - | 121 | 17 | 13 | 3 |
| `struct work_struct` | type | `linux/workqueue_types` | - | - | 11 | - | 36 | 83 | 5 |
| `atomic_set` | inline | `linux/atomic/atomic-instrumented` | - | - | 3 | 16 | - | 50 | 11 |
| `cancel_delayed_work_sync` | func | `linux/workqueue` | - | - | - | 16 | 1 | 56 | 3 |
| `struct mutex` | type | `linux/mutex_types` | - | - | 1 | - | 13 | 53 | 9 |
| `MAX_SCHEDULE_TIMEOUT` | macro | `linux/sched` | - | - | - | 29 | 15 | 17 | 1 |
| `atomic_inc` | inline | `linux/atomic/atomic-instrumented` | - | - | 1 | 41 | - | 8 | 8 |
| `atomic64_read` | inline | `linux/atomic/atomic-instrumented` | - | - | 2 | 3 | - | 31 | 14 |
| `flush_work` | func | `linux/workqueue` | - | - | 4 | 27 | 3 | 13 | - |
| `struct delayed_work` | type | `linux/workqueue` | - | - | - | 16 | 1 | 22 | 3 |
| `INIT_DELAYED_WORK` | macro | `linux/workqueue` | - | - | - | 15 | 1 | 21 | 3 |
| `kref_put` | inline | `linux/kref` | - | - | - | 14 | 5 | 13 | 6 |
| `schedule_work` | inline | `linux/workqueue` | - | - | 4 | 1 | - | 30 | 1 |
| `atomic_dec` | inline | `linux/atomic/atomic-instrumented` | - | - | - | 23 | 1 | 5 | 6 |
| `kref_init` | inline | `linux/kref` | - | - | - | 17 | 4 | 12 | 2 |
| `mutex_lock_interruptible` | macro | `linux/mutex` | - | - | - | 23 | 2 | 2 | 7 |
| `atomic64_set` | inline | `linux/atomic/atomic-instrumented` | - | - | 1 | 2 | - | 28 | 2 |
| `kref_get` | inline | `linux/kref` | - | - | - | 17 | 4 | 8 | 3 |
| `kref_get_unless_zero` | inline | `linux/kref` | - | - | - | 25 | 1 | 2 | 1 |
| `kref_read` | inline | `linux/kref` | - | - | - | 14 | 4 | 3 | 1 |
| `struct task_struct` | type | `linux/sched` | - | - | - | 2 | 4 | 3 | 9 |

### K3 — Estructuras y utilidades (349 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `min` | macro | `linux/minmax` | 1 | 1 | 3 | 354 | 39 | 753 | 14 |
| `ARRAY_SIZE` | macro | `linux/array_size` | 2 | 2 | 4 | 528 | 105 | - | 22 |
| `strlen` | macro | `linux/fortify-string` | - | 2 | 2 | 3 | 2 | 49 | 35 |
| `memset` | macro | `linux/fortify-string` | - | - | 24 | 134 | 6 | 795 | 41 |
| `memcpy` | macro | `linux/fortify-string` | - | - | 3 | 106 | 9 | 402 | 48 |
| `list_for_each_entry` | macro | `linux/list` | - | - | 4 | 91 | 14 | 120 | 10 |
| `INIT_LIST_HEAD` | inline | `linux/list` | - | - | 5 | 72 | 28 | 58 | 33 |
| `list_empty` | inline | `linux/list` | - | - | 1 | 73 | 25 | 38 | 26 |
| `list_for_each_entry_safe` | macro | `linux/list` | - | - | 5 | 53 | 13 | 50 | 7 |
| `list_del` | inline | `linux/list` | - | - | 5 | 33 | 10 | 37 | 21 |
| `list_add_tail` | inline | `linux/list` | - | - | 4 | 39 | 11 | 26 | 16 |
| `strcmp` | func | `linux/string` | - | 1 | - | 15 | 3 | 14 | 27 |
| `list_first_entry` | macro | `linux/list` | - | - | 1 | 5 | 2 | 10 | 9 |
| `strscpy` | func | `linux/fortify-string` | - | 1 | 1 | - | 2 | 3 | 1 |
| `max` | macro | `linux/minmax` | - | - | - | 467 | 32 | 667 | 16 |
| `str_yes_no` | inline | `linux/string_choices` | - | - | 1 | 172 | 13 | 9 | - |
| `DIV_ROUND_UP` | macro | `linux/math` | - | - | 1 | 152 | 9 | 25 | - |
| `min_t` | macro | `linux/minmax` | - | - | 2 | 77 | 9 | 76 | - |
| `struct_size` | macro | `linux/overflow` | - | - | 1 | 7 | 2 | 89 | - |
| `ilog2` | macro | `linux/log2` | - | - | - | 32 | 11 | 22 | 1 |
| `strcpy` | func | `linux/fortify-string` | - | - | - | 3 | 5 | 48 | 6 |
| `max_t` | macro | `linux/minmax` | - | 2 | - | 33 | 5 | 17 | - |
| `list_del_init` | inline | `linux/list` | - | - | - | 24 | 17 | 6 | 6 |
| `array_size` | macro | `linux/overflow` | - | - | 6 | 1 | - | 43 | 2 |
| `list_add` | inline | `linux/list` | - | - | - | 19 | 2 | 15 | 8 |
| `list_move_tail` | inline | `linux/list` | - | - | - | 26 | 11 | 4 | 2 |
| `struct rb_node` | type | `linux/rbtree_types` | - | - | - | 32 | 2 | 1 | 5 |
| `list_entry` | macro | `linux/list` | - | - | - | 5 | 4 | 9 | 21 |
| `list_first_entry_or_null` | macro | `linux/list` | - | - | - | 13 | 10 | 8 | 2 |
| `strsep` | func | `linux/string` | - | - | - | 2 | 1 | 8 | 12 |
| `crc32` | macro | `linux/crc32` | - | - | - | 3 | 3 | 2 | 11 |
| `idr_for_each_entry` | macro | `linux/idr` | - | - | - | 5 | 1 | 10 | 3 |
| `RB_EMPTY_ROOT` | macro | `linux/rbtree` | - | - | - | 4 | 1 | 1 | 2 |
| `roundup` | macro | `linux/math` | - | - | 1 | 3 | - | 2 | 2 |
| `struct ida` | type | `linux/idr` | - | - | 2 | 2 | 1 | 1 | - |
| `ida_init` | inline | `linux/idr` | - | - | 2 | 1 | 1 | 1 | - |
| `memcmp` | func | `linux/fortify-string` | - | - | - | 30 | - | 71 | 11 |
| `DIV_ROUND_CLOSEST` | macro | `linux/math` | - | - | - | 78 | 5 | 7 | - |
| `order_base_2` | macro | `linux/log2` | - | - | - | 2 | 2 | 84 | - |
| `memmove` | macro | `linux/fortify-string` | - | - | - | 2 | - | 32 | 1 |

### K4 — Modelo de dispositivos (210 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `MODULE_LICENSE` | macro | `linux/module` | 1 | 1 | 1 | 1 | 1 | 2 | 3 |
| `MODULE_DEVICE_TABLE` | macro | `linux/module` | 1 | 1 | 1 | 1 | 1 | 1 | 3 |
| `struct device` | type | `linux/device` | 2 | 2 | 4 | - | 56 | 171 | 74 |
| `MODULE_PARM_DESC` | macro | `linux/moduleparam` | 3 | - | 1 | 5 | 8 | 91 | 14 |
| `module_param_named` | macro | `linux/moduleparam` | 1 | - | 2 | 2 | 3 | 76 | 20 |
| `MODULE_DESCRIPTION` | macro | `linux/module` | - | 1 | 1 | 1 | 1 | 2 | 3 |
| `dev_get_drvdata` | inline | `linux/device` | 2 | - | - | 8 | 6 | 114 | 2 |
| `THIS_MODULE` | macro | `linux/init` | - | - | 1 | 39 | 3 | 67 | 18 |
| `MODULE_AUTHOR` | macro | `linux/module` | 1 | - | 3 | 2 | 1 | 2 | - |
| `PCI_ANY_ID` | macro | `linux/mod_devicetable` | 4 | - | - | 4 | - | 619 | 38 |
| `MODULE_FIRMWARE` | macro | `linux/module` | - | - | - | 14 | 1 | 585 | 1 |
| `__init` | macro | `linux/init` | - | - | - | 9 | 4 | 1 | 14 |
| `struct platform_device` | type | `linux/platform_device` | - | 5 | - | 3 | - | 3 | 16 |
| `struct pci_device_id` | type | `linux/mod_devicetable` | 2 | - | - | - | 4 | 4 | 15 |
| `dev_name` | inline | `linux/device` | - | - | 2 | 9 | 2 | - | 7 |
| `module_exit` | macro | `linux/module` | - | - | - | 1 | 1 | 2 | 6 |
| `MODULE_IMPORT_NS` | macro | `linux/module` | - | - | - | 1 | 1 | 1 | 5 |
| `__exit` | macro | `linux/init` | - | - | - | 1 | 1 | 2 | 3 |
| `dev_pm_set_driver_flags` | inline | `linux/device` | - | - | - | 1 | 1 | 2 | 2 |
| `module_init` | macro | `linux/module` | - | - | - | 1 | 1 | 1 | 3 |
| `struct device_attribute` | type | `linux/device` | - | - | - | 13 | 28 | 143 | - |
| `struct kobject` | type | `linux/kobject` | - | - | - | 78 | 53 | 43 | - |
| `sysfs_emit` | func | `linux/sysfs` | - | - | - | 32 | 23 | 110 | - |
| `struct kobj_attribute` | type | `linux/kobject` | - | - | - | 72 | 45 | 13 | - |
| `struct attribute` | type | `linux/sysfs` | - | - | - | 30 | 8 | 25 | - |
| `__ATTR` | macro | `linux/sysfs` | - | - | - | 28 | 18 | 1 | - |
| `DMI_PRODUCT_NAME` | enumconst | `linux/mod_devicetable` | - | - | - | 25 | - | 17 | 1 |
| `struct attribute_group` | type | `linux/sysfs` | - | - | - | 11 | 3 | 26 | - |
| `kobject_put` | func | `linux/kobject` | - | - | - | 8 | 13 | 10 | - |
| `kobj_to_dev` | macro | `linux/device` | - | - | - | 8 | 1 | 9 | - |
| `module_param` | macro | `linux/moduleparam` | 2 | - | - | - | - | 12 | 4 |
| `struct kobj_type` | type | `linux/kobject` | - | - | - | 2 | 4 | 7 | - |
| `sysfs_create_files` | func | `linux/sysfs` | - | - | - | 7 | 5 | 1 | - |
| `sysfs_create_file` | inline | `linux/sysfs` | - | - | - | 7 | 1 | 3 | - |
| `kobj_sysfs_ops` | var | `linux/kobject` | - | - | - | 2 | 4 | 4 | - |
| `kobject_add` | func | `linux/kobject` | - | - | - | 2 | 5 | 1 | - |
| `kobject_init` | func | `linux/kobject` | - | - | - | 2 | 5 | 1 | - |
| `sysfs_create_group` | func | `linux/sysfs` | - | - | - | 2 | 1 | 3 | - |
| `sysfs_remove_group` | func | `linux/sysfs` | - | - | - | 1 | 1 | 3 | - |
| `sysfs_emit_at` | func | `linux/sysfs` | - | - | - | 2 | - | 299 | - |

### K5 — PCI, IRQ, energia (386 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `to_pci_dev` | macro | `linux/pci` | 2 | - | 1 | 64 | 28 | 3 | 21 |
| `pci_get_drvdata` | inline | `linux/pci` | 2 | - | - | 4 | 2 | 13 | 25 |
| `pci_resource_len` | macro | `linux/pci` | 3 | - | - | 10 | 8 | 12 | 4 |
| `pci_resource_start` | macro | `linux/pci` | 2 | - | - | 7 | 4 | 10 | 3 |
| `pci_set_drvdata` | inline | `linux/pci` | 1 | - | - | 2 | 3 | 2 | 5 |
| `struct dev_pm_ops` | type | `linux/pm` | 1 | - | - | 2 | 1 | 1 | 3 |
| `struct pci_driver` | type | `linux/pci` | 1 | - | - | 1 | 1 | 1 | 2 |
| `irqreturn_t` | type | `linux/irqreturn` | - | - | - | 16 | 2 | 2 | 20 |
| `IRQ_NONE` | enumconst | `linux/irqreturn` | - | - | - | 23 | 4 | 1 | 4 |
| `pci_read_config_dword` | func | `linux/pci` | - | - | - | 10 | 1 | 5 | 13 |
| `IRQ_HANDLED` | enumconst | `linux/irqreturn` | - | - | - | 18 | 3 | 2 | 2 |
| `pci_read_config_word` | func | `linux/pci` | - | - | - | 11 | 1 | 2 | 9 |
| `pci_set_master` | func | `linux/pci` | - | - | - | 2 | 3 | 15 | 2 |
| `free_irq` | func | `linux/interrupt` | - | - | - | 1 | 2 | 1 | 12 |
| `PCI_COMMAND` | macro | `linux/pci_regs` | - | - | - | 3 | 3 | 3 | 6 |
| `pci_disable_device` | func | `linux/pci` | - | - | - | 3 | 2 | 6 | 3 |
| `pci_write_config_dword` | func | `linux/pci` | - | - | - | 8 | 2 | 2 | 1 |
| `pci_domain_nr` | inline | `linux/pci` | - | - | - | 3 | 1 | 6 | 2 |
| `pci_resource_flags` | macro | `linux/pci` | - | - | - | 2 | 2 | 2 | 4 |
| `PCI_DEVICE` | macro | `linux/pci` | - | - | - | 4 | 1 | 3 | 1 |
| `pci_enable_device` | func | `linux/pci` | - | - | - | 2 | 2 | 3 | 2 |
| `struct pci_bus` | type | `linux/pci` | - | - | - | 1 | 1 | 1 | 4 |
| `pci_rebar_bytes_to_size` | inline | `linux/pci` | - | - | - | 2 | 2 | 1 | 1 |
| `pci_release_resource` | func | `linux/pci` | - | - | - | 1 | 1 | 2 | 2 |
| `pci_save_state` | func | `linux/pci` | - | - | - | 1 | 2 | 1 | 2 |
| `pci_unregister_driver` | func | `linux/pci` | - | - | - | 1 | 1 | 1 | 3 |
| `DPM_FLAG_NO_DIRECT_COMPLETE` | macro | `linux/pm` | - | - | - | 1 | 1 | 1 | 2 |
| `PCI_COMMAND_MEMORY` | macro | `linux/pci_regs` | - | - | - | 1 | 1 | 1 | 2 |
| `pci_register_driver` | macro | `linux/pci` | - | - | - | 1 | 1 | 1 | 2 |
| `pci_resize_resource` | func | `linux/pci` | - | - | - | 1 | 1 | 1 | 2 |
| `pci_assign_unassigned_bus_resources` | func | `linux/pci` | - | - | - | 1 | 1 | 1 | 1 |
| `pci_rebar_get_possible_sizes` | func | `linux/pci` | - | - | - | 1 | 1 | 1 | 1 |
| `pm_runtime_allow` | func | `linux/pm_runtime` | - | - | - | 1 | 1 | 1 | 1 |
| `pm_runtime_set_autosuspend_delay` | func | `linux/pm_runtime` | - | - | - | 1 | 1 | 1 | 1 |
| `pm_runtime_use_autosuspend` | inline | `linux/pm_runtime` | - | - | - | 1 | 1 | 1 | 1 |
| `pm_runtime_mark_last_busy` | inline | `linux/pm_runtime` | - | - | - | 2 | 2 | 92 | - |
| `pm_runtime_get_sync` | inline | `linux/pm_runtime` | - | - | - | 4 | 2 | 82 | - |
| `PCI_FUNC` | macro | `linux/pci` | - | - | - | 2 | - | 1 | 29 |
| `pci_write_config_word` | func | `linux/pci` | - | - | - | 4 | - | 4 | 7 |
| `pci_get_class` | func | `linux/pci` | - | - | - | 2 | - | 2 | 8 |

### K6 — Firmware, I2C, ACPI, backlight, debugfs... (458 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `seq_printf` | func | `linux/seq_file` | - | - | 4 | 269 | - | 125 | 58 |
| `seq_puts` | func | `linux/seq_file` | - | - | 1 | 52 | - | 28 | 2 |
| `s64` | type | `acpi/actypes` | 1 | - | - | 18 | 5 | 17 | - |
| `s32` | type | `acpi/actypes` | - | - | - | 11 | 7 | 12 | 4 |
| `release_firmware` | func | `linux/firmware` | - | - | - | 5 | 2 | 9 | 2 |
| `request_firmware` | func | `linux/firmware` | - | - | - | 3 | 1 | 4 | 4 |
| `debug` | macro | `video/trident` | - | - | - | 97 | 2 | 750 | - |
| `output` | macro | `video/trident` | - | - | 60 | - | - | 670 | 38 |
| `debugfs_create_file` | func | `linux/debugfs` | - | - | - | 38 | 1 | 47 | - |
| `I2C_M_RD` | macro | `linux/i2c` | - | - | - | 14 | - | 40 | 4 |
| `union acpi_object` | type | `acpi/actypes` | - | - | - | 5 | - | 27 | 23 |
| `acpi_handle` | type | `acpi/actypes` | - | - | - | 7 | - | 13 | 31 |
| `struct i2c_msg` | type | `linux/i2c` | - | - | - | 25 | - | 19 | 3 |
| `single_release` | func | `linux/seq_file` | - | - | - | 23 | - | 3 | 15 |
| `seq_read` | func | `linux/seq_file` | - | - | - | 23 | - | 3 | 9 |
| `acpi_status` | type | `acpi/actypes` | - | - | - | 1 | - | 14 | 16 |
| `single_open` | func | `linux/seq_file` | - | - | - | 18 | - | 3 | 10 |
| `struct acpi_buffer` | type | `acpi/actypes` | - | - | - | 1 | - | 18 | 9 |
| `seq_lseek` | func | `linux/seq_file` | - | - | - | 13 | - | 3 | 9 |
| `ACPI_FAILURE` | macro | `acpi/acexcep` | - | - | - | 1 | - | 8 | 15 |
| `KERN_INFO` | macro | `linux/kern_levels` | - | - | - | 1 | - | 3 | 20 |
| `i2c_transfer` | func | `linux/i2c` | - | - | - | 17 | - | 6 | 1 |
| `struct backlight_device` | type | `linux/backlight` | - | - | - | 4 | - | 10 | 6 |
| `i2c_del_adapter` | func | `linux/i2c` | - | - | - | 2 | - | 15 | 2 |
| `KERN_DEBUG` | macro | `linux/kern_levels` | - | - | - | 12 | - | 3 | 3 |
| `ACPI_HANDLE` | macro | `linux/acpi` | - | - | - | 2 | - | 5 | 8 |
| `struct firmware` | type | `linux/firmware` | - | - | - | 8 | 5 | - | 2 |
| `ACPI_TYPE_BUFFER` | macro | `acpi/actypes` | - | - | - | 1 | - | 4 | 9 |
| `i2c_add_adapter` | func | `linux/i2c` | - | - | - | 2 | - | 9 | 2 |
| `struct i2c_algorithm` | type | `linux/i2c` | - | - | - | 2 | - | 9 | 2 |
| `I2C_FUNC_I2C` | macro | `linux/i2c` | - | - | - | 1 | - | 9 | 2 |
| `bl_get_data` | inline | `linux/backlight` | - | - | - | 2 | - | 5 | 2 |
| `debugfs_create_dir` | func | `linux/debugfs` | - | - | - | 5 | 2 | 2 | - |
| `ACPI_TYPE_PACKAGE` | macro | `acpi/actypes` | - | - | - | 2 | - | 1 | 5 |
| `struct va_format` | type | `linux/printk` | - | - | - | 4 | - | 1 | 1 |
| `struct acpi_bus_event` | type | `acpi/acpi_bus` | - | - | - | 1 | - | 3 | 1 |
| `BACKLIGHT_RAW` | enumconst | `linux/backlight` | - | - | - | 1 | - | 2 | 1 |
| `acpi_video_backlight_use_native` | inline | `acpi/video` | - | - | - | 1 | - | 2 | 1 |
| `acpi_video_register_backlight` | func | `acpi/video` | - | - | - | 1 | - | 2 | 1 |
| `backlight_device_register` | func | `linux/backlight` | - | - | - | 1 | - | 2 | 1 |

### K7 — Fences/sync (96 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `dma_fence_put` | inline | `linux/dma-fence` | - | - | 11 | 44 | 50 | 140 | 16 |
| `dma_fence_get` | inline | `linux/dma-fence` | - | - | 1 | 11 | 20 | 40 | 2 |
| `dma_resv_unlock` | inline | `linux/dma-resv` | - | - | 1 | 6 | 10 | 10 | 1 |
| `dma_resv_add_fence` | func | `linux/dma-resv` | - | - | 1 | 5 | 6 | 11 | 2 |
| `dma_resv_reserve_fences` | func | `linux/dma-resv` | - | - | 2 | 7 | 3 | 11 | 1 |
| `struct dma_fence_ops` | type | `linux/dma-fence` | - | - | 1 | 5 | 3 | 6 | 2 |
| `dma_fence_context_alloc` | func | `linux/dma-fence` | - | - | 2 | 3 | 6 | 3 | 2 |
| `drm_syncobj_put` | inline | `drm/drm_syncobj` | - | - | 2 | 8 | 1 | 3 | 2 |
| `dma_fence_init` | func | `linux/dma-fence` | - | - | 1 | 4 | 3 | 3 | 2 |
| `dma_resv_lock` | inline | `linux/dma-resv` | - | - | 1 | 2 | 3 | 3 | 1 |
| `drm_syncobj_find` | func | `drm/drm_syncobj` | - | - | 2 | 2 | 2 | 2 | 2 |
| `dma_fence_chain_alloc` | inline | `linux/dma-fence-chain` | - | - | 1 | 2 | 1 | 1 | 1 |
| `drm_syncobj_add_point` | func | `drm/drm_syncobj` | - | - | 1 | 1 | 1 | 1 | 1 |
| `dma_fence_wait` | inline | `linux/dma-fence` | - | - | 2 | 11 | 9 | 25 | - |
| `dma_fence_wait_timeout` | func | `linux/dma-fence` | - | - | 1 | 3 | 4 | 32 | - |
| `struct dma_fence_cb` | type | `linux/dma-fence` | - | - | - | 20 | 8 | 8 | 4 |
| `dma_resv_wait_timeout` | func | `linux/dma-resv` | - | - | 1 | 3 | 9 | 6 | - |
| `dma_fence_signal` | func | `linux/dma-fence` | - | - | - | 4 | 4 | 6 | 4 |
| `dma_fence_add_callback` | func | `linux/dma-fence` | - | - | - | 6 | 4 | 4 | 2 |
| `dma_fence_set_error` | inline | `linux/dma-fence` | - | - | - | 1 | 5 | 7 | 2 |
| `struct drm_syncobj` | type | `drm/drm_syncobj` | - | - | 5 | 5 | - | 2 | 2 |
| `struct dma_fence_array` | type | `linux/dma-fence-array` | - | - | - | 5 | 5 | 1 | 1 |
| `struct sync_file` | type | `linux/sync_file` | - | - | 1 | 7 | - | 1 | 2 |
| `DMA_RESV_USAGE_READ` | enumconst | `linux/dma-resv` | - | - | 2 | 4 | - | 3 | 1 |
| `dma_resv_test_signaled` | func | `linux/dma-resv` | - | - | 1 | 3 | 5 | 1 | - |
| `DMA_RESV_USAGE_WRITE` | enumconst | `linux/dma-resv` | - | - | 1 | 3 | - | 3 | 1 |
| `struct dma_fence_chain` | type | `linux/dma-fence-chain` | - | - | 1 | 3 | - | 1 | 2 |
| `drm_syncobj_replace_fence` | func | `drm/drm_syncobj` | - | - | 2 | 2 | 1 | 1 | - |
| `sync_file_create` | func | `linux/sync_file` | - | - | 1 | 2 | - | 1 | 2 |
| `DMA_RESV_USAGE_BOOKKEEP` | enumconst | `linux/dma-resv` | - | - | - | 4 | 26 | 19 | - |
| `dma_fence_is_signaled` | inline | `linux/dma-fence` | - | - | - | 11 | 6 | 23 | - |
| `DMA_RESV_USAGE_KERNEL` | enumconst | `linux/dma-resv` | - | - | - | 3 | 11 | 7 | - |
| `struct dma_resv_iter` | type | `linux/dma-resv` | - | - | - | 7 | 3 | 6 | - |
| `dma_resv_assert_held` | macro | `linux/dma-resv` | - | - | - | 3 | 5 | 7 | - |
| `dma_resv_iter_end` | inline | `linux/dma-resv` | - | - | - | 5 | 2 | 2 | - |
| `dma_resv_trylock` | inline | `linux/dma-resv` | - | - | - | 1 | 3 | 5 | - |
| `dma_resv_for_each_fence` | macro | `linux/dma-resv` | - | - | - | 2 | 1 | 5 | - |
| `dma_resv_for_each_fence_unlocked` | macro | `linux/dma-resv` | - | - | - | 5 | 2 | 1 | - |
| `dma_resv_iter_begin` | inline | `linux/dma-resv` | - | - | - | 5 | 2 | 1 | - |
| `dma_fence_array_create` | func | `linux/dma-fence-array` | - | - | - | 1 | 4 | 1 | - |

### K8 — Entorno de usuario / VFS (116 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `bytes` | macro | `linux/netfilter/xt_sctp` | - | - | - | 49 | 2 | 110 | 11 |
| `struct file_operations` | type | `linux/fs` | - | - | - | 35 | 2 | 61 | 11 |
| `struct inode` | type | `linux/fs` | - | - | - | 30 | 2 | 9 | 52 |
| `capable` | func | `linux/capability` | - | - | - | 22 | 9 | 9 | 3 |
| `CAP_SYS_NICE` | macro | `linux/capability` | - | - | - | 1 | 3 | 1 | 1 |
| `get_unused_fd_flags` | func | `linux/file` | - | - | 1 | 1 | - | 1 | 3 |
| `put_unused_fd` | func | `linux/file` | - | - | 1 | 1 | - | 1 | 1 |
| `struct file` | type | `linux/fs` | - | - | - | - | 2 | 101 | 89 |
| `S_IRUGO` | macro | `linux/stat` | - | - | - | 2 | - | 71 | 6 |
| `file_inode` | inline | `linux/fs` | - | - | - | 4 | - | 68 | 5 |
| `struct workqueue_struct` | type | `linux/fs` | - | - | - | 6 | 16 | 4 | - |
| `S_IWUSR` | macro | `linux/stat` | - | - | - | 4 | - | 17 | 1 |
| `fput` | func | `linux/file` | - | - | 1 | 8 | - | - | 11 |
| `S_IRUSR` | macro | `linux/stat` | - | - | - | 4 | - | 7 | 1 |
| `PIDTYPE_PID` | enumconst | `linux/pid_types` | - | - | - | 2 | - | 2 | 2 |
| `get_task_pid` | func | `linux/pid` | - | - | - | 1 | - | 1 | 2 |
| `put_pid` | func | `linux/pid` | - | - | - | 1 | - | 2 | 1 |
| `noop_llseek` | func | `linux/fs` | - | - | - | 1 | 1 | - | 1 |
| `pid_task` | func | `linux/pid` | - | - | - | 1 | - | 1 | 1 |
| `block_size` | inline | `linux/blkdev` | - | - | - | 28 | - | 68 | - |
| `default_llseek` | func | `linux/fs` | - | - | - | 13 | - | 51 | - |
| `struct address_space` | type | `linux/fs` | - | - | - | 12 | - | - | 1 |
| `_RET_IP_` | macro | `linux/instruction_pointer` | - | - | - | 7 | - | - | 4 |
| `S_IWUGO` | macro | `linux/stat` | - | - | - | - | - | 8 | 1 |
| `poll_table` | type | `linux/poll` | - | - | - | 4 | - | - | 5 |
| `poll_wait` | inline | `linux/poll` | - | - | - | 4 | - | - | 3 |
| `CAP_SYS_ADMIN` | macro | `linux/capability` | - | - | - | 5 | - | - | 1 |
| `S_IFREG` | macro | `linux/stat` | - | - | - | - | - | 4 | 2 |
| `get_random_bytes` | func | `linux/random` | - | - | - | 2 | - | - | 3 |
| `pid_nr` | inline | `linux/pid` | - | - | - | 2 | - | 3 | - |
| `struct pid` | type | `linux/pid` | - | - | - | - | - | 2 | 2 |
| `task_pid_nr` | inline | `linux/pid` | - | - | - | 1 | - | - | 3 |
| `PIDTYPE_TGID` | enumconst | `linux/pid_types` | - | - | - | - | - | 1 | 1 |
| `close_fd` | func | `linux/fdtable` | - | - | - | - | - | 1 | 1 |
| `simple_open` | func | `linux/fs` | - | - | - | 1 | - | 1 | - |
| `struct dentry` | type | `linux/dcache` | - | - | - | - | - | 24 | - |
| `struct proc_dir_entry` | type | `linux/if_vlan` | - | - | - | - | - | - | 23 |
| `dup_fd` | func | `linux/fdtable` | - | - | - | - | - | - | 17 |
| `unregister_chrdev_region` | func | `linux/fs` | - | - | - | - | - | - | 12 |
| `DEFINE_SIMPLE_ATTRIBUTE` | macro | `linux/fs` | - | - | - | 10 | - | - | - |

### K9 — Miscelaneo (stubs: trace, kasan, hmm...) (181 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `u32` | type | `linux/raid/pq` | 2 | 4 | 36 | 5205 | 1540 | 3862 | 68 |
| `u64` | type | `linux/raid/pq` | 1 | - | 13 | 915 | 574 | 882 | 23 |
| `u16` | type | `linux/raid/pq` | 11 | - | - | 740 | 150 | 362 | 35 |
| `__entry` | macro | `trace/stages/stage3_trace_output` | - | - | 16 | 380 | 105 | 514 | 10 |
| `PAGE_SIZE` | macro | `linux/raid/pq` | - | - | 5 | 196 | 24 | 306 | 71 |
| `__field` | macro | `linux/genl_magic_func` | - | - | 8 | 168 | 46 | 252 | 2 |
| `PAGE_SHIFT` | macro | `linux/raid/pq` | - | - | 1 | 147 | 46 | 75 | 30 |
| `TP_ARGS` | macro | `linux/tracepoint` | - | - | 3 | 69 | 77 | 48 | 2 |
| `TP_PROTO` | macro | `linux/tracepoint` | - | - | 3 | 69 | 77 | 48 | 2 |
| `TP_printk` | macro | `trace/stages/stage3_trace_output` | - | - | 1 | 42 | 13 | 36 | 2 |
| `TP_STRUCT__entry` | macro | `trace/stages/stage1_struct_define` | - | - | 1 | 42 | 11 | 36 | 2 |
| `TP_fast_assign` | macro | `trace/stages/stage6_event_callback` | - | - | 1 | 42 | 11 | 36 | 2 |
| `HZ` | macro | `linux/raid/pq` | - | - | 5 | 22 | 13 | 26 | 8 |
| `struct kmem_cache` | type | `linux/fault-inject` | - | - | 1 | 12 | 3 | 3 | 3 |
| `TRACE_HEADER_MULTI_READ` | macro | `trace/define_trace` | - | - | 1 | 3 | 1 | 2 | 2 |
| `DEFINE_EVENT` | macro | `linux/tracepoint` | - | - | 2 | 27 | 64 | 12 | - |
| `TRACE_EVENT` | macro | `linux/tracepoint` | - | - | - | 36 | 1 | 32 | 2 |
| `cleanup` | type | `net/flow_offload` | - | - | 6 | 12 | - | 33 | 13 |
| `__assign_str` | macro | `trace/stages/stage6_event_callback` | - | - | 1 | 28 | - | 9 | 8 |
| `__get_str` | macro | `trace/stages/stage3_trace_output` | - | - | 1 | 28 | - | 9 | 4 |
| `__string` | macro | `trace/stages/stage1_struct_define` | - | - | 1 | 28 | - | 9 | 4 |
| `DECLARE_EVENT_CLASS` | macro | `linux/tracepoint` | - | - | 1 | 5 | 10 | 4 | - |
| `__array` | macro | `linux/genl_magic_func` | - | - | - | 11 | 1 | 1 | - |
| `struct mmu_interval_notifier` | type | `linux/mmu_notifier` | - | - | - | 2 | 2 | 8 | - |
| `outb` | macro | `linux/logic_pio` | 1 | - | - | 3 | - | - | 1 |
| `inb` | macro | `linux/logic_pio` | 1 | - | - | 2 | - | - | 1 |
| `mmu_interval_notifier_insert` | func | `linux/mmu_notifier` | - | - | - | 1 | 1 | 2 | - |
| `mmu_interval_read_begin` | func | `linux/mmu_notifier` | - | - | - | 1 | 2 | 1 | - |
| `mmu_interval_read_retry` | inline | `linux/mmu_notifier` | - | - | - | 2 | 1 | 1 | - |
| `mmu_interval_set_seq` | inline | `linux/mmu_notifier` | - | - | - | 1 | 1 | 2 | - |
| `mmu_notifier_range_blockable` | inline | `linux/mmu_notifier` | - | - | - | 1 | 1 | 2 | - |
| `struct mmu_interval_notifier_ops` | type | `linux/mmu_notifier` | - | - | - | 1 | 1 | 2 | - |
| `struct mmu_notifier_range` | type | `linux/mmu_notifier` | - | - | - | 1 | 1 | 2 | - |
| `mmu_interval_notifier_remove` | func | `linux/mmu_notifier` | - | - | - | 1 | 1 | 1 | - |
| `after` | macro | `net/tcp` | - | - | - | 8 | - | 20 | - |
| `before` | inline | `net/tcp` | - | - | - | 6 | - | 19 | - |
| `struct perf_event` | type | `linux/perf_event` | - | - | - | 18 | - | 6 | - |
| `BITS_PER_LONG` | macro | `linux/raid/pq` | - | - | - | 16 | - | 2 | - |
| `cpu_relax` | inline | `linux/raid/pq` | - | - | - | 5 | - | 6 | - |
| `struct hw_perf_event` | type | `linux/perf_event` | - | - | - | 1 | - | 6 | - |

### K9-drm — DRM core y helpers (shim propio con la opcion A) (2292 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `DRIVER_MODESET` | enumconst | `drm/drm_drv` | 1 | 1 | 2 | 2 | 2 | 1 | 18 |
| `DRIVER_ATOMIC` | enumconst | `drm/drm_drv` | 1 | 1 | 3 | 3 | 2 | 3 | 1 |
| `DRIVER_GEM` | enumconst | `drm/drm_drv` | 1 | 1 | 1 | 1 | 1 | 3 | 1 |
| `drm_dev_unplug` | func | `drm/drm_drv` | 1 | 1 | 1 | 1 | 1 | 3 | 1 |
| `drm_dev_register` | func | `drm/drm_drv` | 1 | 1 | 1 | 1 | 1 | 2 | 1 |
| `drm_mode_config_reset` | func | `drm/drm_mode_config` | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| `struct drm_framebuffer` | type | `drm/drm_framebuffer` | 1 | 2 | 4 | - | 9 | 40 | 9 |
| `drm_dev_exit` | func | `drm/drm_drv` | 3 | 2 | 2 | 1 | 1 | 37 | - |
| `drm_dev_enter` | func | `drm/drm_drv` | 3 | 2 | 2 | 1 | 1 | 34 | - |
| `drm_connector_helper_add` | inline | `drm/drm_modeset_helper_vtables` | 1 | 1 | 1 | 10 | - | 14 | 1 |
| `struct drm_connector_helper_funcs` | type | `drm/drm_modeset_helper_vtables` | 1 | 1 | 1 | 10 | - | 12 | 1 |
| `drm_helper_probe_single_connector_modes` | func | `drm/drm_probe_helper` | 1 | 1 | 1 | 10 | - | 9 | 1 |
| `struct drm_connector_funcs` | type | `drm/drm_connector` | 1 | 1 | 1 | 10 | - | 9 | 1 |
| `drm_connector_cleanup` | func | `drm/drm_connector` | 1 | 1 | 1 | 6 | - | 7 | 2 |
| `struct drm_driver` | type | `drm/drm_drv` | 1 | 2 | 2 | 2 | - | 8 | 1 |
| `drm_connector_init` | func | `drm/drm_connector` | 1 | 1 | 1 | 4 | - | 2 | 1 |
| `struct drm_mode_config_funcs` | type | `drm/drm_mode_config` | 1 | 1 | 1 | 1 | - | 4 | 1 |
| `drm_aperture_remove_conflicting_pci_framebuffers` | func | `drm/drm_aperture` | 1 | - | 1 | 1 | 1 | 1 | 2 |
| `DRM_ERROR` | macro | `drm/drm_print` | 8 | - | 9 | 9 | - | 950 | 1 |
| `DRM_DEBUG` | macro | `drm/drm_print` | 1 | - | 10 | 20 | - | 411 | 1 |
| `struct drm_connector` | type | `drm/drm_connector` | 3 | 3 | 6 | - | - | 245 | 38 |
| `DRM_INFO` | macro | `drm/drm_print` | 3 | - | 8 | 5 | - | 225 | 1 |
| `DRM_IOCTL_DEF_DRV` | macro | `drm/drm_ioctl` | - | - | 11 | 59 | 11 | 16 | 26 |
| `DRM_RENDER_ALLOW` | enumconst | `drm/drm_ioctl` | - | - | 11 | 33 | 11 | 15 | 23 |
| `struct drm_plane_state` | type | `drm/drm_plane` | 3 | 3 | 8 | - | - | 54 | 25 |
| `struct drm_crtc_state` | type | `drm/drm_crtc` | 1 | 1 | 2 | - | - | 50 | 25 |
| `struct drm_atomic_state` | type | `drm/drm_atomic` | - | 3 | 7 | - | 1 | 60 | 1 |
| `enum drm_mode_status` | type | `drm/drm_modes` | - | 1 | 1 | 45 | - | 6 | 1 |
| `DRM_FORMAT_XRGB8888` | macro | `drm/drm_fourcc` | 3 | - | 1 | 38 | - | 10 | 1 |
| `drm_gem_object_put` | inline | `drm/drm_gem` | - | - | 9 | 2 | 5 | 34 | 2 |
| `DRM_FORMAT_MOD_LINEAR` | macro | `drm/drm_fourcc` | - | 1 | - | 29 | 1 | 6 | 2 |
| `drm_atomic_get_new_crtc_state` | inline | `drm/drm_atomic` | - | 1 | 1 | 8 | - | 14 | 2 |
| `drmm_kzalloc` | inline | `drm/drm_managed` | 1 | 2 | 1 | 7 | 9 | - | - |
| `DRM_PLANE_TYPE_PRIMARY` | enumconst | `drm/drm_plane` | - | 1 | 3 | 6 | - | 5 | 3 |
| `drm_atomic_helper_connector_destroy_state` | func | `drm/drm_atomic_state_helper` | 1 | 1 | 1 | 10 | - | 4 | - |
| `struct drm_crtc_funcs` | type | `drm/drm_crtc` | - | 1 | 1 | 8 | - | 6 | 1 |
| `struct drm_plane_funcs` | type | `drm/drm_plane` | - | 1 | 1 | 11 | - | 2 | 1 |
| `drm_gem_handle_create` | func | `drm/drm_gem` | - | - | 3 | 3 | 2 | 4 | 2 |
| `drm_atomic_helper_disable_plane` | func | `drm/drm_atomic_helper` | - | 1 | 1 | 8 | - | 2 | 1 |
| `drm_atomic_helper_update_plane` | func | `drm/drm_atomic_helper` | - | 1 | 1 | 8 | - | 2 | 1 |

### K9-ttm — TTM, drm_buddy, drm_exec, gpuvm (174 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `ttm_bo_type_kernel` | enumconst | `drm/ttm/ttm_bo` | - | - | - | 2 | 20 | 23 | - |
| `TTM_PL_VRAM` | macro | `drm/ttm/ttm_placement` | - | - | - | 1 | 1 | 36 | - |
| `struct drm_buddy_block` | type | `drm/drm_buddy` | - | - | - | 5 | 14 | 19 | - |
| `ttm_bo_validate` | func | `drm/ttm/ttm_bo` | - | - | - | 7 | 3 | 27 | - |
| `TTM_PL_TT` | macro | `drm/ttm/ttm_placement` | - | - | - | 1 | 1 | 34 | - |
| `ttm_manager_type` | inline | `drm/ttm/ttm_device` | - | - | - | 5 | 18 | 11 | - |
| `struct ttm_placement` | type | `drm/ttm/ttm_placement` | - | - | - | 10 | 10 | 5 | - |
| `TTM_PL_FLAG_CONTIGUOUS` | macro | `drm/ttm/ttm_placement` | - | - | - | 3 | 13 | 8 | - |
| `TTM_PL_SYSTEM` | macro | `drm/ttm/ttm_placement` | - | - | - | 4 | 1 | 18 | - |
| `enum ttm_bo_type` | type | `drm/ttm/ttm_bo` | - | - | - | 1 | 17 | 4 | - |
| `ttm_bo_type_device` | enumconst | `drm/ttm/ttm_bo` | - | - | - | 1 | 6 | 7 | - |
| `drm_buddy_block_offset` | inline | `drm/drm_buddy` | - | - | - | 4 | 8 | 1 | - |
| `ttm_bo_move_null` | inline | `drm/ttm/ttm_bo` | - | - | - | 2 | 8 | 3 | - |
| `ttm_resource_manager_set_used` | inline | `drm/ttm/ttm_resource` | - | - | - | 2 | 4 | 6 | - |
| `ttm_set_driver_manager` | inline | `drm/ttm/ttm_device` | - | - | - | 2 | 4 | 6 | - |
| `ttm_cached` | enumconst | `drm/ttm/ttm_caching` | - | - | - | 4 | 2 | 5 | - |
| `ttm_resource_fini` | func | `drm/ttm/ttm_resource` | - | - | - | 2 | 4 | 5 | - |
| `TTM_TT_FLAG_EXTERNAL` | macro | `drm/ttm/ttm_tt` | - | - | - | 2 | 3 | 4 | - |
| `ttm_tt_populate` | func | `drm/ttm/ttm_tt` | - | - | - | 5 | 3 | 1 | - |
| `ttm_write_combined` | enumconst | `drm/ttm/ttm_caching` | - | - | - | 2 | 5 | 2 | - |
| `drm_buddy_free_list` | func | `drm/drm_buddy` | - | - | - | 3 | 2 | 3 | - |
| `ttm_resource_manager_evict_all` | func | `drm/ttm/ttm_resource` | - | - | - | 1 | 3 | 4 | - |
| `TTM_PL_FLAG_TOPDOWN` | macro | `drm/ttm/ttm_placement` | - | - | - | 3 | 2 | 2 | - |
| `struct ttm_resource_manager_func` | type | `drm/ttm/ttm_resource` | - | - | - | 1 | 2 | 4 | - |
| `ttm_resource_free` | func | `drm/ttm/ttm_resource` | - | - | - | 1 | 2 | 4 | - |
| `ttm_resource_manager_cleanup` | inline | `drm/ttm/ttm_resource` | - | - | - | 2 | 2 | 3 | - |
| `struct ttm_range_mgr_node` | type | `drm/ttm/ttm_range_manager` | - | - | - | 1 | 1 | 4 | - |
| `ttm_resource_init` | func | `drm/ttm/ttm_resource` | - | - | - | 1 | 2 | 3 | - |
| `ttm_resource_manager_init` | func | `drm/ttm/ttm_resource` | - | - | - | 1 | 2 | 3 | - |
| `DRM_BUDDY_RANGE_ALLOCATION` | macro | `drm/drm_buddy` | - | - | - | 2 | 1 | 2 | - |
| `TTM_PL_FLAG_TEMPORARY` | macro | `drm/ttm/ttm_placement` | - | - | - | 1 | 2 | 2 | - |
| `drm_buddy_alloc_blocks` | func | `drm/drm_buddy` | - | - | - | 2 | 1 | 2 | - |
| `enum ttm_caching` | type | `drm/ttm/ttm_caching` | - | - | - | 2 | 1 | 1 | - |
| `ttm_bo_eviction_valuable` | func | `drm/ttm/ttm_bo` | - | - | - | 1 | 1 | 2 | - |
| `ttm_bo_move_accel_cleanup` | func | `drm/ttm/ttm_bo` | - | - | - | 1 | 1 | 2 | - |
| `ttm_pool_free` | func | `drm/ttm/ttm_pool` | - | - | - | 1 | 2 | 1 | - |
| `DRM_BUDDY_TOPDOWN_ALLOCATION` | macro | `drm/drm_buddy` | - | - | - | 1 | 1 | 1 | - |
| `TTM_BO_VM_NUM_PREFAULT` | macro | `drm/ttm/ttm_bo` | - | - | - | 1 | 1 | 1 | - |
| `drm_buddy_fini` | func | `drm/drm_buddy` | - | - | - | 1 | 1 | 1 | - |
| `drm_buddy_init` | func | `drm/drm_buddy` | - | - | - | 1 | 1 | 1 | - |

### K9-sched — drm/gpu_scheduler (45 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `struct drm_sched_entity` | type | `drm/gpu_scheduler` | - | - | - | - | 2 | 32 | - |
| `struct drm_gpu_scheduler` | type | `drm/gpu_scheduler` | - | - | - | - | 4 | 21 | - |
| `struct drm_sched_job` | type | `drm/gpu_scheduler` | - | - | - | - | 8 | 11 | - |
| `drm_sched_entity_init` | func | `drm/gpu_scheduler` | - | - | - | - | 2 | 7 | - |
| `struct drm_sched_fence` | type | `drm/gpu_scheduler` | - | - | - | - | 1 | 8 | - |
| `drm_sched_job_add_dependency` | func | `drm/gpu_scheduler` | - | - | - | - | 3 | 3 | - |
| `struct drm_sched_backend_ops` | type | `drm/gpu_scheduler` | - | - | - | - | 4 | 2 | - |
| `drm_sched_job_cleanup` | func | `drm/gpu_scheduler` | - | - | - | - | 2 | 3 | - |
| `drm_sched_entity_fini` | func | `drm/gpu_scheduler` | - | - | - | - | 2 | 2 | - |
| `drm_sched_fini` | func | `drm/gpu_scheduler` | - | - | - | - | 3 | 1 | - |
| `drm_sched_job_add_resv_dependencies` | func | `drm/gpu_scheduler` | - | - | - | - | 2 | 2 | - |
| `drm_sched_stop` | func | `drm/gpu_scheduler` | - | - | - | - | 1 | 3 | - |
| `drm_sched_entity_push_job` | func | `drm/gpu_scheduler` | - | - | - | - | 1 | 2 | - |
| `drm_sched_init` | func | `drm/gpu_scheduler` | - | - | - | - | 2 | 1 | - |
| `drm_sched_job_arm` | func | `drm/gpu_scheduler` | - | - | - | - | 1 | 2 | - |
| `drm_sched_wqueue_start` | func | `drm/gpu_scheduler` | - | - | - | - | 1 | 2 | - |
| `drm_sched_wqueue_stop` | func | `drm/gpu_scheduler` | - | - | - | - | 1 | 2 | - |
| `DRM_GPU_SCHED_STAT_NOMINAL` | enumconst | `drm/gpu_scheduler` | - | - | - | - | 1 | 1 | - |
| `drm_sched_job_init` | func | `drm/gpu_scheduler` | - | - | - | - | 1 | 1 | - |
| `enum drm_gpu_sched_stat` | type | `drm/gpu_scheduler` | - | - | - | - | 1 | 1 | - |
| `drm_sched_fault` | func | `drm/gpu_scheduler` | - | - | - | - | - | 17 | - |
| `drm_sched_entity_destroy` | func | `drm/gpu_scheduler` | - | - | - | - | - | 9 | - |
| `DRM_SCHED_PRIORITY_NORMAL` | enumconst | `drm/gpu_scheduler` | - | - | - | - | - | 8 | - |
| `to_drm_sched_fence` | func | `drm/gpu_scheduler` | - | - | - | - | - | 6 | - |
| `drm_sched_entity_error` | func | `drm/gpu_scheduler` | - | - | - | - | - | 3 | - |
| `drm_sched_entity_modify_sched` | func | `drm/gpu_scheduler` | - | - | - | - | - | 3 | - |
| `drm_sched_start` | func | `drm/gpu_scheduler` | - | - | - | - | - | 3 | - |
| `DRM_SCHED_PRIORITY_HIGH` | enumconst | `drm/gpu_scheduler` | - | - | - | - | - | 2 | - |
| `DRM_SCHED_PRIORITY_KERNEL` | enumconst | `drm/gpu_scheduler` | - | - | - | - | - | 2 | - |
| `DRM_SCHED_PRIORITY_LOW` | enumconst | `drm/gpu_scheduler` | - | - | - | - | - | 2 | - |
| `DRM_GPU_SCHED_STAT_ENODEV` | enumconst | `drm/gpu_scheduler` | - | - | - | - | - | 1 | - |
| `DRM_SCHED_FENCE_DONT_PIPELINE` | macro | `drm/gpu_scheduler` | - | - | - | - | - | 1 | - |
| `DRM_SCHED_PRIORITY_COUNT` | enumconst | `drm/gpu_scheduler` | - | - | - | - | - | 1 | - |
| `MAX_WAIT_SCHED_ENTITY_Q_EMPTY` | macro | `drm/gpu_scheduler` | - | - | - | - | - | 1 | - |
| `drm_sched_entity_flush` | func | `drm/gpu_scheduler` | - | - | - | - | - | 1 | - |
| `drm_sched_entity_set_priority` | func | `drm/gpu_scheduler` | - | - | - | - | - | 1 | - |
| `drm_sched_increase_karma` | func | `drm/gpu_scheduler` | - | - | - | - | - | 1 | - |
| `drm_sched_invalidate_job` | inline | `drm/gpu_scheduler` | - | - | - | - | 1 | - | - |
| `drm_sched_pick_best` | func | `drm/gpu_scheduler` | - | - | - | - | - | 1 | - |
| `drm_sched_policy` | var | `drm/gpu_scheduler` | - | - | - | - | 1 | - | - |

### K? — Sin clasificar (25 simbolos)

| Simbolo | Tipo | Cabecera | bochs | simple | virtio | i915 | xe | amdgpu | nvidia |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `free` | macro | `linux/decompress/mm` | - | - | 2 | 17 | 10 | 16 | 15 |
| `mode_set` | inline | `linux/hisi_acc_qm` | - | - | 1 | 8 | - | 16 | 1 |
| `__u32` | enumconst | `linux/android/binder` | - | - | - | - | - | 6 | 4 |
| `struct clk` | type | `linux/sh_clk` | - | 2 | - | - | - | - | 5 |
| `struct sysinfo` | type | `linux/sysinfo` | - | - | - | - | 1 | 5 | - |
| `struct gpio_desc` | type | `linux/synclink` | - | - | - | 4 | - | - | 1 |
| `dev_coredumpm` | func | `linux/devcoredump` | - | - | - | - | 1 | 1 | - |
| `dout` | macro | `linux/ceph/ceph_debug` | - | - | - | - | - | 142 | - |
| `RESERVED` | enumconst | `linux/habanalabs/hl_boot_if` | - | - | - | - | - | 64 | - |
| `OFFSET` | macro | `linux/kbuild` | - | - | - | - | - | 43 | - |
| `MASK` | macro | `linux/soc/ti/knav_dma` | - | - | - | - | - | 17 | - |
| `PSI` | enumconst | `linux/scmi_protocol` | - | - | - | - | - | 5 | - |
| `struct reset_control` | type | `linux/mdio` | - | - | - | - | - | - | 4 |
| `DECLARE_CRYPTO_WAIT` | macro | `linux/crypto` | - | - | - | - | - | - | 3 |
| `crypto_wait_req` | inline | `linux/crypto` | - | - | - | - | - | - | 3 |
| `signalled` | macro | `linux/sunrpc/types` | - | - | - | - | 3 | - | - |
| `struct key_preparsed_payload` | type | `linux/key-type` | - | - | - | - | - | - | 3 |
| `CRYPTO_ALG_TYPE_AEAD` | macro | `linux/crypto` | - | - | - | - | - | - | 2 |
| `CRYPTO_TFM_REQ_MAY_BACKLOG` | macro | `linux/crypto` | - | - | - | - | - | - | 2 |
| `CRYPTO_TFM_REQ_MAY_SLEEP` | macro | `linux/crypto` | - | - | - | - | - | - | 2 |
| `crypto_req_done` | func | `linux/crypto` | - | - | - | - | - | - | 2 |
| `CRYPTO_ALG_TYPE_AKCIPHER` | macro | `linux/crypto` | - | - | - | - | - | - | 1 |
| `CRYPTO_ALG_TYPE_SIG` | macro | `linux/crypto` | - | - | - | - | - | - | 1 |
| `CRYPTO_TFM_NEED_KEY` | macro | `linux/crypto` | - | - | - | - | - | - | 1 |
| `wchar_t` | type | `linux/nls` | - | - | - | - | - | - | 1 |

