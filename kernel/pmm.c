// kernel/pmm.c
#include "pmm.h"
#include "serial.h"

extern uint8_t _kernel_end;

static uint8_t *bitmap = 0;
static uint64_t max_blocks = 0;
static uint64_t used_blocks = 0;
static uint64_t last_alloc_bit = 0; // Pista de búsqueda para Next-Fit

#define EFI_CONVENTIONAL_MEMORY 7

void pmm_init(uint64_t memmap, uint64_t memmap_size, uint64_t memmap_desc_size) {
    if (!memmap || memmap_size == 0) return;

    uint8_t *ptr = (uint8_t*)memmap;
    uint64_t max_phys_addr = 0;

    // Pasada 1: Encontrar la memoria fisica maxima
    for (uint64_t i = 0; i < memmap_size; i += memmap_desc_size) {
        uint64_t phys = *(uint64_t*)(ptr + i + 8);
        uint64_t pages = *(uint64_t*)(ptr + i + 24);
        uint64_t end_addr = phys + (pages * PAGE_SIZE);
        if (end_addr > max_phys_addr) {
            max_phys_addr = end_addr;
        }
    }

    max_blocks = max_phys_addr / PAGE_SIZE;
    uint64_t bitmap_size = max_blocks / 8;
    if (max_blocks % 8 != 0) bitmap_size++;

    // Pasada 1.5: Buscar memoria fisica libre (< 4GB por el identity-map del bootloader) para el bitmap
    uint64_t bitmap_phys = 0;
    for (uint64_t i = 0; i < memmap_size; i += memmap_desc_size) {
        uint32_t type = *(uint32_t*)(ptr + i + 0);
        uint64_t phys = *(uint64_t*)(ptr + i + 8);
        uint64_t pages = *(uint64_t*)(ptr + i + 24);
        uint64_t size = pages * PAGE_SIZE;

        if (type == EFI_CONVENTIONAL_MEMORY && size >= bitmap_size) {
            if ((phys + bitmap_size) < 0x100000000ULL) { // Debe estar por debajo de los 4GB
                bitmap_phys = phys;
                break;
            }
        }
    }

    if (bitmap_phys == 0) {
        serial_puts("[PMM] ERROR: No hay RAM por debajo de 4GB para el bitmap!\n");
        return;
    }

    // Usar el puntero fisico directamente (aprovechando el identity-map de 4GB)
    bitmap = (uint8_t*)bitmap_phys;

    // Llenar bitmap de 1s (Ocupado por defecto)
    for (uint64_t i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0xFF;
    }
    used_blocks = max_blocks;

    // Pasada 2: Marcar memoria convencional como libre (0)
    for (uint64_t i = 0; i < memmap_size; i += memmap_desc_size) {
        uint32_t type = *(uint32_t*)(ptr + i + 0);
        uint64_t phys = *(uint64_t*)(ptr + i + 8);
        uint64_t pages = *(uint64_t*)(ptr + i + 24);

        // Solo liberamos RAM usable. El Kernel fue alojado por UEFI como EfiLoaderData,
        // asi que nunca entrara aqui y seguira marcado como Ocupado (1). Magia!
        if (type == EFI_CONVENTIONAL_MEMORY) {
            uint64_t start_bit = phys / PAGE_SIZE;
            for (uint64_t b = 0; b < pages; b++) {
                BITMAP_CLEAR(bitmap, start_bit + b);
                used_blocks--;
            }
        }
    }

    // Proteger pagina 0 (Null)
    BITMAP_SET(bitmap, 0);
    used_blocks++;

    // Proteger la memoria donde hemos puesto el bitmap
    uint64_t bmp_start_bit = bitmap_phys / PAGE_SIZE;
    uint64_t bmp_end_bit = (bitmap_phys + bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t b = bmp_start_bit; b < bmp_end_bit; b++) {
        if (!BITMAP_TEST(bitmap, b)) {
            BITMAP_SET(bitmap, b);
            used_blocks++;
        }
    }

    serial_puts("[PMM] Bitmap inicializado en 0x");
    serial_hex(bitmap_phys);
    serial_puts(". Max RAM: ");
    serial_putn(max_phys_addr / (1024*1024), 10, 0);
    serial_puts(" MB, Libres: ");
    serial_putn((max_blocks - used_blocks) * 4 / 1024, 10, 0);
    serial_puts(" MB\n");
}

uint64_t pmm_alloc_page(void) {
    uint64_t start_bit = last_alloc_bit;

    // Pasada 1: Desde el último bit asignado hasta el final de la RAM
    for (uint64_t bit = start_bit; bit < max_blocks; bit++) {
        if (!BITMAP_TEST(bitmap, bit)) {
            BITMAP_SET(bitmap, bit);
            used_blocks++;
            last_alloc_bit = bit + 1;
            return bit * PAGE_SIZE;
        }
    }

    // Pasada 2: Si no hubo espacio al final, buscar desde el principio hasta start_bit
    for (uint64_t bit = 0; bit < start_bit; bit++) {
        if (!BITMAP_TEST(bitmap, bit)) {
            BITMAP_SET(bitmap, bit);
            used_blocks++;
            last_alloc_bit = bit + 1;
            return bit * PAGE_SIZE;
        }
    }

    serial_puts("[PMM] ERROR CRITICO: Memoria fisica agotada!\n");
    return 0; // Out of memory
}

void pmm_free_page(uint64_t phys_addr) {
    if (phys_addr == 0) return; // Nunca liberar la página nula
    uint64_t bit = phys_addr / PAGE_SIZE;

    if (bit < max_blocks && BITMAP_TEST(bitmap, bit)) {
        BITMAP_CLEAR(bitmap, bit);
        used_blocks--;

        // Si liberamos una página con índice menor a la pista actual,
        // movemos el puntero hacia atrás para reutilizarla inmediatamente.
        if (bit < last_alloc_bit) {
            last_alloc_bit = bit;
        }
    }
}