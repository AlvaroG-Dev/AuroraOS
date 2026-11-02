// kernel/efi_rt.c
#include "efi_rt.h"
#include "cmdline.h"
#include "klog.h"
#include "paging.h"
#include "pmm.h"
#include "string.h"

#define EFI_MEMORY_RUNTIME (1ULL << 63)
#define EFI_MEMORY_UC 0x1ULL
#define EfiMemoryMappedIO 11
#define EfiMemoryMappedIOPortSpace 12

// Offset de ResetSystem en EFI_RUNTIME_SERVICES (cabecera de 24 bytes + 10
// punteros).
#define EFI_RT_RESET_SYSTEM_OFF 0x68

// Tope de paginas de 4 KiB mapeadas (256 MiB): descarta regiones absurdas.
#define EFI_RT_MAX_PAGES (65536ULL)
#define EFI_RT_MAX_RANGES 64

typedef struct {
  uint64_t phys;
  uint64_t pages;
  uint32_t type;
  uint64_t attr;
} rt_range_t;

typedef void(__attribute__((ms_abi)) * efi_reset_fn)(uint32_t type,
                                                     uint64_t status,
                                                     uint64_t data_size,
                                                     void *data);

static uint64_t g_rt_phys;     // EFI_RUNTIME_SERVICES (fisica)
static uint64_t *g_rt_pml4;    // PML4 virtual (ventana de RAM) con identidad
static uint64_t g_rt_pml4_phys;
static rt_range_t g_ranges[EFI_RT_MAX_RANGES];
static int g_range_count;
static int g_ready;

int efi_rt_available(void) { return g_ready; }

static int map_ranges(void) {
  uint64_t total = 0;
  for (int i = 0; i < g_range_count; i++) {
    rt_range_t *r = &g_ranges[i];
    if (r->pages > EFI_RT_MAX_PAGES - total) {
      LOG_WARN("[EFI-RT] region 0x%lx (%lu paginas) demasiado grande, omitida",
               (unsigned long)r->phys, (unsigned long)r->pages);
      continue;
    }
    total += r->pages;
    uint64_t flags = PTE_PRESENT | PTE_WRITABLE;
    if (r->type == EfiMemoryMappedIO || r->type == EfiMemoryMappedIOPortSpace ||
        (r->attr & EFI_MEMORY_UC))
      flags |= PTE_NOCACHE | PTE_WRITETHRU;
    for (uint64_t p = 0; p < r->pages; p++) {
      uint64_t a = r->phys + p * PAGE_SIZE;
      if (paging_map_page_in(g_rt_pml4, a, a, flags) != 0)
        return -1;
    }
  }
  return 0;
}

static int g_collected;

int efi_rt_collect(uint64_t runtime_services_phys, const void *memmap,
                   uint64_t memmap_size, uint64_t desc_size) {
  char opt[16];
  if (cmdline_get("efi", opt, sizeof(opt)) && !strcmp(opt, "noruntime")) {
    LOG_INFO("[EFI-RT] desactivado por efi=noruntime");
    return -1;
  }
  if (!runtime_services_phys) {
    LOG_INFO("[EFI-RT] sin RuntimeServices (arranque BIOS)");
    return -1;
  }
  if (!memmap || desc_size < 40 || memmap_size % desc_size != 0)
    return -1;

  const uint8_t *p = memmap;
  for (uint64_t i = 0; i < memmap_size; i += desc_size) {
    uint32_t type = *(const uint32_t *)(p + i);
    uint64_t phys = *(const uint64_t *)(p + i + 8);
    uint64_t pages = *(const uint64_t *)(p + i + 24);
    uint64_t attr = *(const uint64_t *)(p + i + 32);
    if (!(attr & EFI_MEMORY_RUNTIME) || !pages)
      continue;
    if (g_range_count >= EFI_RT_MAX_RANGES) {
      LOG_WARN("[EFI-RT] demasiadas regiones runtime, se descartan las demas");
      break;
    }
    g_ranges[g_range_count++] =
        (rt_range_t){.phys = phys, .pages = pages, .type = type, .attr = attr};
  }

  g_rt_phys = runtime_services_phys;
  g_collected = 1;
  return 0;
}

int efi_rt_init(void) {
  if (!g_collected)
    return -1;
  uint64_t pml4_phys = pmm_alloc_page();
  if (!pml4_phys)
    return -1;
  g_rt_pml4 = phys_to_virt(pml4_phys);
  memset(g_rt_pml4, 0, PAGE_SIZE);
  g_rt_pml4_phys = pml4_phys;

  if (map_ranges() != 0) {
    LOG_ERR("[EFI-RT] no pude construir el mapa identidad");
    return -1;
  }
  g_ready = 1;
  LOG_INFO("[EFI-RT] RuntimeServices=0x%lx, %d regiones runtime en identidad",
           (unsigned long)g_rt_phys, g_range_count);
  return 0;
}

void efi_rt_reset(int type) {
  if (!g_ready)
    return;
  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  uint64_t *cur = phys_to_virt(cr3 & PTE_FRAME);

  // La mitad alta (kernel, ventana de RAM, pilas) se toma del PML4 actual
  // en este instante; la baja es la identidad de las regiones runtime.
  for (int i = 256; i < 512; i++)
    g_rt_pml4[i] = cur[i];

  // Puntero a ResetSystem: se lee con la ventana de RAM antes de cambiar.
  efi_reset_fn fn =
      *(efi_reset_fn *)phys_to_virt(g_rt_phys + EFI_RT_RESET_SYSTEM_OFF);
  LOG_INFO("[EFI-RT] ResetSystem(%d) @ %p", type, (void *)fn);

  __asm__ volatile("cli");
  __asm__ volatile("mov %0, %%cr3" : : "r"(g_rt_pml4_phys) : "memory");
  fn((uint32_t)type, 0, 0, NULL);
  __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}
