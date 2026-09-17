// kernel/tests/pmm_stress.c
#include "pmm.h"
#include "serial.h"
#include "klog.h"

void pmm_self_test(void) {
    LOG_INFO("[PMM-TEST] Iniciando pruebas de estrés");
    uint64_t total = pmm_total_pages();
    uint64_t free_before = pmm_free_pages_count();
    LOG_INFO("[PMM-TEST] Total pages: %lu", (unsigned long)total);
    LOG_INFO("[PMM-TEST] Free before: %lu", (unsigned long)free_before);

    // Allocate 64 single pages
    uint64_t pages[64];
    int i;
    for (i = 0; i < 64; i++) {
        pages[i] = pmm_alloc_page();
        if (!pages[i]) {
            LOG_ERR("[PMM-TEST] fallo al asignar pagina simple #%d", i);
            break;
        }
    }
    LOG_INFO("[PMM-TEST] Asignadas paginas simples: %d", i);

    // Free every other page
    for (int j = 0; j < i; j+=2) {
        pmm_free_page(pages[j]);
    }
    LOG_INFO("[PMM-TEST] Liberadas paginas impares");

    // Try power-of-two contiguous allocations to exercise buddy
    uint64_t a4 = pmm_alloc_pages(4);
    LOG_INFO("[PMM-TEST] Alloc 4 pages @ %p", (void *)a4);
    uint64_t a8 = pmm_alloc_pages(8);
    LOG_INFO("[PMM-TEST] Alloc 8 pages @ %p", (void *)a8);
    uint64_t a16 = pmm_alloc_pages(16);
    LOG_INFO("[PMM-TEST] Alloc 16 pages @ %p", (void *)a16);

    if (a8) pmm_free_pages(a8,8);
    if (a4) pmm_free_pages(a4,4);
    if (a16) pmm_free_pages(a16,16);

    LOG_INFO("[PMM-TEST] Liberados bloques contiguos");

    uint64_t free_after = pmm_free_pages_count();
    LOG_INFO("[PMM-TEST] Free after: %lu", (unsigned long)free_after);
    LOG_INFO("[PMM-TEST] Prueba completada");
}
