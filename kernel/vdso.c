#include "vdso.h"
#include "klog.h"
#include "paging.h"
#include "pmm.h"
#include "rtc.h"
#include "string.h"
#include "time.h"
#include "vdso/vdso_defs.h"


extern const uint8_t vdso_blob_start[];
extern const uint8_t vdso_blob_end[];

static uint64_t g_vvar_phys = 0;
static uint64_t g_vdso_phys = 0;
static struct vdso_vvar *g_vvar = NULL;
static int g_vdso_ready = 0;
static int64_t g_epoch_offset_ns = 0;

void vdso_init(void) {
  if (g_vdso_ready)
    return;

  // --- Pagina vvar ---
  g_vvar_phys = pmm_alloc_page();
  if (!g_vvar_phys) {
    LOG_ERR("[VDSO] sin pagina para vvar");
    return;
  }
  g_vvar = (struct vdso_vvar *)phys_to_virt(g_vvar_phys);
  memset(g_vvar, 0, PAGE_SIZE);
  g_vvar->clock_mode = 1;
  g_vvar->ticks_per_sec = KERNEL_HZ;

  int64_t epoch = rtc_get_epoch();
  if (epoch > 0) {
    // real_ns = mono_ns + offset.
    // En el instante del init: tick_count = t0, real = epoch * 1e9.
    // offset = epoch * 1e9 - t0 * 1e6.
    g_epoch_offset_ns = epoch * 1000000000LL - (int64_t)tick_count * 1000000LL;
  } else {
    LOG_WARN("[VDSO] RTC no disponible, clock_mode=0");
    g_vvar->clock_mode = 0;
  }

  // --- Pagina vdso ---
  g_vdso_phys = pmm_alloc_page();
  if (!g_vdso_phys) {
    LOG_ERR("[VDSO] sin pagina para vdso");
    pmm_free_page(g_vvar_phys);
    g_vvar_phys = 0;
    g_vvar = NULL;
    return;
  }
  uint8_t *vdso_page = (uint8_t *)phys_to_virt(g_vdso_phys);
  memset(vdso_page, 0, PAGE_SIZE);
  size_t blob_size = (size_t)(vdso_blob_end - vdso_blob_start);
  if (blob_size > PAGE_SIZE) {
    LOG_ERR("[VDSO] blob demasiado grande: %lu", (unsigned long)blob_size);
    pmm_free_page(g_vdso_phys);
    pmm_free_page(g_vvar_phys);
    g_vdso_phys = g_vvar_phys = 0;
    g_vvar = NULL;
    return;
  }
  memcpy(vdso_page, vdso_blob_start, blob_size);

  vdso_update_clock();
  g_vdso_ready = 1;

  LOG_INFO("[VDSO] listo: blob=%lu B, vvar=%p vdso=%p clock_mode=%u",
           (unsigned long)blob_size, (void *)VVAR_VMA_BASE,
           (void *)VDSO_VMA_BASE, g_vvar->clock_mode);
}

uint64_t vdso_map_in(uint64_t *pml4) {
  if (!g_vdso_ready)
    return 0;
  if (paging_map_page_in(pml4, VVAR_VMA_BASE, g_vvar_phys,
                         PTE_USER | PTE_PRESENT | PTE_NX | PTE_SPECIAL) != 0)
    return 0;
  if (paging_map_page_in(pml4, VDSO_VMA_BASE, g_vdso_phys,
                         PTE_USER | PTE_PRESENT | PTE_SPECIAL) != 0)
    return 0;
  return VDSO_VMA_BASE;
}

void vdso_update_clock(void) {
  if (!g_vvar)
    return;

  uint64_t mono_ns = tick_count * 1000000ULL;
  uint64_t real_ns =
      (g_epoch_offset_ns > 0) ? mono_ns + (uint64_t)g_epoch_offset_ns : mono_ns;

  uint32_t s = g_vvar->seq;
  g_vvar->seq = s + 1;
  __sync_synchronize();
  g_vvar->mono_ns = mono_ns;
  g_vvar->real_ns = real_ns;
  __sync_synchronize();
  g_vvar->seq = s + 2;
}