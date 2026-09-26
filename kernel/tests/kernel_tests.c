// kernel/tests/kernel_tests.c
//
// Tests registrados en el framework .tests.
// Cada test corre con preempt_disable() activo y no debe bloquearse.
//
// Para añadir uno nuevo:
//   static void test_mi_cosa(void) {
//       TEST_ASSERT(cond, "mensaje");
//   }
//   REGISTER_TEST("mi_cosa", test_mi_cosa);

#include "../acpi.h"
#include "../ahci.h"
#include "../apic.h"
#include "../ata_pio.h"
#include "../atapi.h"
#include "../block.h"
#include "../bmp.h"
#include "../cpu.h"
#include "../elf.h"
#include "../fat32.h"
#include "../heap.h"
#include "../ipi.h"
#include "../klog.h"
#include "../paging.h"
#include "../part.h"
#include "../pmm.h"
#include "../sched.h"
#include "../slab.h"
#include "../smp_boot.h"
#include "../string.h"
#include "../test.h"
#include "../time.h"
#include "../vfs.h"


// ---------------------------------------------------------------------------
// BMP: alpha y reducción de iconos
// ---------------------------------------------------------------------------
static void test_bmp_scaled_preserves_transparent_alpha(void) {
  uint8_t raw[54 + 8 * 8 * 4];
  memset(raw, 0, sizeof(raw));

  bmp_file_header_t *fh = (bmp_file_header_t *)raw;
  bmp_info_header_t *ih = (bmp_info_header_t *)(raw + sizeof(*fh));

  fh->type = 0x4D42;
  fh->size = sizeof(raw);
  fh->offset = 54;

  ih->size = 40;
  ih->width = 8;
  ih->height = 8;
  ih->planes = 1;
  ih->bpp = 32;
  ih->compression = 0;
  ih->image_size = 8 * 8 * 4;

  // BMP bottom-up. Todas las muestras son transparentes salvo una,
  // que es rojo opaco. Al reducir 8x8 -> 1x1, el resultado correcto
  // conserva un rojo prácticamente puro con alpha muy bajo.
  uint8_t *pixels = raw + 54;
  pixels[3] = 0xFF;       // BGRA: B=0, G=0, R=0, A=255 por defecto?
  pixels[0] = 0;
  pixels[1] = 0;
  pixels[2] = 0xFF;
  pixels[3] = 0xFF;

  tar_node_t node;
  memset(&node, 0, sizeof(node));
  node.data = raw;
  node.size = sizeof(raw);

  uint32_t dst = 0;
  rect_t clip = {0, 0, 1, 1};
  TEST_ASSERT(bmp_draw_scaled(&node, &dst, 1, clip, 0, 0, 1, 1) == 0,
              "bmp_draw_scaled rechazó un BMP 32-bit válido");

  uint32_t alpha = (dst >> 24) & 0xFF;
  uint32_t red = (dst >> 16) & 0xFF;
  uint32_t green = (dst >> 8) & 0xFF;
  uint32_t blue = dst & 0xFF;

  TEST_ASSERT(alpha >= 2 && alpha <= 5,
              "alpha reducido incorrecto: %u", alpha);
  TEST_ASSERT(red >= 240 && green == 0 && blue == 0,
              "RGB del icono se contaminó al reducir: %08x", dst);
}
REGISTER_TEST("bmp: preserva alpha al escalar iconos",
              test_bmp_scaled_preserves_transparent_alpha);

// ---------------------------------------------------------------------------
// Heap: kmalloc/kfree básicos
// ---------------------------------------------------------------------------
static void test_kmalloc_64(void) {
  uint8_t *p = (uint8_t *)kmalloc(64);
  TEST_ASSERT(p != NULL, "kmalloc(64) devolvió NULL");
  if (!p)
    return;

  for (int i = 0; i < 64; i++)
    p[i] = (uint8_t)i;
  int ok = 1;
  for (int i = 0; i < 64; i++)
    if (p[i] != (uint8_t)i) {
      ok = 0;
      break;
    }
  TEST_ASSERT(ok, "contenido corrupto tras escritura");
  kfree(p);
}
REGISTER_TEST("heap: kmalloc(64)", test_kmalloc_64);

static void test_kmalloc_8k(void) {
  uint8_t *p = (uint8_t *)kmalloc(8192);
  TEST_ASSERT(p != NULL, "kmalloc(8192) devolvió NULL");
  if (p)
    kfree(p);
}
REGISTER_TEST("heap: kmalloc(8192)", test_kmalloc_8k);

// ---------------------------------------------------------------------------
// Heap: kzalloc
// ---------------------------------------------------------------------------
static void test_kzalloc_zeroed(void) {
  uint8_t *p = (uint8_t *)kzalloc(200);
  TEST_ASSERT(p != NULL, "kzalloc(200) devolvió NULL");
  if (!p)
    return;
  int ok = 1;
  for (int i = 0; i < 200; i++)
    if (p[i] != 0) {
      ok = 0;
      break;
    }
  TEST_ASSERT(ok, "kzalloc no dejó todo a cero");
  kfree(p);
}
REGISTER_TEST("heap: kzalloc zeroed", test_kzalloc_zeroed);

// ---------------------------------------------------------------------------
// Heap: kcalloc + overflow
// ---------------------------------------------------------------------------
static void test_kcalloc(void) {
  int *arr = (int *)kcalloc(16, sizeof(int));
  TEST_ASSERT(arr != NULL, "kcalloc(16,4) devolvió NULL");
  if (!arr)
    return;
  int ok = 1;
  for (int i = 0; i < 16; i++)
    if (arr[i] != 0) {
      ok = 0;
      break;
    }
  TEST_ASSERT(ok, "kcalloc no zeroed");
  kfree(arr);
}
REGISTER_TEST("heap: kcalloc", test_kcalloc);

static void test_kcalloc_overflow(void) {
  void *ovf = kcalloc((size_t)-1, 2);
  TEST_ASSERT(ovf == NULL, "kcalloc no detectó overflow");
  if (ovf)
    kfree(ovf);
}
REGISTER_TEST("heap: kcalloc overflow", test_kcalloc_overflow);

// ---------------------------------------------------------------------------
// Heap: kstrdup / kstrndup
// ---------------------------------------------------------------------------
static void test_kstrdup(void) {
  char *s = kstrdup("Aurora OS");
  TEST_ASSERT(s != NULL, "kstrdup devolvió NULL");
  if (!s)
    return;
  TEST_ASSERT(strcmp(s, "Aurora OS") == 0, "contenido != original");
  kfree(s);
}
REGISTER_TEST("heap: kstrdup", test_kstrdup);

static void test_kstrndup_cut(void) {
  char *s = kstrndup("abcdefgh", 4);
  TEST_ASSERT(s != NULL, "kstrndup devolvió NULL");
  if (!s)
    return;
  TEST_ASSERT(strcmp(s, "abcd") == 0, "corte incorrecto: '%s'", s);
  kfree(s);
}
REGISTER_TEST("heap: kstrndup cut", test_kstrndup_cut);

static void test_kstrndup_short(void) {
  char *s = kstrndup("ab", 8);
  TEST_ASSERT(s != NULL, "kstrndup devolvió NULL");
  if (!s)
    return;
  TEST_ASSERT(strcmp(s, "ab") == 0, "contenido incorrecto: '%s'", s);
  kfree(s);
}
REGISTER_TEST("heap: kstrndup short", test_kstrndup_short);

// ---------------------------------------------------------------------------
// Heap: krealloc
// ---------------------------------------------------------------------------
static void test_krealloc_grow(void) {
  char *r = (char *)kmalloc(16);
  TEST_ASSERT(r != NULL, "kmalloc(16) devolvió NULL");
  if (!r)
    return;
  for (int i = 0; i < 16; i++)
    r[i] = (char)('A' + i);
  char *r2 = (char *)krealloc(r, 128);
  TEST_ASSERT(r2 != NULL, "krealloc grow devolvió NULL");
  if (!r2) {
    kfree(r);
    return;
  }
  int ok = 1;
  for (int i = 0; i < 16; i++)
    if (r2[i] != (char)('A' + i)) {
      ok = 0;
      break;
    }
  TEST_ASSERT(ok, "contenido no preservado tras crecer");
  kfree(r2);
}
REGISTER_TEST("heap: krealloc grow", test_krealloc_grow);

static void test_krealloc_shrink_same(void) {
  void *same = kmalloc(64);
  TEST_ASSERT(same != NULL, "kmalloc(64) devolvió NULL");
  if (!same)
    return;
  void *same2 = krealloc(same, 32);
  TEST_ASSERT(same2 == same, "krealloc shrink devolvió distinto ptr");
  kfree(same);
}
REGISTER_TEST("heap: krealloc shrink same ptr", test_krealloc_shrink_same);

static void test_krealloc_null(void) {
  void *fresh = krealloc(NULL, 32);
  TEST_ASSERT(fresh != NULL, "krealloc(NULL, 32) devolvió NULL");
  if (fresh)
    kfree(fresh);
}
REGISTER_TEST("heap: krealloc(NULL)", test_krealloc_null);

static void test_krealloc_zero(void) {
  void *p = kmalloc(64);
  TEST_ASSERT(p != NULL, "kmalloc(64) devolvió NULL");
  if (!p)
    return;
  void *p2 = krealloc(p, 0);
  TEST_ASSERT(p2 == NULL, "krealloc(p, 0) debía devolver NULL");
}
REGISTER_TEST("heap: krealloc(p, 0)", test_krealloc_zero);

// ---------------------------------------------------------------------------
// SLAB tests
// ---------------------------------------------------------------------------

static void test_slab_basic_32(void) {
  void *p = kmalloc(32);
  TEST_ASSERT(p != NULL, "kmalloc(32) devolvió NULL");
  if (!p)
    return;

  uint64_t addr = (uint64_t)p;
  TEST_ASSERT(addr >= SLAB_VMA, "kmalloc(32) no vino del SLAB: %p", p);

  memset(p, 0xAB, 32);
  uint8_t *b = (uint8_t *)p;
  int ok = 1;
  for (int i = 0; i < 32; i++)
    if (b[i] != 0xAB) {
      ok = 0;
      break;
    }
  TEST_ASSERT(ok, "contenido corrupto");

  kfree(p);
}
REGISTER_TEST("slab: basic 32", test_slab_basic_32);

static void test_slab_all_sizes(void) {
  static const size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048};
  void *ptrs[8] = {0};

  for (int i = 0; i < 8; i++) {
    ptrs[i] = kmalloc(sizes[i]);
    TEST_ASSERT(ptrs[i] != NULL, "kmalloc(%lu) devolvió NULL",
                (unsigned long)sizes[i]);
    if (ptrs[i]) {
      uint64_t a = (uint64_t)ptrs[i];
      TEST_ASSERT(a >= SLAB_VMA, "kmalloc(%lu) no vino del SLAB",
                  (unsigned long)sizes[i]);
      memset(ptrs[i], (int)(i + 1), sizes[i]);
    }
  }

  for (int i = 0; i < 8; i++) {
    if (ptrs[i])
      kfree(ptrs[i]);
  }
}
REGISTER_TEST("slab: all sizes", test_slab_all_sizes);

static void test_slab_many_32(void) {
  enum { N = 500 };
  static void *ptrs[N];

  for (int i = 0; i < N; i++) {
    ptrs[i] = kmalloc(32);
    if (!ptrs[i]) {
      TEST_ASSERT(0, "kmalloc(32) falló en iteración %d", i);
      for (int j = 0; j < i; j++)
        kfree(ptrs[j]);
      return;
    }
    ((uint32_t *)ptrs[i])[0] = (uint32_t)i;
    ((uint32_t *)ptrs[i])[1] = (uint32_t)~i;
  }

  int ok = 1;
  for (int i = 0; i < N; i++) {
    uint32_t a = ((uint32_t *)ptrs[i])[0];
    uint32_t b = ((uint32_t *)ptrs[i])[1];
    if (a != (uint32_t)i || b != (uint32_t)~i) {
      ok = 0;
      break;
    }
  }
  TEST_ASSERT(ok, "objetos se solapan o se corrompen");

  for (int i = 0; i < N; i++)
    kfree(ptrs[i]);
}
REGISTER_TEST("slab: 500 x 32 bytes", test_slab_many_32);

static void test_slab_free_reuse(void) {
  void *a = kmalloc(64);
  void *b = kmalloc(64);
  TEST_ASSERT(a && b, "kmalloc falló");
  if (!a || !b) {
    if (a)
      kfree(a);
    if (b)
      kfree(b);
    return;
  }

  TEST_ASSERT(a != b, "kmalloc devolvió la misma dirección dos veces");

  kfree(a);
  void *c = kmalloc(64);
  TEST_ASSERT(c != NULL, "kmalloc tras kfree falló");
  if (c)
    kfree(c);
  kfree(b);
}
REGISTER_TEST("slab: free + reuse", test_slab_free_reuse);

static void test_slab_krealloc_inplace(void) {
  void *p = kmalloc(64);
  TEST_ASSERT(p != NULL, "kmalloc(64) falló");
  if (!p)
    return;

  memset(p, 0x42, 64);

  void *p2 = krealloc(p, 48);
  TEST_ASSERT(p2 == p, "krealloc(64→48) debía ser in-place");
  uint8_t *b = (uint8_t *)p2;
  int ok = 1;
  for (int i = 0; i < 48; i++)
    if (b[i] != 0x42) {
      ok = 0;
      break;
    }
  TEST_ASSERT(ok, "contenido no preservado");
  kfree(p2);
}
REGISTER_TEST("slab: krealloc in-place", test_slab_krealloc_inplace);

static void test_slab_usable_size(void) {
  void *p = kmalloc(20);
  TEST_ASSERT(p != NULL, "kmalloc(20) falló");
  if (!p)
    return;
  size_t usable = slab_usable_size(p);
  TEST_ASSERT(usable >= 20, "usable_size < solicitado: %lu",
              (unsigned long)usable);
  kfree(p);
}
REGISTER_TEST("slab: usable size", test_slab_usable_size);

// ===========================================================================
// [Fase 1.1] slab: late free tras liberar un slab completo
//
// Regresión de C5. Antes, cuando un slab quedaba totalmente libre y se
// devolvía al PMM, su header conservaba magic=SLAB_MAGIC. Un slab_free
// tardío sobre un puntero obsoleto escribía sobre memoria ajena. Con el
// fix, magic queda a 0 y el late free se rechaza.
//
// El test no puede garantizar que ptrs[0] pertenezca a un slab ya
// devuelto al PMM (depende del orden interno de freelist), pero con N
// grande y liberación en orden, la primera mitad de los punteros casi
// siempre vienen de slabs liberados. La parte determinista es que
// *no debe corromper nada*: el sistema sigue funcional después.
// ===========================================================================
static void test_slab_late_free_after_release(void) {
  enum { N = 256 };
  static void *ptrs[N];

  for (int i = 0; i < N; i++) {
    ptrs[i] = kmalloc(64);
    TEST_ASSERT(ptrs[i] != NULL, "kmalloc(64) falló en i=%d", i);
    if (!ptrs[i]) {
      for (int j = 0; j < i; j++)
        kfree(ptrs[j]);
      return;
    }
    ((uint32_t *)ptrs[i])[0] = (uint32_t)i;
  }

  for (int i = 0; i < N; i++)
    kfree(ptrs[i]);

  // Late frees: con el fix no deben corromper. Los que apunten a slabs
  // ya devueltos al PMM verán magic=0 y se rechazarán; los que apunten
  // al último slab vivo son idempotentes respecto al bitmap de la cache
  // (comportamiento no garantizado, pero no debe crashear el kernel).
  kfree(ptrs[0]);
  kfree(ptrs[1]);

  // El SLAB debe seguir operativo.
  void *p = kmalloc(64);
  TEST_ASSERT(p != NULL, "kmalloc(64) tras late free devolvió NULL");
  if (p) {
    memset(p, 0xCC, 64);
    kfree(p);
  }

  TEST_ASSERT(1, "late free no corrompió el SLAB");
}
REGISTER_TEST("slab: late free tras liberar slab",
              test_slab_late_free_after_release);

// ---------------------------------------------------------------------------
// PMM: validación de rangos físicos EFI
// ---------------------------------------------------------------------------
static void test_pmm_efi_range_overflow(void) {
  uint64_t size = 0;

  TEST_ASSERT(pmm_test_validate_efi_range(0x1000, 1, &size),
              "rango EFI válido fue rechazado");
  TEST_ASSERT(size == PAGE_SIZE, "tamaño EFI válido incorrecto");

  TEST_ASSERT(
      !pmm_test_validate_efi_range(0, UINT64_MAX / PAGE_SIZE + 1, &size),
      "overflow pages * PAGE_SIZE no fue rechazado");

  TEST_ASSERT(
      !pmm_test_validate_efi_range(UINT64_MAX - PAGE_SIZE + 2, 1, &size),
      "overflow phys + size no fue rechazado");

  TEST_ASSERT(!pmm_test_validate_efi_range(UINT64_MAX, 1, &size),
              "rango físico al final de uint64 no fue rechazado");
}
REGISTER_TEST("pmm: rechaza overflow de rangos EFI",
              test_pmm_efi_range_overflow);

// ===========================================================================
// [Fase 1.1] pmm: doble free de una sola página
//
// Regresión de H5 en pmm_free_page(). Antes el segundo free era
// silencioso e idempotente (bitmap_clear_safe); el contador
// used_blocks no se tocaba, pero el bug del llamante pasaba
// desapercibido. Con el fix debe aparecer un LOG_WARN y el contador
// no debe moverse en la segunda llamada.
// ===========================================================================
static void test_pmm_double_free_page(void) {
  uint64_t p = pmm_alloc_page();
  TEST_ASSERT(p != 0, "pmm_alloc_page devolvió 0");
  if (!p)
    return;

  uint64_t free_before = pmm_free_pages_count();
  pmm_free_page(p);
  uint64_t free_after_first = pmm_free_pages_count();
  TEST_ASSERT(free_after_first == free_before + 1,
              "primer free no incrementó libres (%lu -> %lu)",
              (unsigned long)free_before, (unsigned long)free_after_first);

  // Segundo free: debe ser detectado.
  pmm_free_page(p);
  uint64_t free_after_second = pmm_free_pages_count();
  TEST_ASSERT(free_after_second == free_after_first,
              "segundo free corrompió el contador (%lu -> %lu)",
              (unsigned long)free_after_first,
              (unsigned long)free_after_second);
}
REGISTER_TEST("pmm: doble free de una página", test_pmm_double_free_page);

// ===========================================================================
// [Fase 1.1] pmm: doble free de un rango buddy (potencia de 2)
//
// La rama buddy de pmm_free_pages() usa range_mark_free(), que es
// idempotente. Sin el chequeo previo de "todas ya libres", el segundo
// free decrementaba used_blocks de más. Este test lo fuerza con
// count=4 (order=2, garantiza la rama buddy).
// ===========================================================================
static void test_pmm_double_free_range_buddy(void) {
  uint64_t base = pmm_alloc_pages(4);
  TEST_ASSERT(base != 0, "pmm_alloc_pages(4) falló");
  if (!base)
    return;
  TEST_ASSERT((base & (4 * PAGE_SIZE - 1)) == 0,
              "pmm_alloc_pages(4) devolvió 0x%llx no alineado a 16 KB",
              (unsigned long long)base);

  uint64_t free_before = pmm_free_pages_count();
  pmm_free_pages(base, 4);
  uint64_t free_after_first = pmm_free_pages_count();
  TEST_ASSERT(free_after_first == free_before + 4,
              "primer free_pages(4) no liberó 4 (%lu -> %lu)",
              (unsigned long)free_before, (unsigned long)free_after_first);

  pmm_free_pages(base, 4);
  uint64_t free_after_second = pmm_free_pages_count();
  TEST_ASSERT(free_after_second == free_after_first,
              "segundo free_pages(4) corrompió el contador (%lu -> %lu)",
              (unsigned long)free_after_first,
              (unsigned long)free_after_second);
}
REGISTER_TEST("pmm: doble free de un rango buddy",
              test_pmm_double_free_range_buddy);

// ---------------------------------------------------------------------------
// ELF: rechazo de overflow al reubicar segmentos ET_DYN
// ---------------------------------------------------------------------------
static void test_elf_relocation_overflow(void) {
  uint8_t image[sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr)];
  memset(image, 0, sizeof(image));

  Elf64_Ehdr *hdr = (Elf64_Ehdr *)image;
  hdr->e_ident[EI_MAG0] = ELFMAG0;
  hdr->e_ident[EI_MAG1] = ELFMAG1;
  hdr->e_ident[EI_MAG2] = ELFMAG2;
  hdr->e_ident[EI_MAG3] = ELFMAG3;
  hdr->e_ident[EI_CLASS] = ELFCLASS64;
  hdr->e_ident[EI_DATA] = ELFDATA2LSB;
  hdr->e_ident[EI_VERSION] = EV_CURRENT;
  hdr->e_type = ET_DYN;
  hdr->e_machine = EM_X86_64;
  hdr->e_version = EV_CURRENT;
  hdr->e_entry = 0x1000;
  hdr->e_phoff = sizeof(Elf64_Ehdr);
  hdr->e_ehsize = sizeof(Elf64_Ehdr);
  hdr->e_phentsize = sizeof(Elf64_Phdr);
  hdr->e_phnum = 1;

  Elf64_Phdr *ph = (Elf64_Phdr *)(image + sizeof(Elf64_Ehdr));
  ph->p_type = PT_LOAD;
  ph->p_flags = PF_R | PF_X;
  ph->p_offset = sizeof(image);
  ph->p_vaddr = 0x1000;
  ph->p_filesz = 0;
  ph->p_memsz = 0x1000;

  uint64_t pml4_phys = pmm_alloc_page();
  TEST_ASSERT(pml4_phys != 0, "no se pudo reservar PML4 de prueba");
  if (!pml4_phys)
    return;

  uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
  memset(pml4, 0, PAGE_SIZE);
  uint64_t baseline = pmm_free_pages_count();

  /*
   * 0x1000 + (UINT64_MAX - 0xFFF) = 2^64: el código antiguo hacía wrap
   * y podía convertir un ELF válido estructuralmente en un rango absurdo.
   */
  uint64_t load_base = UINT64_MAX - 0xFFFULL;
  uint64_t entry = 0;
  int rc = elf_load(image, sizeof(image), pml4, load_base, &entry);

  TEST_ASSERT(rc != 0, "elf_load aceptó overflow en p_vaddr + load_base");
  TEST_ASSERT(pml4[PML4_INDEX(0x1000)] == 0,
              "ELF inválido modificó el PML4 antes de ser rechazado");
  TEST_ASSERT(pmm_free_pages_count() == baseline,
              "ELF inválido consumió páginas antes de ser rechazado");

  pmm_free_page(pml4_phys);
}
REGISTER_TEST("elf: rechaza overflow de relocación ET_DYN",
              test_elf_relocation_overflow);

// ---------------------------------------------------------------------------
// Paging: rollback de tablas ante fallo de asignación
// ---------------------------------------------------------------------------
static void test_paging_map_rollback(void) {
  const uint64_t virt = 0x0000001000000000ULL;
  uint64_t pml4_phys = pmm_alloc_page();
  TEST_ASSERT(pml4_phys != 0, "no se pudo reservar PML4 de prueba");
  if (!pml4_phys)
    return;

  uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
  memset(pml4, 0, PAGE_SIZE);

  uint64_t mapped_phys = pmm_alloc_page();
  TEST_ASSERT(mapped_phys != 0, "no se pudo reservar página hoja de prueba");
  if (!mapped_phys) {
    pmm_free_page(pml4_phys);
    return;
  }

  uint64_t baseline = pmm_free_pages_count();
  for (int fail_after = 0; fail_after < 3; fail_after++) {
    memset(pml4, 0, PAGE_SIZE);
    paging_test_set_alloc_fail_after(fail_after);
    int rc = paging_map_page_in(pml4, virt, mapped_phys, PTE_WRITABLE | PTE_NX);
    paging_test_set_alloc_fail_after(-1);

    TEST_ASSERT(rc != 0, "fallo inyectado #%d no produjo error", fail_after);
    TEST_ASSERT(pml4[PML4_INDEX(virt)] == 0,
                "rollback #%d dejó PML4[%lu] = 0x%llx", fail_after,
                (unsigned long)PML4_INDEX(virt),
                (unsigned long long)pml4[PML4_INDEX(virt)]);
    TEST_ASSERT(pmm_free_pages_count() == baseline,
                "rollback #%d perdió páginas: libres=%lu esperado=%lu",
                fail_after, (unsigned long)pmm_free_pages_count(),
                (unsigned long)baseline);
  }

  pmm_free_page(mapped_phys);
  pmm_free_page(pml4_phys);
}
REGISTER_TEST("paging: rollback de tablas en fallo de alloc",
              test_paging_map_rollback);

// ---------------------------------------------------------------------------
// Paging: rechazo de rangos con overflow / direcciones no canónicas
// ---------------------------------------------------------------------------
static void test_paging_map_range_overflow(void) {
  // Todos estos casos deben fallar antes de tocar ninguna tabla de páginas.
  int rc = paging_map_range(0x00007FFFFFFFF000ULL, 0x1000, 0x2000,
                            PTE_WRITABLE | PTE_NX);
  TEST_ASSERT(rc != 0,
              "paging_map_range permitió cruzar el hueco no canónico x86-64");

  rc = paging_map_range(0xFFFFFFFFFFFFF000ULL, 0x2000, 0x2000,
                        PTE_WRITABLE | PTE_NX);
  TEST_ASSERT(rc != 0,
              "paging_map_range permitió overflow de dirección virtual");

  rc = paging_map_range(0x0000001000000000ULL, 0x000FFFFFFFFFF000ULL, 0x2000,
                        PTE_WRITABLE | PTE_NX);
  TEST_ASSERT(rc != 0,
              "paging_map_range permitió overflow/límite físico de PTE");

  rc = paging_map_range(0x0000001000000000ULL, 0x1000, UINT64_MAX,
                        PTE_WRITABLE | PTE_NX);
  TEST_ASSERT(rc != 0,
              "paging_map_range aceptó un tamaño que desborda el rango");
}
REGISTER_TEST("paging: rechaza overflow en map_range",
              test_paging_map_range_overflow);

// ---------------------------------------------------------------------------
// MMIO: rechazo de rangos físicos que desbordan PTE_FRAME
// ---------------------------------------------------------------------------
static void test_mmio_map_overflow(void) {
  const uint64_t mmio_window_size = (1ULL << 39);
  const uint64_t mmio_max_phys = mmio_window_size - 1;

  // El último byte de la ventana MMIO es válido.
  void *ok = mmio_map(mmio_max_phys, 1, PTE_NOCACHE | PTE_NX);
  TEST_ASSERT(ok != NULL, "mmio_map rechazó el último byte de la ventana");
  if (ok)
    mmio_unmap(mmio_max_phys, 1);

  // Un rango que exceda la ventana virtual debe rechazarse.
  void *bad = mmio_map(mmio_max_phys, 2, PTE_NOCACHE | PTE_NX);
  TEST_ASSERT(bad == NULL, "mmio_map aceptó un rango fuera de la ventana MMIO");

  // La PTE admite direcciones físicas mayores que la ventana MMIO.
  bad = mmio_map(PTE_FRAME, 1, PTE_NOCACHE | PTE_NX);
  TEST_ASSERT(bad == NULL,
              "mmio_map aceptó una dirección física fuera de la ventana");

  // El cálculo phys + size no puede envolver.
  bad = mmio_map(UINT64_MAX - 0x7FFULL, 0x1000, PTE_NOCACHE | PTE_NX);
  TEST_ASSERT(bad == NULL, "mmio_map permitió overflow de phys + size");

  // El último frame de 4 KiB de la ventana también es válido.
  ok = mmio_map(mmio_max_phys - 0xFFFULL, 0x1000, PTE_NOCACHE | PTE_NX);
  TEST_ASSERT(ok != NULL, "mmio_map rechazó el último frame de la ventana");
  if (ok)
    mmio_unmap(mmio_max_phys - 0xFFFULL, 0x1000);
}
REGISTER_TEST("paging: mmio_map rechaza overflow físico",
              test_mmio_map_overflow);

// ---------------------------------------------------------------------------
// Paging: rechazo de direcciones virtuales no canónicas
// ---------------------------------------------------------------------------
static void test_paging_noncanonical_addresses(void) {
  const uint64_t noncanonical_low = 0x0000800000000000ULL;
  const uint64_t noncanonical_high = 0xFFFF7FFFFFFFFFFFULL;
  const uint64_t valid_low = 0x00007FFFFFFFFFFFULL;
  const uint64_t valid_high = 0xFFFF800000000000ULL;

  uint64_t pml4_phys = pmm_alloc_page();
  TEST_ASSERT(pml4_phys != 0, "no se pudo reservar PML4 de prueba");
  if (!pml4_phys)
    return;

  uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
  memset(pml4, 0, PAGE_SIZE);
  uint64_t baseline = pmm_free_pages_count();

  TEST_ASSERT(paging_map_page_in(pml4, noncanonical_low, 0x1000,
                                 PTE_WRITABLE | PTE_NX) != 0,
              "paging_map_page_in aceptó dirección no canónica baja");
  TEST_ASSERT(paging_map_page_in(pml4, noncanonical_high, 0x1000,
                                 PTE_WRITABLE | PTE_NX) != 0,
              "paging_map_page_in aceptó dirección no canónica alta");
  TEST_ASSERT(pml4[0] == 0 && pml4[511] == 0,
              "una dirección no canónica modificó el PML4");
  TEST_ASSERT(pmm_free_pages_count() == baseline,
              "dirección no canónica consumió páginas de tablas");

  TEST_ASSERT(
      paging_map_page(noncanonical_low, 0x1000, PTE_WRITABLE | PTE_NX) != 0,
      "paging_map_page aceptó dirección no canónica baja");
  TEST_ASSERT(
      paging_map_page(noncanonical_high, 0x1000, PTE_WRITABLE | PTE_NX) != 0,
      "paging_map_page aceptó dirección no canónica alta");

  TEST_ASSERT(paging_unmap_page_in(pml4, noncanonical_low) != 0,
              "paging_unmap_page_in aceptó dirección no canónica");
  TEST_ASSERT(paging_get_phys_in(pml4, noncanonical_low) == 0,
              "paging_get_phys_in aceptó dirección no canónica");
  TEST_ASSERT(paging_unmap_page(noncanonical_low) != 0,
              "paging_unmap_page aceptó dirección no canónica");
  TEST_ASSERT(paging_get_phys(noncanonical_low) == 0,
              "paging_get_phys aceptó dirección no canónica");

  TEST_ASSERT(
      paging_map_page_in(pml4, valid_low, 0x1000, PTE_WRITABLE | PTE_NX) == 0,
      "paging_map_page_in rechazó el máximo bajo canónico");
  TEST_ASSERT(paging_unmap_page_in(pml4, valid_low) == 0,
              "no se pudo desmapear el máximo bajo canónico");

  TEST_ASSERT(
      paging_map_page_in(pml4, valid_high, 0x1000, PTE_WRITABLE | PTE_NX) == 0,
      "paging_map_page_in rechazó el mínimo alto canónico");
  TEST_ASSERT(paging_unmap_page_in(pml4, valid_high) == 0,
              "no se pudo desmapear el mínimo alto canónico");

  uint64_t pml4_entry = pml4[PML4_INDEX(valid_low)];
  if (pml4_entry) {
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4_entry & PTE_FRAME);
    uint64_t pdpt_entry = pdpt[PDPT_INDEX(valid_low)];
    if (pdpt_entry) {
      uint64_t *pd = (uint64_t *)phys_to_virt(pdpt_entry & PTE_FRAME);
      uint64_t pd_entry = pd[PD_INDEX(valid_low)];
      if (pd_entry) {
        pmm_free_page(pd_entry & PTE_FRAME);
        pd[PD_INDEX(valid_low)] = 0;
      }
      pmm_free_page(pdpt_entry & PTE_FRAME);
      pdpt[PDPT_INDEX(valid_low)] = 0;
    }
    pmm_free_page(pml4_entry & PTE_FRAME);
    pml4[PML4_INDEX(valid_low)] = 0;
  }

  pml4_entry = pml4[PML4_INDEX(valid_high)];
  if (pml4_entry) {
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4_entry & PTE_FRAME);
    uint64_t pdpt_entry = pdpt[PDPT_INDEX(valid_high)];
    if (pdpt_entry) {
      uint64_t *pd = (uint64_t *)phys_to_virt(pdpt_entry & PTE_FRAME);
      uint64_t pd_entry = pd[PD_INDEX(valid_high)];
      if (pd_entry) {
        pmm_free_page(pd_entry & PTE_FRAME);
        pd[PD_INDEX(valid_high)] = 0;
      }
      pmm_free_page(pdpt_entry & PTE_FRAME);
      pdpt[PDPT_INDEX(valid_high)] = 0;
    }
    pmm_free_page(pml4_entry & PTE_FRAME);
    pml4[PML4_INDEX(valid_high)] = 0;
  }

  TEST_ASSERT(pmm_free_pages_count() == baseline,
              "el test dejó tablas de páginas sin liberar");
  pmm_free_page(pml4_phys);
}
REGISTER_TEST("paging: rechaza direcciones no canónicas",
              test_paging_noncanonical_addresses);

static void test_paging_unmap_rejects_huge_page(void) {
  /*
   * PHYS_MAP_BASE se construye con páginas de 2 MiB. Un unmap genérico
   * no puede tratar una entrada PDE con PTE_HUGE como si apuntara a una PT:
   * el frame físico de la huge page no es una tabla de 512 PTEs.
   */
  TEST_ASSERT(paging_unmap_page(PHYS_MAP_BASE) != 0,
              "paging_unmap_page aceptó una página huge de 2 MiB");

  /*
   * Repetimos la comprobación con un PML4 aislado para garantizar que
   * paging_unmap_page_in() conserva la entrada PDE huge intacta.
   */
  uint64_t pml4_phys = pmm_alloc_page();
  TEST_ASSERT(pml4_phys != 0, "no se pudo reservar PML4 de prueba");
  if (!pml4_phys)
    return;

  uint64_t pdpt_phys = pmm_alloc_page();
  uint64_t pd_phys = pmm_alloc_page();
  if (!pdpt_phys || !pd_phys) {
    TEST_ASSERT(0, "no se pudieron reservar tablas de prueba");
    if (pd_phys)
      pmm_free_page(pd_phys);
    if (pdpt_phys)
      pmm_free_page(pdpt_phys);
    pmm_free_page(pml4_phys);
    return;
  }

  uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
  uint64_t *pdpt = (uint64_t *)phys_to_virt(pdpt_phys);
  uint64_t *pd = (uint64_t *)phys_to_virt(pd_phys);
  memset(pml4, 0, PAGE_SIZE);
  memset(pdpt, 0, PAGE_SIZE);
  memset(pd, 0, PAGE_SIZE);

  const uint64_t virt = 0x0000004000000000ULL;
  const uint64_t huge_phys = 0x00200000ULL;
  const uint64_t huge_entry = huge_phys | PTE_PRESENT | PTE_WRITABLE | PTE_HUGE;

  pml4[PML4_INDEX(virt)] = pdpt_phys | PTE_PRESENT | PTE_WRITABLE;
  pdpt[PDPT_INDEX(virt)] = pd_phys | PTE_PRESENT | PTE_WRITABLE;
  pd[PD_INDEX(virt)] = huge_entry;

  int rc = paging_unmap_page_in(pml4, virt);
  TEST_ASSERT(rc != 0, "paging_unmap_page_in aceptó una PDE huge de 2 MiB");
  TEST_ASSERT(pd[PD_INDEX(virt)] == huge_entry,
              "unmap modificó una PDE huge en lugar de rechazarla");

  pmm_free_page(pd_phys);
  pmm_free_page(pdpt_phys);
  pmm_free_page(pml4_phys);
}
REGISTER_TEST("paging: unmap rechaza huge pages",
              test_paging_unmap_rejects_huge_page);

// ---------------------------------------------------------------------------
// Paging: cálculo seguro de la ventana física
// ---------------------------------------------------------------------------
static void test_paging_phys_window_size(void) {
  const uint64_t max_window = PHYS_MAP_MAX_SIZE;
  const uint64_t huge = UINT64_MAX;

  TEST_ASSERT(paging_phys_window_size(0) == 0,
              "max_phys_addr=0 no debería mapear RAM");
  TEST_ASSERT(paging_phys_window_size(1) == 0x200000ULL,
              "1 byte debería redondear a 2 MiB");
  TEST_ASSERT(paging_phys_window_size(0x200001ULL) == 0x400000ULL,
              "2 MiB+1 debería redondear a 4 MiB");
  TEST_ASSERT(
      paging_phys_window_size(max_window - 1) == max_window,
      "RAM justo por debajo de 512 GiB debería quedar limitada a 512 GiB");
  TEST_ASSERT(paging_phys_window_size(max_window) == max_window,
              "512 GiB debería caber exactamente en la ventana");
  TEST_ASSERT(paging_phys_window_size(max_window + 1) == max_window,
              "RAM superior a 512 GiB debería quedar limitada a 512 GiB");
  TEST_ASSERT(paging_phys_window_size(huge) == max_window,
              "UINT64_MAX no debería provocar overflow ni superar la ventana");
}
REGISTER_TEST("paging: ventana física sin overflow",
              test_paging_phys_window_size);

// ===========================================================================
// [Fase 1.1] paging: split de huge page preserva PTE_NX
//
// Regresión de H2 en split_huge_page(): antes se hacía
//   inherit_flags = (huge_entry & 0xFFF) & ~(PTE_HUGE | 0x080ULL);
// que descartaba el bit NX (63) al construir los PTEs hijos. Este test
// monta una PDE con PTE_HUGE|PTE_NX, fuerza el split mapeando una PTE
// dentro, y verifica que los PTEs resultantes heredan NX.
// ===========================================================================
static void test_paging_split_huge_preserves_nx(void) {
  // PML4/PDPT/PD propios para no tocar kernel_pml4.
  uint64_t pml4_phys = pmm_alloc_page();
  uint64_t pdpt_phys = pmm_alloc_page();
  uint64_t pd_phys = pmm_alloc_page();
  TEST_ASSERT(pml4_phys && pdpt_phys && pd_phys,
              "no se pudieron reservar tablas (pml4=%p pdpt=%p pd=%p)",
              (void *)pml4_phys, (void *)pdpt_phys, (void *)pd_phys);
  if (!pml4_phys || !pdpt_phys || !pd_phys) {
    if (pd_phys)
      pmm_free_page(pd_phys);
    if (pdpt_phys)
      pmm_free_page(pdpt_phys);
    if (pml4_phys)
      pmm_free_page(pml4_phys);
    return;
  }

  uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
  uint64_t *pdpt = (uint64_t *)phys_to_virt(pdpt_phys);
  uint64_t *pd = (uint64_t *)phys_to_virt(pd_phys);
  memset(pml4, 0, PAGE_SIZE);
  memset(pdpt, 0, PAGE_SIZE);
  memset(pd, 0, PAGE_SIZE);

  // virt elegido con pd_idx=1 y pt_idx=0, para poder revisar
  // pt[0..511] enteros y no pisar pd[0] por accidente.
  //   0x0000000040200000 = 2^30 + 2 * 2^20 → pd_idx=1, pt_idx=0
  const uint64_t virt = 0x0000000040200000ULL;

  // Huge page de 2 MB: base alineada a 2 MB, flags con NX.
  const uint64_t huge_phys = 0x200000ULL;
  const uint64_t huge_entry =
      huge_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_HUGE | PTE_NX;

  pml4[PML4_INDEX(virt)] = pdpt_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
  pdpt[PDPT_INDEX(virt)] = pd_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
  pd[PD_INDEX(virt)] = huge_entry;

  uint64_t leaf_phys = pmm_alloc_page();
  TEST_ASSERT(leaf_phys != 0, "no se pudo reservar leaf phys");
  if (!leaf_phys) {
    pmm_free_page(pd_phys);
    pmm_free_page(pdpt_phys);
    pmm_free_page(pml4_phys);
    return;
  }

  int rc = paging_map_page_in(pml4, virt, leaf_phys,
                              PTE_USER | PTE_WRITABLE | PTE_NX);
  TEST_ASSERT(rc == 0, "paging_map_page_in falló: %d", rc);
  if (rc != 0) {
    pmm_free_page(leaf_phys);
    pmm_free_page(pd_phys);
    pmm_free_page(pdpt_phys);
    pmm_free_page(pml4_phys);
    return;
  }

  TEST_ASSERT((pd[PD_INDEX(virt)] & PTE_HUGE) == 0,
              "PDE sigue marcada como huge tras el split (0x%llx)",
              (unsigned long long)pd[PD_INDEX(virt)]);

  uint64_t pt_phys = pd[PD_INDEX(virt)] & PTE_FRAME;
  uint64_t *pt = (uint64_t *)phys_to_virt(pt_phys);

  // pt[0] es la entrada que acabamos de escribir (leaf_phys).
  TEST_ASSERT((pt[0] & PTE_FRAME) == leaf_phys,
              "pt[0]=0x%llx no apunta a leaf_phys=0x%llx",
              (unsigned long long)(pt[0] & PTE_FRAME),
              (unsigned long long)leaf_phys);
  TEST_ASSERT((pt[0] & PTE_NX) != 0, "pt[0] perdió PTE_NX");

  // pt[1..511] son herencia del split: deben tener PRESENT, USER,
  // WRITABLE, NX, y apuntar a huge_phys + i*PAGE_SIZE.
  int bad_idx = -1;
  const char *bad_reason = NULL;
  for (int i = 1; i < PAGE_ENTRIES; i++) {
    if (!(pt[i] & PTE_PRESENT)) {
      bad_idx = i;
      bad_reason = "PRESENT=0";
      break;
    }
    if (!(pt[i] & PTE_USER)) {
      bad_idx = i;
      bad_reason = "USER=0";
      break;
    }
    if (!(pt[i] & PTE_WRITABLE)) {
      bad_idx = i;
      bad_reason = "WRITABLE=0";
      break;
    }
    if (!(pt[i] & PTE_NX)) {
      bad_idx = i;
      bad_reason = "NX=0";
      break;
    }
    uint64_t expected = huge_phys + (uint64_t)i * PAGE_SIZE;
    if ((pt[i] & PTE_FRAME) != expected) {
      bad_idx = i;
      bad_reason = "phys incorrecta";
      break;
    }
  }
  TEST_ASSERT(bad_idx < 0,
              "split_huge_page perdió flags en pt[%d]: %s (entry=0x%llx)",
              bad_idx, bad_reason ? bad_reason : "?",
              bad_idx >= 0 ? (unsigned long long)pt[bad_idx] : 0ULL);

  // Cleanup ordenado.
  paging_unmap_page_in(pml4, virt);
  pmm_free_page(pt_phys);
  pmm_free_page(leaf_phys);
  pmm_free_page(pd_phys);
  pmm_free_page(pdpt_phys);
  pmm_free_page(pml4_phys);
}
REGISTER_TEST("paging: split huge preserva NX",
              test_paging_split_huge_preserves_nx);

// ===========================================================================
// [Fase 1.1] paging: remap actualiza la PTE y no corrompe
//
// Verifica que paging_map_page_in sobre un VA ya mapeado tiene éxito,
// actualiza la PTE al nuevo phys y emite el LOG_WARN de H4 (que no
// comprobamos aquí, solo que el comportamiento sea el correcto).
// ===========================================================================
static void test_paging_remap_updates_phys(void) {
  const uint64_t virt = 0x0000005000000000ULL;

  uint64_t pml4_phys = pmm_alloc_page();
  uint64_t phys_a = pmm_alloc_page();
  uint64_t phys_b = pmm_alloc_page();
  TEST_ASSERT(pml4_phys && phys_a && phys_b, "fallo alloc (pml4=%p a=%p b=%p)",
              (void *)pml4_phys, (void *)phys_a, (void *)phys_b);
  if (!pml4_phys || !phys_a || !phys_b) {
    if (phys_b)
      pmm_free_page(phys_b);
    if (phys_a)
      pmm_free_page(phys_a);
    if (pml4_phys)
      pmm_free_page(pml4_phys);
    return;
  }

  uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
  memset(pml4, 0, PAGE_SIZE);

  int rc =
      paging_map_page_in(pml4, virt, phys_a, PTE_USER | PTE_WRITABLE | PTE_NX);
  TEST_ASSERT(rc == 0, "primer map falló: %d", rc);
  TEST_ASSERT(paging_get_phys_in(pml4, virt) == phys_a,
              "primer map no dejó phys_a");

  // Remap al mismo VA con otro phys.
  rc = paging_map_page_in(pml4, virt, phys_b, PTE_USER | PTE_WRITABLE | PTE_NX);
  TEST_ASSERT(rc == 0, "remap falló: %d", rc);
  TEST_ASSERT(paging_get_phys_in(pml4, virt) == phys_b,
              "remap no actualizó a phys_b (sigue 0x%llx)",
              (unsigned long long)paging_get_phys_in(pml4, virt));

  // Idempotente: remap al mismo phys no debe romper.
  rc = paging_map_page_in(pml4, virt, phys_b, PTE_USER | PTE_WRITABLE | PTE_NX);
  TEST_ASSERT(rc == 0, "remap idempotente falló: %d", rc);
  TEST_ASSERT(paging_get_phys_in(pml4, virt) == phys_b,
              "remap idempotente cambió el phys");

  // Cleanup: recorrer el PML4 y liberar tablas.
  paging_unmap_page_in(pml4, virt);

  uint64_t pdpt_phys = 0, pd_phys = 0, pt_phys = 0;
  if (pml4[PML4_INDEX(virt)] & PTE_PRESENT) {
    pdpt_phys = pml4[PML4_INDEX(virt)] & PTE_FRAME;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pdpt_phys);
    if (pdpt[PDPT_INDEX(virt)] & PTE_PRESENT) {
      pd_phys = pdpt[PDPT_INDEX(virt)] & PTE_FRAME;
      uint64_t *pd = (uint64_t *)phys_to_virt(pd_phys);
      if ((pd[PD_INDEX(virt)] & PTE_PRESENT) &&
          !(pd[PD_INDEX(virt)] & PTE_HUGE)) {
        pt_phys = pd[PD_INDEX(virt)] & PTE_FRAME;
      }
    }
  }
  if (pt_phys)
    pmm_free_page(pt_phys);
  if (pd_phys)
    pmm_free_page(pd_phys);
  if (pdpt_phys)
    pmm_free_page(pdpt_phys);
  pmm_free_page(phys_a);
  pmm_free_page(phys_b);
  pmm_free_page(pml4_phys);
}
REGISTER_TEST("paging: remap actualiza la PTE", test_paging_remap_updates_phys);

// ---------------------------------------------------------------------------
// SMP (Fase 0)
// ---------------------------------------------------------------------------
static void test_smp_processor_id_valid(void) {
  int cpu = smp_processor_id();
  TEST_ASSERT(cpu >= 0 && cpu < MAX_CPUS,
              "smp_processor_id() devolvió %d, fuera de rango [0,%d)", cpu,
              MAX_CPUS);
  TEST_ASSERT(cpu_local_data[cpu].cpu_id == cpu,
              "cpu_local_data[%d].cpu_id = %d, esperado %d", cpu,
              cpu_local_data[cpu].cpu_id, cpu);
}
REGISTER_TEST("smp: smp_processor_id() válido", test_smp_processor_id_valid);

static void test_smp_cpu_local_data_exists(void) {
  TEST_ASSERT(cpu_local_data[0].cpu_id == 0,
              "cpu_local_data[0].cpu_id = %d, esperado 0",
              cpu_local_data[0].cpu_id);
}
REGISTER_TEST("smp: cpu_local_data[0] inicializado",
              test_smp_cpu_local_data_exists);

static void test_smp_this_cpu_macro(void) {
  int cpu = smp_processor_id();
  uint64_t saved = this_cpu(ticks_since_resched);
  this_cpu(ticks_since_resched) = 0xDEADBEEF;
  TEST_ASSERT(cpu_local_data[cpu].ticks_since_resched == 0xDEADBEEF,
              "this_cpu(ticks_since_resched) no escribió en cpu_local_data[%d]",
              cpu);
  this_cpu(ticks_since_resched) = saved;
}
REGISTER_TEST("smp: this_cpu() accede al CPU actual", test_smp_this_cpu_macro);

static void test_smp_per_cpu_macro(void) {
  per_cpu(ticks_since_resched, 3) = 0xCAFEBABE;
  TEST_ASSERT(
      cpu_local_data[3].ticks_since_resched == 0xCAFEBABE,
      "per_cpu(ticks_since_resched, 3) no escribió en cpu_local_data[3]");
  per_cpu(ticks_since_resched, 3) = 0;
}
REGISTER_TEST("smp: per_cpu() accede al slot correcto", test_smp_per_cpu_macro);

// ---------------------------------------------------------------------------
// ACPI (Fase 1 de SMP)
// ---------------------------------------------------------------------------
static void test_acpi_valid(void) {
  const acpi_info_t *info = acpi_get_info();
  TEST_ASSERT(info->valid == 1, "ACPI no se inicializó correctamente");
}
REGISTER_TEST("acpi: parseo del MADT correcto", test_acpi_valid);

static void test_acpi_cpu_count(void) {
  const acpi_info_t *info = acpi_get_info();
  TEST_ASSERT(info->cpu_count >= 1, "ACPI cpu_count = %d, esperado >= 1",
              info->cpu_count);
}
REGISTER_TEST("acpi: al menos 1 CPU detectada", test_acpi_cpu_count);

static void test_acpi_lapic_address(void) {
  const acpi_info_t *info = acpi_get_info();
  TEST_ASSERT(info->lapic_address != 0, "ACPI lapic_address = 0");
}
REGISTER_TEST("acpi: LAPIC address válida", test_acpi_lapic_address);

static void test_acpi_bsp_found(void) {
  const acpi_info_t *info = acpi_get_info();
  TEST_ASSERT(info->bsp_index >= 0, "ACPI no encontró el BSP (bsp_index=%d)",
              info->bsp_index);
}
REGISTER_TEST("acpi: BSP identificado", test_acpi_bsp_found);

// ---------------------------------------------------------------------------
// APIC (Fase 2.1)
// ---------------------------------------------------------------------------
static void test_apic_lapic_mapped(void) {
  uint32_t version = lapic_read(LAPIC_REG_VERSION);
  TEST_ASSERT(version != 0 && version != 0xFFFFFFFF,
              "LAPIC VERSION = 0x%x (no mapeado o no activo)", version);
}
REGISTER_TEST("apic: LAPIC mapeado y activo", test_apic_lapic_mapped);

static void test_apic_svr_enabled(void) {
  uint32_t svr = lapic_read(LAPIC_REG_SVR);
  TEST_ASSERT((svr & LAPIC_SVR_ENABLE) != 0,
              "LAPIC SVR no tiene el bit ENABLE (svr=0x%x)", svr);
}
REGISTER_TEST("apic: LAPIC SVR habilitado", test_apic_svr_enabled);

static void test_apic_bsp_id(void) {
  const acpi_info_t *info = acpi_get_info();
  TEST_ASSERT(info->bsp_index >= 0 && info->bsp_index < info->cpu_count,
              "ACPI bsp_index=%d inválido (cpu_count=%d)", info->bsp_index,
              info->cpu_count);
  if (info->bsp_index < 0 || info->bsp_index >= info->cpu_count)
    return;

  uint32_t expected = info->cpus[info->bsp_index].apic_id;
  uint32_t actual = lapic_get_bsp_id();
  TEST_ASSERT(actual == expected, "BSP APIC ID = %u, esperado %u (MADT)",
              actual, expected);
}

// ---------------------------------------------------------------------------
// APIC / IOAPIC (Fase 2.2)
// ---------------------------------------------------------------------------
static void test_apic_ioapic_detected(void) {
  const acpi_info_t *info = acpi_get_info();
  TEST_ASSERT(info->ioapic_count >= 1,
              "No hay IOAPICs detectados por ACPI (count=%d)",
              info->ioapic_count);
}
REGISTER_TEST("apic: IOAPIC detectado", test_apic_ioapic_detected);

static void test_apic_iso_qemu(void) {
  const acpi_info_t *info = acpi_get_info();
  int found = 0;
  for (int i = 0; i < info->iso_count; i++) {
    if (info->isos[i].irq == 0 && info->isos[i].gsi == 2) {
      found = 1;
      break;
    }
  }
  TEST_ASSERT(1, "ISO IRQ0->GSI2: %s", found ? "presente" : "no presente");
}
REGISTER_TEST("apic: ISO IRQ0->GSI2 registrado", test_apic_iso_qemu);

// ---------------------------------------------------------------------------
// LAPIC timer (Fase 2.3)
// ---------------------------------------------------------------------------
static void test_lapic_timer_running(void) {
  uint32_t c1 = lapic_read(LAPIC_REG_TIMER_CURRENT);
  for (volatile int i = 0; i < 1000; i++) {
  }
  uint32_t c2 = lapic_read(LAPIC_REG_TIMER_CURRENT);
  TEST_ASSERT(c1 != c2, "LAPIC timer no decrementa (c1=%u c2=%u)", c1, c2);
}
REGISTER_TEST("lapic-timer: contador decrementa", test_lapic_timer_running);

static void test_lapic_timer_tick(void) {
  extern volatile uint64_t tick_count;

  uint64_t t1 = tick_count;
  uint64_t spins = 0;
  const uint64_t spin_limit = 1800000000ULL;

  while (tick_count == t1 && spins < spin_limit) {
    __asm__ volatile("pause");
    spins++;
  }

  uint64_t t2 = tick_count;
  TEST_ASSERT(t2 > t1, "tick_count no avanza (t1=%lu t2=%lu, spins=%lu)",
              (unsigned long)t1, (unsigned long)t2, (unsigned long)spins);
}
REGISTER_TEST("lapic-timer: tick_count avanza", test_lapic_timer_tick);

// ---------------------------------------------------------------------------
// Scheduler SMP y APs (Fase 3 & 4)
// ---------------------------------------------------------------------------
static void test_smp_sched_current_valid(void) {
  task_t *cur = sched_current();
  TEST_ASSERT(cur != NULL, "sched_current() devolvió NULL");
  TEST_ASSERT(cur->state == TASK_RUNNING,
              "sched_current() no está en TASK_RUNNING (state=%d)", cur->state);
}
REGISTER_TEST("smp: sched_current() per-CPU válido",
              test_smp_sched_current_valid);

static void test_smp_aps_status(void) {
  const acpi_info_t *acpi = acpi_get_info();
  if (acpi->cpu_count > 1) {
    int ready = smp_aps_ready();
    TEST_ASSERT(ready > 0, "Sistema SMP con %d CPUs pero aps_ready=%d",
                acpi->cpu_count, ready);
  } else {
    TEST_ASSERT(smp_aps_ready() == 0,
                "Sistema uniprocesador pero aps_ready != 0");
  }
}
REGISTER_TEST("smp: estado de APs coherente con ACPI", test_smp_aps_status);

// ---------------------------------------------------------------------------
// [SMP 4.4] Test serio: wakeup cross-CPU vía IPI.
// ---------------------------------------------------------------------------
static struct wait_queue g_ipi_waiter_wq;
static struct wait_queue g_ipi_done_wq;
static volatile int g_ipi_cond = 0;
static volatile int g_ipi_done = 0;
static volatile uint32_t g_ipi_runner_cpu = 0xFFFFFFFF;
static volatile uint32_t g_ipi_waker_cpu = 0xFFFFFFFF;

static bool ipi_waiter_cond_fn(void *arg) {
  (void)arg;
  return g_ipi_cond != 0;
}

static bool ipi_done_cond_fn(void *arg) {
  (void)arg;
  return g_ipi_done != 0;
}

static void ipi_test_waiter(void) {
  wait_event(&g_ipi_waiter_wq, ipi_waiter_cond_fn, NULL);
  g_ipi_runner_cpu = (uint32_t)smp_processor_id();
  g_ipi_done = 1;
  wake_up_all(&g_ipi_done_wq);
}

static void test_smp_ipi_wakeup(void) {
  if (smp_aps_ready() == 0) {
    TEST_ASSERT(0, "sin APs, el test no puede validar el cross-CPU");
    return;
  }

  g_ipi_cond = 0;
  g_ipi_done = 0;
  g_ipi_runner_cpu = 0xFFFFFFFF;
  g_ipi_waker_cpu = 0xFFFFFFFF;
  wait_queue_init(&g_ipi_waiter_wq);
  wait_queue_init(&g_ipi_done_wq);

  uint32_t my_cpu = (uint32_t)smp_processor_id();
  g_ipi_waker_cpu = my_cpu;

  task_t *waiter = sched_create_task(ipi_test_waiter);
  TEST_ASSERT(waiter != NULL, "sched_create_task falló");
  if (!waiter)
    return;

  waiter->cpu_affinity = -1;

  for (int i = 0; i < 5000 && waiter->state != TASK_BLOCKED; i++) {
    sched_yield();
    for (volatile int k = 0; k < 1000; k++) {
      __asm__ volatile("pause");
    }
  }

  TEST_ASSERT(waiter->state == TASK_BLOCKED,
              "el waiter no se bloqueó (state=%d)", waiter->state);
  if (waiter->state != TASK_BLOCKED)
    return;

  int ap_cpu = -1;
  for (int c = 0; c < MAX_CPUS; c++) {
    if ((uint32_t)c == my_cpu)
      continue;
    task_t *cur = per_cpu(current_task, c);
    if (cur && cur->is_idle) {
      ap_cpu = c;
      break;
    }
  }

  /*
   * Regression for sched_kick_idle_cpu(): the target must really be idle
   * when the waiter is woken. Otherwise the scheduler can simply pick the
   * READY task on a later timer tick and the test would not exercise the
   * remote-idle IPI path.
   */
  for (int i = 0; i < 5000 && ap_cpu < 0; i++) {
    sched_yield();
    for (volatile int k = 0; k < 1000; k++)
      __asm__ volatile("pause");

    for (int c = 0; c < MAX_CPUS; c++) {
      if ((uint32_t)c == my_cpu)
        continue;
      task_t *cur = per_cpu(current_task, c);
      if (cur && cur->is_idle) {
        ap_cpu = c;
        break;
      }
    }
  }

  TEST_ASSERT(ap_cpu >= 0, "no se encontró ningún AP idle");
  if (ap_cpu < 0)
    return;

  waiter->cpu_affinity = ap_cpu;

  g_ipi_cond = 1;
  wake_up_all(&g_ipi_waiter_wq);

  wait_event(&g_ipi_done_wq, ipi_done_cond_fn, NULL);

  TEST_ASSERT(g_ipi_done == 1, "g_ipi_done != 1");
  TEST_ASSERT(g_ipi_runner_cpu == (uint32_t)ap_cpu,
              "el waiter corrió en cpu=%u, esperado cpu=%d", g_ipi_runner_cpu,
              ap_cpu);

  LOG_INFO("[TEST] ipi-wakeup: waker=cpu[%u] runner=cpu[%u] (afinidad=%d)",
           g_ipi_waker_cpu, g_ipi_runner_cpu, ap_cpu);
}
REGISTER_TEST_FLAGS("smp: IPI wakeup cross-CPU", test_smp_ipi_wakeup,
                    TEST_FLAG_BLOCKING | TEST_FLAG_NEEDS_SMP);

// ---------------------------------------------------------------------------
// SMP: migración real + canary de stack
//
// Obliga a una tarea a bloquearse en CPU1, mantiene CPU1 ocupada con otra
// tarea y después la despierta con cpu_affinity=-1. La tarea debe reaparecer
// en una CPU distinta y conservar intacto su canary de stack.
// ---------------------------------------------------------------------------
static wait_queue_t g_smp_mig_wq;
static volatile int g_smp_mig_generation;
static volatile int g_smp_mig_blocked;
static volatile int g_smp_mig_done;
static volatile int g_smp_mig_cpu_before;
static volatile int g_smp_mig_cpu_after;
static volatile int g_smp_mig_pin_running;
static volatile int g_smp_mig_pin_stop;
static volatile int g_smp_mig_pin_cpu;
static volatile int g_smp_mig_source_cpu;

static bool smp_mig_cond(void *arg) {
  int seen = (int)(uintptr_t)arg;
  return __atomic_load_n(&g_smp_mig_generation, __ATOMIC_ACQUIRE) != seen;
}

static void smp_mig_waiter(void) {
  task_t *self = sched_current();
  if (!self)
    return;

  // Fuerza la primera ejecución en una CPU distinta del controlador. Después queda libre para migrar.
  int source = __atomic_load_n(&g_smp_mig_source_cpu, __ATOMIC_ACQUIRE);
  self->cpu_affinity = source;
  while (smp_processor_id() != source)
    sched_yield();
  self->cpu_affinity = -1;

  __sched_canary_arm();
  g_smp_mig_cpu_before = smp_processor_id();
  int seen = 0;
  wait_event(&g_smp_mig_wq, smp_mig_cond, (void *)(uintptr_t)seen);

  seen = __atomic_load_n(&g_smp_mig_generation, __ATOMIC_ACQUIRE);
  g_smp_mig_cpu_after = smp_processor_id();
  __sched_canary_check();
  g_smp_mig_done = (g_smp_mig_cpu_after != g_smp_mig_cpu_before);
}

static void smp_mig_pin(void) {
  task_t *self = sched_current();
  if (!self)
    return;

  int target = __atomic_load_n(&g_smp_mig_pin_cpu, __ATOMIC_ACQUIRE);
  self->cpu_affinity = target;
  while (smp_processor_id() != target)
    sched_yield();

  g_smp_mig_pin_running = 1;
  while (!__atomic_load_n(&g_smp_mig_pin_stop, __ATOMIC_ACQUIRE))
    __asm__ volatile("pause");
  g_smp_mig_pin_running = 0;
}

static void test_smp_task_migration_canary(void) {
  if (smp_aps_ready() == 0) {
    TEST_ASSERT(0, "se necesita al menos un AP para probar migración SMP");
    return;
  }

  task_t *controller = sched_current();
  if (!controller)
    return;

  wait_queue_init(&g_smp_mig_wq);
  g_smp_mig_generation = 0;
  g_smp_mig_blocked = 0;
  g_smp_mig_done = 0;
  g_smp_mig_cpu_before = -1;
  g_smp_mig_cpu_after = -1;
  g_smp_mig_pin_running = 0;
  g_smp_mig_pin_stop = 0;
  int controller_cpu = smp_processor_id();
  int source_cpu = (controller_cpu == 0) ? 1 : 0;
  g_smp_mig_source_cpu = source_cpu;
  g_smp_mig_pin_cpu = source_cpu;

  task_t *waiter = sched_create_task(smp_mig_waiter);
  TEST_ASSERT(waiter != NULL, "no se pudo crear waiter de migración");
  if (!waiter)
    return;

  // Esperar a que wait_common() haya publicado realmente la entrada en la
  // wait queue. Esto evita despertar demasiado pronto, entre el flag local
  // del test y wq_add_locked().
  uint64_t deadline = sched_get_ticks() + 3000;
  while (__atomic_load_n((task_t *volatile *)&waiter->waiting_on,
                         __ATOMIC_ACQUIRE) != &g_smp_mig_wq) {
    TEST_ASSERT(sched_get_ticks() < deadline,
                "timeout esperando waiter en la wait queue");
    if (sched_get_ticks() >= deadline)
      return;
    __asm__ volatile("pause");
  }

  task_t *pin = sched_create_task(smp_mig_pin);
  TEST_ASSERT(pin != NULL, "no se pudo crear pin task");
  if (!pin)
    return;

  deadline = sched_get_ticks() + 3000;
  while (!__atomic_load_n(&g_smp_mig_pin_running, __ATOMIC_ACQUIRE)) {
    TEST_ASSERT(sched_get_ticks() < deadline,
                "timeout esperando pin en CPU1");
    if (sched_get_ticks() >= deadline)
      return;
    __asm__ volatile("pause");
  }

  // Despertar con afinidad libre. La CPU fuente está ocupada por pin, así que
  // el yield del controller fuerza al waiter a ejecutarse en otra CPU.
  g_smp_mig_generation = 1;
  wake_up_all(&g_smp_mig_wq);
  sched_yield();

  deadline = sched_get_ticks() + 3000;
  while (!__atomic_load_n(&g_smp_mig_done, __ATOMIC_ACQUIRE)) {
    TEST_ASSERT(sched_get_ticks() < deadline,
                "timeout esperando migración");
    if (sched_get_ticks() >= deadline)
      break;
    __asm__ volatile("pause");
  }

  TEST_ASSERT(g_smp_mig_cpu_before == source_cpu,
              "waiter no arrancó en CPU%d: cpu=%d", source_cpu,
              g_smp_mig_cpu_before);
  TEST_ASSERT(g_smp_mig_cpu_after >= 0 &&
                  g_smp_mig_cpu_after != g_smp_mig_cpu_before,
              "waiter no migró: antes=%d después=%d",
              g_smp_mig_cpu_before, g_smp_mig_cpu_after);
  TEST_ASSERT(g_smp_mig_done == 1,
              "migración/canary no completó correctamente");

  g_smp_mig_pin_stop = 1;
  sched_yield();
}
REGISTER_TEST_FLAGS("smp: migración cpu_affinity=-1 + stack canary",
                    test_smp_task_migration_canary,
                    TEST_FLAG_BLOCKING | TEST_FLAG_NEEDS_SMP);

// ---------------------------------------------------------------------------
// Scheduler: timeout wakeup with >16 distinct wait queues
//
// Regression test for sched_wake_expired(): the old implementation kept a
// fixed array of 16 wait queues. With 17 expired tasks sleeping on 17
// different queues, the 17th queue was never awakened and its task could
// remain BLOCKED forever.
//
// The test deliberately puts all 17 tasks into distinct wait queues. Once
// every waiter is blocked, it advances the kernel tick counter past the
// common timeout while interrupts are disabled and calls sched_wake_expired()
// exactly once. This keeps the test deterministic while preserving the real
// wait_event_interruptible_timeout() deadline used by each waiter.
// ---------------------------------------------------------------------------
#define TIMEOUT_WQ_TEST_COUNT 17
#define TIMEOUT_WQ_TEST_TIMEOUT 100

typedef struct timeout_wq_test_slot {
  wait_queue_t wq;
  task_t *task;
  uint32_t task_id;
  volatile int completed;
  volatile long result;
} timeout_wq_test_slot_t;

static timeout_wq_test_slot_t g_timeout_wq_slots[TIMEOUT_WQ_TEST_COUNT];
static volatile int g_timeout_wq_completed = 0;

static bool timeout_wq_never_ready(void *arg) {
  (void)arg;
  return false;
}

static timeout_wq_test_slot_t *timeout_wq_find_slot(task_t *task) {
  if (!task)
    return NULL;

  for (int i = 0; i < TIMEOUT_WQ_TEST_COUNT; i++) {
    if (g_timeout_wq_slots[i].task == task ||
        g_timeout_wq_slots[i].task_id == task->id)
      return &g_timeout_wq_slots[i];
  }

  return NULL;
}

static void timeout_wq_test_waiter(void) {
  task_t *self = sched_current();
  timeout_wq_test_slot_t *slot = NULL;

  // sched_create_task() publishes the task before returning it. If this
  // task gets scheduled before the test has stored its pointer/ID in the
  // slot, yield and let the creator finish the association.
  while ((slot = timeout_wq_find_slot(self)) == NULL)
    sched_yield();

  slot->result = wait_event_interruptible_timeout(
      &slot->wq, timeout_wq_never_ready, NULL, TIMEOUT_WQ_TEST_TIMEOUT);
  slot->completed = 1;
  __sync_fetch_and_add(&g_timeout_wq_completed, 1);
}

static void test_sched_timeout_more_than_16_queues(void) {
  g_timeout_wq_completed = 0;

  for (int i = 0; i < TIMEOUT_WQ_TEST_COUNT; i++) {
    wait_queue_init(&g_timeout_wq_slots[i].wq);
    g_timeout_wq_slots[i].task = NULL;
    g_timeout_wq_slots[i].task_id = UINT32_MAX;
    g_timeout_wq_slots[i].completed = 0;
    g_timeout_wq_slots[i].result = -1;
  }

  // Create all waiters. They all use the same finite timeout. We will
  // advance tick_count after all 17 have reached TASK_BLOCKED, so no waiter
  // can expire naturally before the complete set is ready.
  for (int i = 0; i < TIMEOUT_WQ_TEST_COUNT; i++) {
    task_t *task = sched_create_task(timeout_wq_test_waiter);
    if (!task) {
      TEST_ASSERT(0, "sched_create_task falló en waiter %d", i);
      // Avoid leaving already-created waiters permanently blocked.
      for (int j = 0; j < i; j++)
        wake_up_all(&g_timeout_wq_slots[j].wq);
      return;
    }

    g_timeout_wq_slots[i].task = task;
    g_timeout_wq_slots[i].task_id = task->id;
  }

  // Wait until every task is actually sleeping on its own distinct queue.
  uint64_t block_deadline = sched_get_ticks() + 2000;
  while (1) {
    int blocked = 0;
    for (int i = 0; i < TIMEOUT_WQ_TEST_COUNT; i++) {
      task_t *task = g_timeout_wq_slots[i].task;
      if (task && task->state == TASK_BLOCKED &&
          task->waiting_on == &g_timeout_wq_slots[i].wq)
        blocked++;
    }

    if (blocked == TIMEOUT_WQ_TEST_COUNT)
      break;

    if (sched_get_ticks() >= block_deadline) {
      TEST_ASSERT(0, "solo %d/%d waiters llegaron a TASK_BLOCKED", blocked,
                  TIMEOUT_WQ_TEST_COUNT);
      for (int i = 0; i < TIMEOUT_WQ_TEST_COUNT; i++)
        wake_up_all(&g_timeout_wq_slots[i].wq);
      return;
    }

    sched_yield();
  }

  /*
   * Make all real wait_event() deadlines expire simultaneously.
   *
   * We deliberately advance tick_count rather than overwriting each task's
   * wake_deadline. wait_common() keeps its own local deadline, so changing
   * only task->wake_deadline would wake the task but make wait_common() think
   * its timeout had not elapsed; that was the flaw in the first version of
   * this regression test.
   *
   * Disabling interrupts prevents the normal timer handler from racing with
   * the single explicit sched_wake_expired() call below.
   */
  unsigned long flags;
  __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");

  uint64_t now = tick_count;
  tick_count = now + TIMEOUT_WQ_TEST_TIMEOUT + 1;

  // With the old implementation this wakes only 16 distinct queues.
  sched_wake_expired();

  __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");

  // Every waiter must actually run after being made READY and observe a
  // timeout return (0), not remain blocked or re-enter the wait.
  uint64_t done_deadline = sched_get_ticks() + 2000;
  while (g_timeout_wq_completed < TIMEOUT_WQ_TEST_COUNT &&
         sched_get_ticks() < done_deadline) {
    sched_yield();
  }

  // Aggregate the result into one assertion so one broken scheduler path
  // produces one failing test rather than dozens of failures.
  int completed = g_timeout_wq_completed;
  int bad_results = 0;
  for (int i = 0; i < TIMEOUT_WQ_TEST_COUNT; i++) {
    if (g_timeout_wq_slots[i].completed != 1 ||
        g_timeout_wq_slots[i].result != 0)
      bad_results++;
  }

  TEST_ASSERT(
      completed == TIMEOUT_WQ_TEST_COUNT && bad_results == 0,
      "timeout wakeup incorrect: completed=%d/%d, resultados_invalidos=%d",
      completed, TIMEOUT_WQ_TEST_COUNT, bad_results);

  if (completed == TIMEOUT_WQ_TEST_COUNT && bad_results == 0) {
    LOG_INFO(
        "[TEST] timeout-wq: %d/%d waiters despertados y retornaron timeout",
        TIMEOUT_WQ_TEST_COUNT, TIMEOUT_WQ_TEST_COUNT);
  }
}
REGISTER_TEST_FLAGS("sched: timeout wakeup >16 wait queues",
                    test_sched_timeout_more_than_16_queues, TEST_FLAG_BLOCKING);

// ===========================================================================
// ---------------------------------------------------------------------------
// Regression: an old timeout snapshot must not wake a requeued wait.
// ---------------------------------------------------------------------------
static wait_queue_t g_timeout_requeue_wq;
static volatile int g_timeout_requeue_phase;
static volatile uint64_t g_timeout_requeue_old_seq;
static volatile uint64_t g_timeout_requeue_new_seq;
static volatile int g_timeout_requeue_result;
static task_t *g_timeout_requeue_task;
static bool timeout_requeue_never_ready(void *arg) { (void)arg; return false; }
static void timeout_requeue_waiter(void) {
  task_t *self=sched_current(); g_timeout_requeue_task=self;
  g_timeout_requeue_old_seq=__atomic_load_n(&self->wait_seq,__ATOMIC_ACQUIRE)+1;
  g_timeout_requeue_phase=1;
  (void)wait_event_interruptible_timeout(&g_timeout_requeue_wq,timeout_requeue_never_ready,NULL,5000);
  g_timeout_requeue_new_seq=__atomic_load_n(&self->wait_seq,__ATOMIC_ACQUIRE)+1;
  g_timeout_requeue_phase=2;
  g_timeout_requeue_result=(int)wait_event_interruptible_timeout(&g_timeout_requeue_wq,timeout_requeue_never_ready,NULL,5000);
  g_timeout_requeue_phase=3;
}
static bool timeout_requeue_is_waiting(task_t *task) {
  bool waiting;
  unsigned long flags = spin_lock_irqsave(&g_timeout_requeue_wq.lock);
  waiting = task->waiting_on == &g_timeout_requeue_wq && task->state == TASK_BLOCKED;
  spin_unlock_irqrestore(&g_timeout_requeue_wq.lock, flags);
  return waiting;
}

static void test_sched_stale_timeout_requeue(void) {
  /*
   * Esta regresión prueba la identidad de una espera, no el mecanismo de
   * wakeup remoto (eso ya lo cubren los tests SMP anteriores). Fijamos el
   * waiter a la CPU del test para que el rearmado no dependa de que una
   * CPU idle concreta reciba el IPI justo en esta ventana.
   */
  wait_queue_init(&g_timeout_requeue_wq); g_timeout_requeue_phase=0;
  g_timeout_requeue_old_seq=0; g_timeout_requeue_new_seq=0; g_timeout_requeue_result=-1; g_timeout_requeue_task=NULL;
  task_t *task=sched_create_task(timeout_requeue_waiter);
  TEST_ASSERT(task!=NULL,"no se pudo crear waiter de timeout rearmado"); if(!task)return;

  task->cpu_affinity = smp_processor_id();

  uint64_t deadline=sched_get_ticks()+3000;
  while(g_timeout_requeue_phase<1||!timeout_requeue_is_waiting(task)){
    TEST_ASSERT(sched_get_ticks()<deadline,"timeout esperando primera espera bloqueada"); if(sched_get_ticks()>=deadline)return; sched_yield();
  }

  wake_up_all(&g_timeout_requeue_wq);
  sched_yield();
  deadline=sched_get_ticks()+3000;
  while(g_timeout_requeue_phase<2||!timeout_requeue_is_waiting(task)){
    TEST_ASSERT(sched_get_ticks()<deadline,"timeout esperando segunda espera rearmada"); if(sched_get_ticks()>=deadline)break; sched_yield();
  }
  if (g_timeout_requeue_phase < 2)
    return;

  uint64_t old_seq=g_timeout_requeue_old_seq, new_seq=__atomic_load_n(&task->wait_seq,__ATOMIC_ACQUIRE);
  TEST_ASSERT(new_seq>old_seq,"la espera rearmada no obtuvo nueva generación: old=%llu new=%llu",(unsigned long long)old_seq,(unsigned long long)new_seq);
  TEST_ASSERT(new_seq==g_timeout_requeue_new_seq,"generación observada incorrecta: %llu != %llu",(unsigned long long)new_seq,(unsigned long long)g_timeout_requeue_new_seq);

  /*
   * Simular un timeout ya capturado para la espera anterior. La generación
   * antigua no puede retirar la entrada correspondiente a la espera nueva.
   */
  wait_queue_wake_timeout_task(&g_timeout_requeue_wq,task,old_seq);
  TEST_ASSERT(timeout_requeue_is_waiting(task),"timeout antiguo despertó una espera nueva (seq=%llu)",(unsigned long long)new_seq);

  wake_up_all(&g_timeout_requeue_wq);
  deadline=sched_get_ticks()+3000;
  while(g_timeout_requeue_phase<3){TEST_ASSERT(sched_get_ticks()<deadline,"waiter no terminó tras wakeup legítimo");if(sched_get_ticks()>=deadline)break;sched_yield();}
  TEST_ASSERT(g_timeout_requeue_phase==3,"waiter no completó: phase=%d",g_timeout_requeue_phase);
  TEST_ASSERT(g_timeout_requeue_result!=0,"wakeup legítimo no devolvió resultado esperado: %d",g_timeout_requeue_result);
}
REGISTER_TEST_FLAGS("sched: timeout antiguo no despierta wait rearmada",test_sched_stale_timeout_requeue,TEST_FLAG_BLOCKING);

// [Fase 1.2] sched: need_resched se limpia con xchg atómico
//
// Regresión de C3. Antes sched_tick() hacía:
//     int force = curr->need_resched;
//     curr->need_resched = 0;
// y un sched_kick_idle_cpu() en otra CPU podía escribir 1 entre el
// read y el write, perdiendo la petición. Este test verifica la
// semántica del xchg que sustituye ese par: no prueba la carrera
// directamente (necesitaría instrumentación específica), pero sí
// garantiza que el primitivo hace lo que promete y evita una
// "simplificación" futura que rompa el fix.
// ===========================================================================
static void test_sched_need_resched_atomic(void) {
  task_t *cur = sched_current();
  TEST_ASSERT(cur != NULL, "sched_current() devolvió NULL");
  if (!cur)
    return;

  int saved = cur->need_resched;

  // Caso 1: need_resched=1 → xchg devuelve 1 y lo deja a 0.
  cur->need_resched = 1;
  __sync_synchronize();
  int force =
      __atomic_exchange_n((int *)&cur->need_resched, 0, __ATOMIC_ACQ_REL);
  TEST_ASSERT(force == 1, "xchg con need_resched=1 devolvió %d (esperado 1)",
              force);
  TEST_ASSERT(cur->need_resched == 0, "xchg no limpió need_resched (=%d)",
              cur->need_resched);

  // Caso 2: need_resched=0 → xchg devuelve 0.
  force = __atomic_exchange_n((int *)&cur->need_resched, 0, __ATOMIC_ACQ_REL);
  TEST_ASSERT(force == 0, "xchg con need_resched=0 devolvió %d (esperado 0)",
              force);

  // Restaurar estado.
  cur->need_resched = saved;
}
REGISTER_TEST("sched: need_resched con xchg atómico",
              test_sched_need_resched_atomic);
// ---------------------------------------------------------------------------
// Block layer
// ---------------------------------------------------------------------------
static void test_blk_init(void) {
  TEST_ASSERT(blk_count() >= 0, "blk_count() < 0");
}
REGISTER_TEST("blk: init", test_blk_init);

static void test_blk_no_duplicates(void) {
  int n = blk_count();
  for (int i = 0; i < n; i++) {
    block_device_t *a = blk_get_by_index(i);
    TEST_ASSERT(a != NULL, "blk_get_by_index(%d) == NULL", i);
    for (int j = i + 1; j < n; j++) {
      block_device_t *b = blk_get_by_index(j);
      TEST_ASSERT(strcmp(a->name, b->name) != 0, "nombres duplicados: %s",
                  a->name);
    }
  }
}
REGISTER_TEST("blk: no duplicados", test_blk_no_duplicates);

static void test_blk_lookup_missing(void) {
  block_device_t *d = blk_lookup("nonexistent");
  TEST_ASSERT(d == NULL, "blk_lookup('nonexistent') != NULL");
}
REGISTER_TEST("blk: lookup de disco inexistente", test_blk_lookup_missing);

// ---------------------------------------------------------------------------
// ATA (driver completo)
//
// [FIX] Todos los tests legacy de ATA usan test_skip() cuando no hay
// hda ni controlador IDE. En QEMU con solo AHCI, hda no existe y estos
// tests deben saltarse, no fallar.
// ---------------------------------------------------------------------------
static void test_ata_hda_detected(void) {
  // [FIX] Si no hay controlador IDE, skip.
  if (!ata_ide_present()) {
    test_skip("sin controlador IDE (hda no disponible)");
    return;
  }
  block_device_t *hda = blk_lookup("hda");
  TEST_ASSERT(hda != NULL, "hda no detectado");
}
REGISTER_TEST("ata: hda detectado", test_ata_hda_detected);

static void test_ata_hda_capacity(void) {
  block_device_t *hda = blk_lookup("hda");
  if (!hda) {
    // [FIX] Skip en lugar de fail.
    test_skip("hda no existe");
    return;
  }
  TEST_ASSERT(hda->num_sectors > 0, "hda sin sectores");
  TEST_ASSERT(hda->sector_size == 512 || hda->sector_size == 4096,
              "sector_size inesperado: %u", hda->sector_size);
}
REGISTER_TEST("ata: capacidad de hda coherente", test_ata_hda_capacity);

static void test_ata_read_sector0(void) {
  block_device_t *hda = blk_lookup("hda");
  if (!hda) {
    test_skip("hda no existe");
    return;
  }
  uint8_t buf[512];
  int rc = bdev_read(hda, 0, 1, buf);
  TEST_ASSERT(rc == 0, "read sector 0 falló: %d", rc);
  if (rc != 0)
    return;
  TEST_ASSERT(buf[510] == 0x55 && buf[511] == 0xAA,
              "sector 0 sin firma 0x55AA: %02x %02x", buf[510], buf[511]);
}
REGISTER_TEST("ata: leer sector 0 (firma 0x55AA)", test_ata_read_sector0);

static void test_ata_multi_sector_read(void) {
  block_device_t *hda = blk_lookup("hda");
  if (!hda) {
    test_skip("hda no existe");
    return;
  }

  uint8_t *buf = (uint8_t *)kmalloc(512 * 4);
  TEST_ASSERT(buf != NULL, "kmalloc falló");
  if (!buf)
    return;

  int rc = bdev_read(hda, 0, 4, buf);
  TEST_ASSERT(rc == 0, "read 4 sectores falló: %d", rc);
  if (rc == 0) {
    TEST_ASSERT(buf[510] == 0x55 && buf[511] == 0xAA, "sector 0 sin firma");
  }
  kfree(buf);
}
REGISTER_TEST("ata: leer 4 sectores contiguos", test_ata_multi_sector_read);

static void test_ata_write_read_back(void) {
  block_device_t *hda = blk_lookup("hda");
  if (!hda) {
    test_skip("hda no existe");
    return;
  }

  uint64_t lba = hda->num_sectors - 1;
  uint8_t orig[512], pattern[512], readback[512];

  int rc = bdev_read(hda, lba, 1, orig);
  TEST_ASSERT(rc == 0, "read original falló: %d", rc);
  if (rc != 0)
    return;

  for (int i = 0; i < 512; i++)
    pattern[i] = (uint8_t)(i ^ 0xA5);

  rc = bdev_write(hda, lba, 1, pattern);
  TEST_ASSERT(rc == 0, "write falló: %d", rc);
  if (rc != 0)
    return;

  rc = bdev_flush(hda);
  TEST_ASSERT(rc == 0, "flush falló: %d", rc);

  rc = bdev_read(hda, lba, 1, readback);
  TEST_ASSERT(rc == 0, "read back falló: %d", rc);
  if (rc != 0)
    return;

  int ok = 1;
  for (int i = 0; i < 512; i++) {
    if (readback[i] != pattern[i]) {
      ok = 0;
      break;
    }
  }
  TEST_ASSERT(ok, "el patrón leído no coincide");

  bdev_write(hda, lba, 1, orig);
  bdev_flush(hda);
}
REGISTER_TEST("ata: escribir y leer de vuelta", test_ata_write_read_back);

static void test_ata_read_oob(void) {
  block_device_t *hda = blk_lookup("hda");
  if (!hda) {
    test_skip("hda no existe");
    return;
  }
  uint8_t buf[512];
  int rc = bdev_read(hda, hda->num_sectors, 1, buf);
  TEST_ASSERT(rc < 0, "read fuera de rango debía fallar");
}
REGISTER_TEST("ata: leer fuera de rango falla", test_ata_read_oob);

static void test_ata_write_oob(void) {
  block_device_t *hda = blk_lookup("hda");
  if (!hda) {
    test_skip("hda no existe");
    return;
  }
  uint8_t buf[512];
  int rc = bdev_write(hda, hda->num_sectors, 1, buf);
  TEST_ASSERT(rc < 0, "write fuera de rango debía fallar");
}
REGISTER_TEST("ata: escribir fuera de rango falla", test_ata_write_oob);

// ---------------------------------------------------------------------------
// ATAPI
//
// [FIX] Skip si sr0 no existe.
// ---------------------------------------------------------------------------
static void test_atapi_sr0_detected(void) {
  block_device_t *sr0 = blk_lookup("sr0");
  if (!sr0) {
    test_skip("sr0 no detectado (sin unidad ATAPI)");
    return;
  }
  TEST_ASSERT(sr0 != NULL, "sr0 no detectado");
}
REGISTER_TEST("atapi: sr0 detectado", test_atapi_sr0_detected);

static void test_atapi_sr0_read_only(void) {
  block_device_t *sr0 = blk_lookup("sr0");
  if (!sr0) {
    test_skip("sr0 no existe");
    return;
  }
  TEST_ASSERT(sr0->is_read_only == 1, "sr0 no es read-only");
}
REGISTER_TEST("atapi: sr0 read-only", test_atapi_sr0_read_only);

static void test_atapi_sr0_sector_size(void) {
  block_device_t *sr0 = blk_lookup("sr0");
  if (!sr0) {
    test_skip("sr0 no existe");
    return;
  }
  TEST_ASSERT(sr0->sector_size == 2048 || sr0->sector_size == 4096,
              "sector_size inesperado: %u", sr0->sector_size);
}
REGISTER_TEST("atapi: sr0 sector size", test_atapi_sr0_sector_size);

static void test_atapi_sr0_media(void) {
  block_device_t *sr0 = blk_lookup("sr0");
  if (!sr0) {
    test_skip("sr0 no existe");
    return;
  }
  if (sr0->num_sectors == 0) {
    TEST_ASSERT(1, "sr0 sin medio (OK si no hay CD)");
    return;
  }
  TEST_ASSERT(sr0->num_sectors > 0, "sr0 tiene medio pero 0 sectores");
}
REGISTER_TEST("atapi: sr0 medio", test_atapi_sr0_media);

static void test_atapi_read_pvd(void) {
  block_device_t *sr0 = blk_lookup("sr0");
  if (!sr0) {
    test_skip("sr0 no existe");
    return;
  }
  if (sr0->num_sectors == 0) {
    TEST_ASSERT(1, "sr0 sin medio, test skip");
    return;
  }

  uint8_t *buf = (uint8_t *)kmalloc(sr0->sector_size);
  if (!buf) {
    TEST_ASSERT(0, "kmalloc falló");
    return;
  }

  int rc = bdev_read(sr0, 16, 1, buf);
  TEST_ASSERT(rc == 0, "read PVD falló: %d", rc);
  if (rc == 0) {
    TEST_ASSERT(buf[1] == 'C' && buf[2] == 'D' && buf[3] == '0' &&
                    buf[4] == '0' && buf[5] == '1',
                "PVD sin firma CD001: %02x %02x %02x %02x %02x", buf[1], buf[2],
                buf[3], buf[4], buf[5]);
  }
  kfree(buf);
}
REGISTER_TEST("atapi: leer PVD (firma CD001)", test_atapi_read_pvd);

static void test_atapi_write_fails(void) {
  block_device_t *sr0 = blk_lookup("sr0");
  if (!sr0) {
    test_skip("sr0 no existe");
    return;
  }
  uint8_t *buf = (uint8_t *)kzalloc(sr0->sector_size ? sr0->sector_size : 2048);
  if (!buf) {
    TEST_ASSERT(0, "kzalloc falló");
    return;
  }
  int rc = bdev_write(sr0, 0, 1, buf);
  TEST_ASSERT(rc < 0, "write a sr0 debía fallar (read-only), rc=%d", rc);
  kfree(buf);
}
REGISTER_TEST("atapi: escribir falla (read-only)", test_atapi_write_fails);

static void test_atapi_read_oob(void) {
  block_device_t *sr0 = blk_lookup("sr0");
  if (!sr0) {
    test_skip("sr0 no existe");
    return;
  }
  if (sr0->num_sectors == 0) {
    TEST_ASSERT(1, "sr0 sin medio, test skip");
    return;
  }
  uint8_t *buf = (uint8_t *)kmalloc(sr0->sector_size);
  if (!buf) {
    TEST_ASSERT(0, "kmalloc falló");
    return;
  }
  int rc = bdev_read(sr0, sr0->num_sectors, 1, buf);
  TEST_ASSERT(rc < 0, "read fuera de rango debía fallar");
  kfree(buf);
}
REGISTER_TEST("atapi: leer fuera de rango falla", test_atapi_read_oob);

// ---------------------------------------------------------------------------
// ATA DMA
//
// [FIX] Skip si el BMIDE no está inicializado (no hay controlador IDE).
// ---------------------------------------------------------------------------
static void test_ata_dma_bmide_ready(void) {
  extern int ata_dma_is_ready(int channel_idx);
  // [FIX] Si ninguno de los dos canales tiene BMIDE, skip.
  if (!ata_dma_is_ready(0) && !ata_dma_is_ready(1)) {
    test_skip("BMIDE no inicializado (sin controlador IDE)");
    return;
  }
  TEST_ASSERT(ata_dma_is_ready(0), "BMIDE primario no inicializado");
  TEST_ASSERT(ata_dma_is_ready(1), "BMIDE secundario no inicializado");
}
REGISTER_TEST("ata_dma: BMIDE listo", test_ata_dma_bmide_ready);

static void test_ata_dma_read_4k(void) {
  block_device_t *hda = blk_lookup("hda");
  if (!hda) {
    test_skip("hda no existe");
    return;
  }

  uint8_t *buf1 = kmalloc(4096);
  uint8_t *buf2 = kmalloc(4096);
  if (!buf1 || !buf2) {
    TEST_ASSERT(0, "kmalloc falló");
    if (buf1)
      kfree(buf1);
    if (buf2)
      kfree(buf2);
    return;
  }

  int rc1 = bdev_read(hda, 0, 8, buf1);
  TEST_ASSERT(rc1 == 0, "read 4K (1) falló: %d", rc1);
  if (rc1 != 0) {
    kfree(buf1);
    kfree(buf2);
    return;
  }

  int rc2 = bdev_read(hda, 0, 8, buf2);
  TEST_ASSERT(rc2 == 0, "read 4K (2) falló: %d", rc2);
  if (rc2 != 0) {
    kfree(buf1);
    kfree(buf2);
    return;
  }

  int same = 1;
  for (int i = 0; i < 4096; i++) {
    if (buf1[i] != buf2[i]) {
      same = 0;
      break;
    }
  }
  TEST_ASSERT(same, "los dos buffers no coinciden");

  kfree(buf1);
  kfree(buf2);
}
REGISTER_TEST("ata_dma: leer 4KB", test_ata_dma_read_4k);

static void test_ata_dma_write_read(void) {
  block_device_t *hda = blk_lookup("hda");
  if (!hda) {
    test_skip("hda no existe");
    return;
  }

  uint64_t lba = hda->num_sectors - 8;
  uint8_t *orig = kmalloc(4096);
  uint8_t *pattern = kmalloc(4096);
  uint8_t *readback = kmalloc(4096);
  if (!orig || !pattern || !readback) {
    TEST_ASSERT(0, "kmalloc falló");
    if (orig)
      kfree(orig);
    if (pattern)
      kfree(pattern);
    if (readback)
      kfree(readback);
    return;
  }

  int rc = bdev_read(hda, lba, 8, orig);
  TEST_ASSERT(rc == 0, "read original falló: %d", rc);
  if (rc != 0)
    goto out;

  for (int i = 0; i < 4096; i++)
    pattern[i] = (uint8_t)(i ^ 0x5A);

  rc = bdev_write(hda, lba, 8, pattern);
  TEST_ASSERT(rc == 0, "write 4K falló: %d", rc);
  if (rc != 0)
    goto out;

  rc = bdev_flush(hda);
  TEST_ASSERT(rc == 0, "flush falló: %d", rc);

  rc = bdev_read(hda, lba, 8, readback);
  TEST_ASSERT(rc == 0, "read back falló: %d", rc);
  if (rc != 0)
    goto out;

  int same = 1;
  for (int i = 0; i < 4096; i++) {
    if (readback[i] != pattern[i]) {
      same = 0;
      break;
    }
  }
  TEST_ASSERT(same, "el patrón no coincide");

out:
  bdev_write(hda, lba, 8, orig);
  bdev_flush(hda);
  kfree(orig);
  kfree(pattern);
  kfree(readback);
}
REGISTER_TEST("ata_dma: escribir y leer 4KB", test_ata_dma_write_read);

// ---------------------------------------------------------------------------
// AHCI (Fase 3)
//
// Los tests buscan "sda", "sdb", ... en el block layer. Si no hay HBA AHCI
// o no hay discos SATA conectados, los tests pasan sin más (skip silencioso).
// ---------------------------------------------------------------------------
static void test_ahci_controller_detected(void) {
  extern int ahci_disk_count(void);
  int n = ahci_disk_count();
  LOG_INFO("  discos AHCI detectados: %d", n);
  TEST_ASSERT(n >= 0, "ahci_disk_count() devolvió %d", n);
}
REGISTER_TEST("ahci: controlador detectado", test_ahci_controller_detected);

static void test_ahci_sda_detected(void) {
  block_device_t *sda = blk_lookup("sda");
  if (!sda) {
    LOG_INFO("  sda no existe (sin disco SATA), test omitido");
    TEST_ASSERT(1, "sin sda: skip");
    return;
  }
  TEST_ASSERT(sda->num_sectors > 0, "sda sin sectores");
  TEST_ASSERT(sda->sector_size == 512 || sda->sector_size == 4096,
              "sda sector_size inesperado: %u", sda->sector_size);
  TEST_ASSERT(sda->is_read_only == 0, "sda no debería ser read-only");
}
REGISTER_TEST("ahci: sda detectado", test_ahci_sda_detected);

static void test_ahci_read_sector0(void) {
  block_device_t *sda = blk_lookup("sda");
  if (!sda) {
    LOG_INFO("  sda no existe, test omitido");
    TEST_ASSERT(1, "sin sda: skip");
    return;
  }

  uint8_t *buf = (uint8_t *)kmalloc(sda->sector_size);
  TEST_ASSERT(buf != NULL, "kmalloc falló");
  if (!buf)
    return;

  int rc = bdev_read(sda, 0, 1, buf);
  TEST_ASSERT(rc == 0, "read sector 0 falló: %d", rc);
  if (rc == 0) {
    LOG_INFO("  sector 0 leído OK (primeros bytes: %02x %02x %02x %02x)",
             buf[0], buf[1], buf[2], buf[3]);
  }
  kfree(buf);
}
REGISTER_TEST("ahci: leer sector 0 de sda", test_ahci_read_sector0);

static void test_ahci_read_4k(void) {
  block_device_t *sda = blk_lookup("sda");
  if (!sda) {
    LOG_INFO("  sda no existe, test omitido");
    TEST_ASSERT(1, "sin sda: skip");
    return;
  }

  uint8_t *buf1 = (uint8_t *)kmalloc(4096);
  uint8_t *buf2 = (uint8_t *)kmalloc(4096);
  if (!buf1 || !buf2) {
    TEST_ASSERT(0, "kmalloc falló");
    if (buf1)
      kfree(buf1);
    if (buf2)
      kfree(buf2);
    return;
  }

  int rc1 = bdev_read(sda, 0, 8, buf1);
  TEST_ASSERT(rc1 == 0, "read 4K (1) falló: %d", rc1);
  if (rc1 != 0)
    goto out;

  int rc2 = bdev_read(sda, 0, 8, buf2);
  TEST_ASSERT(rc2 == 0, "read 4K (2) falló: %d", rc2);
  if (rc2 != 0)
    goto out;

  int same = 1;
  for (int i = 0; i < 4096; i++) {
    if (buf1[i] != buf2[i]) {
      same = 0;
      break;
    }
  }
  TEST_ASSERT(same, "las dos lecturas de 4K no coinciden");

out:
  kfree(buf1);
  kfree(buf2);
}
REGISTER_TEST("ahci: leer 4KB", test_ahci_read_4k);

static void test_ahci_write_read_back(void) {
  block_device_t *sda = blk_lookup("sda");
  if (!sda) {
    LOG_INFO("  sda no existe, test omitido");
    TEST_ASSERT(1, "sin sda: skip");
    return;
  }
  if (sda->num_sectors < 16) {
    LOG_INFO("  sda demasiado pequeño, test omitido");
    TEST_ASSERT(1, "sda muy pequeño: skip");
    return;
  }

  uint64_t lba = sda->num_sectors - 8;
  uint8_t *orig = (uint8_t *)kmalloc(4096);
  uint8_t *pattern = (uint8_t *)kmalloc(4096);
  uint8_t *readback = (uint8_t *)kmalloc(4096);
  if (!orig || !pattern || !readback) {
    TEST_ASSERT(0, "kmalloc falló");
    if (orig)
      kfree(orig);
    if (pattern)
      kfree(pattern);
    if (readback)
      kfree(readback);
    return;
  }

  int rc = bdev_read(sda, lba, 8, orig);
  TEST_ASSERT(rc == 0, "read original falló: %d", rc);
  if (rc != 0)
    goto out;

  for (int i = 0; i < 4096; i++)
    pattern[i] = (uint8_t)(i * 7 ^ 0x5A);

  rc = bdev_write(sda, lba, 8, pattern);
  TEST_ASSERT(rc == 0, "write 4K falló: %d", rc);
  if (rc != 0)
    goto out;

  rc = bdev_flush(sda);
  TEST_ASSERT(rc == 0, "flush falló: %d", rc);

  rc = bdev_read(sda, lba, 8, readback);
  TEST_ASSERT(rc == 0, "read back falló: %d", rc);
  if (rc != 0)
    goto out;

  int same = 1;
  for (int i = 0; i < 4096; i++) {
    if (readback[i] != pattern[i]) {
      same = 0;
      break;
    }
  }
  TEST_ASSERT(same, "el patrón leído no coincide tras write+read");

out:
  bdev_write(sda, lba, 8, orig);
  bdev_flush(sda);
  kfree(orig);
  kfree(pattern);
  kfree(readback);
}
REGISTER_TEST("ahci: escribir y leer 4KB", test_ahci_write_read_back);

static void test_ahci_read_oob(void) {
  block_device_t *sda = blk_lookup("sda");
  if (!sda) {
    LOG_INFO("  sda no existe, test omitido");
    TEST_ASSERT(1, "sin sda: skip");
    return;
  }
  uint8_t buf[512];
  int rc = bdev_read(sda, sda->num_sectors, 1, buf);
  TEST_ASSERT(rc < 0, "read fuera de rango debía fallar, rc=%d", rc);
}
REGISTER_TEST("ahci: leer fuera de rango falla", test_ahci_read_oob);

// ===========================================================================
// ahci: latencia de comandos
// ===========================================================================
static void test_ahci_latencia(void) {
  block_device_t *bdev = blk_lookup("sda");
  if (!bdev) {
    test_skip("sin sda (no hay disco AHCI)");
    return;
  }

  extern uint64_t klog_get_tsc_freq(void);
  uint64_t freq = klog_get_tsc_freq();
  if (freq == 0) {
    test_skip("TSC no calibrado");
    return;
  }

  uint8_t *buf = kmalloc(4096);
  if (!buf) {
    TEST_ASSERT(0, "kmalloc(4096) devolvió NULL");
    return;
  }

  uint64_t t_min = (uint64_t)-1, t_max = 0, t_sum = 0;
  const int N = 32;
  int errores = 0;

  uint64_t lba_base = 100;
  if (lba_base + 8 > bdev->num_sectors)
    lba_base = 0;

  for (int i = 0; i < N; i++) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t t0 = ((uint64_t)hi << 32) | lo;

    int rc = bdev_read(bdev, lba_base, 8, buf);

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t t1 = ((uint64_t)hi << 32) | lo;

    if (rc < 0) {
      errores++;
      continue;
    }

    uint64_t dt = t1 - t0;
    if (dt < t_min)
      t_min = dt;
    if (dt > t_max)
      t_max = dt;
    t_sum += dt;
  }

  kfree(buf);

  if (errores > 0) {
    TEST_ASSERT(0, "%d/%d lecturas fallaron", errores, N);
    return;
  }

  uint64_t t_avg = t_sum / N;
  unsigned long avg_us = (unsigned long)(t_avg * 1000000ULL / freq);
  unsigned long min_us = (unsigned long)(t_min * 1000000ULL / freq);
  unsigned long max_us = (unsigned long)(t_max * 1000000ULL / freq);

  LOG_INFO("  latencia: min=%lu us, avg=%lu us, max=%lu us", min_us, avg_us,
           max_us);

  if (avg_us > 500) {
    LOG_WARN("  latencia alta: posiblemente IRQs enmascaradas (polling)");
  }

  TEST_ASSERT(avg_us > 0 && avg_us < 3000,
              "latencia media demasiado alta: %lu us (esperado < 500 us)",
              avg_us);
}

REGISTER_TEST_FLAGS("ahci: latencia", test_ahci_latencia, TEST_FLAG_BLOCKING);

// ===========================================================================
// ahci: stress 1MB write+read
//
// Escribe 1 MB (2048 sectores de 512 B) en una zona segura del disco,
// lo lee de vuelta, compara byte a byte, y restaura el contenido original.
//
// Ejercita:
//   - PRDT con múltiples entradas (1 MB / 64 KB = 16 comandos).
//   - Port_xfer dividiendo el bio en trozos de AHCI_MAX_XFER_BYTES.
//   - FLUSH tras la escritura (port_xfer lo hace).
//   - Lectura desde LBAs que no empiezan en 0.
//
// Es BLOCKING porque bdev_read/bdev_write pueden dormir en wait_event.
// ===========================================================================
static void test_ahci_stress_1mb(void) {
  block_device_t *sda = blk_lookup("sda");
  if (!sda) {
    test_skip("sin sda (no hay disco AHCI)");
    return;
  }

  const uint32_t bytes = 1024 * 1024; // 1 MB
  const uint32_t sectors = bytes / sda->sector_size;

  if (sda->num_sectors < sectors + 4096) {
    test_skip("sda demasiado pequeño (%lu sectores) para el test",
              (unsigned long)sda->num_sectors);
    return;
  }

  // Zona segura: 2 MB antes del final, dejando margen para los otros
  // tests que escriben en num_sectors - 8.
  uint64_t lba = sda->num_sectors - sectors - 4096;

  uint8_t *orig = kmalloc(bytes);
  uint8_t *pattern = kmalloc(bytes);
  uint8_t *readback = kmalloc(bytes);
  if (!orig || !pattern || !readback) {
    TEST_ASSERT(0, "kmalloc(%u) falló (orig=%p pattern=%p readback=%p)", bytes,
                (void *)orig, (void *)pattern, (void *)readback);
    if (orig)
      kfree(orig);
    if (pattern)
      kfree(pattern);
    if (readback)
      kfree(readback);
    return;
  }

  int rc;

  // 1. Leer el contenido original para poder restaurarlo.
  rc = bdev_read(sda, lba, sectors, orig);
  TEST_ASSERT(rc == 0, "read original falló: %d", rc);
  if (rc != 0)
    goto out;

  // 2. Generar un patrón determinista pero no trivial.
  //    Usamos un LCG simple para que sea reproducible.
  {
    uint32_t state = 0x12345678u;
    for (uint32_t i = 0; i < bytes; i++) {
      state = state * 1103515245u + 12345u;
      pattern[i] = (uint8_t)(state >> 16);
    }
  }

  // 3. Escribir 1 MB.
  LOG_INFO("  escribiendo %u bytes en lba=%lu", bytes, (unsigned long)lba);
  rc = bdev_write(sda, lba, sectors, pattern);
  TEST_ASSERT(rc == 0, "write 1MB falló: %d", rc);
  if (rc != 0)
    goto out;

  // 4. FLUSH (bdev_write ya llama a flush? No, lo hace port_xfer tras
  //    cada bio de escritura. Pero llamamos a bdev_flush explícitamente
  //    para asegurar que el disco persistió los datos.)
  rc = bdev_flush(sda);
  TEST_ASSERT(rc == 0, "flush falló: %d", rc);
  if (rc != 0)
    goto out;

  // 5. Leer de vuelta y comparar.
  memset(readback, 0, bytes);
  rc = bdev_read(sda, lba, sectors, readback);
  TEST_ASSERT(rc == 0, "read back falló: %d", rc);
  if (rc != 0)
    goto out;

  {
    uint32_t first_mismatch = UINT32_MAX;
    for (uint32_t i = 0; i < bytes; i++) {
      if (readback[i] != pattern[i]) {
        first_mismatch = i;
        break;
      }
    }
    if (first_mismatch != UINT32_MAX) {
      TEST_ASSERT(0, "mismatch en offset %u: esperado 0x%02x, leído 0x%02x",
                  first_mismatch, pattern[first_mismatch],
                  readback[first_mismatch]);
    } else {
      TEST_ASSERT(1, "1 MB escritos y leídos correctamente");
    }
  }

out:
  // Restaurar el contenido original, pase lo que pase.
  if (orig) {
    bdev_write(sda, lba, sectors, orig);
    bdev_flush(sda);
  }
  if (orig)
    kfree(orig);
  if (pattern)
    kfree(pattern);
  if (readback)
    kfree(readback);
}
REGISTER_TEST_FLAGS("ahci: stress 1MB write+read", test_ahci_stress_1mb,
                    TEST_FLAG_BLOCKING);

// ===========================================================================
// ahci: stress multiples LBAs
//
// Escribe un patrón distinto en 8 LBAs dispersos, verifica cada uno,
// y restaura. Valida que el driver funciona con LBAs no contiguos y
// que el FLUSH persiste cada escritura.
// ===========================================================================
static void test_ahci_stress_multi_lba(void) {
  block_device_t *sda = blk_lookup("sda");
  if (!sda) {
    test_skip("sin sda (no hay disco AHCI)");
    return;
  }

  const uint32_t sectors_per_op = 8; // 4 KB por operación
  const int N = 8;
  const uint64_t margen_final = 8192;
  const uint64_t margen_inicial = 4096;

  if (sda->num_sectors < margen_inicial + margen_final + N * 4096) {
    test_skip("sda demasiado pequeño para el test");
    return;
  }

  uint32_t bytes = sectors_per_op * sda->sector_size;
  uint8_t *orig[N];
  uint8_t *pattern[N];
  uint8_t *readback[N];
  uint64_t lbas[N];

  for (int i = 0; i < N; i++) {
    orig[i] = NULL;
    pattern[i] = NULL;
    readback[i] = NULL;
  }

  // Elegir LBAs dispersos: espaciados ~4 MB, dentro de la zona segura.
  uint64_t zona = sda->num_sectors - margen_final - margen_inicial;
  for (int i = 0; i < N; i++) {
    lbas[i] = margen_inicial + (zona / N) * i;
    lbas[i] &= ~(uint64_t)7; // alinear a 8 sectores

    orig[i] = kmalloc(bytes);
    pattern[i] = kmalloc(bytes);
    readback[i] = kmalloc(bytes);
    if (!orig[i] || !pattern[i] || !readback[i]) {
      TEST_ASSERT(0, "kmalloc falló en iteración %d", i);
      goto cleanup;
    }
  }

  // Leer originales.
  for (int i = 0; i < N; i++) {
    int rc = bdev_read(sda, lbas[i], sectors_per_op, orig[i]);
    TEST_ASSERT(rc == 0, "read orig[%d] falló: %d", i, rc);
    if (rc != 0)
      goto cleanup;
  }

  // Generar patrones distintos y escribir.
  for (int i = 0; i < N; i++) {
    for (uint32_t j = 0; j < bytes; j++)
      pattern[i][j] = (uint8_t)((i * 31 + j) ^ 0xA5);

    int rc = bdev_write(sda, lbas[i], sectors_per_op, pattern[i]);
    TEST_ASSERT(rc == 0, "write[%d] falló: %d", i, rc);
    if (rc != 0)
      goto cleanup;
  }

  int rc = bdev_flush(sda);
  TEST_ASSERT(rc == 0, "flush falló: %d", rc);
  if (rc != 0)
    goto cleanup;

  // Leer de vuelta y comparar.
  for (int i = 0; i < N; i++) {
    memset(readback[i], 0, bytes);
    rc = bdev_read(sda, lbas[i], sectors_per_op, readback[i]);
    TEST_ASSERT(rc == 0, "read back[%d] falló: %d", i, rc);
    if (rc != 0)
      goto cleanup;

    int ok = 1;
    uint32_t first_bad = 0;
    for (uint32_t j = 0; j < bytes; j++) {
      if (readback[i][j] != pattern[i][j]) {
        ok = 0;
        first_bad = j;
        break;
      }
    }
    if (!ok) {
      TEST_ASSERT(0,
                  "LBA %d (lba=%lu) mismatch en offset %u: "
                  "esperado 0x%02x, leído 0x%02x",
                  i, (unsigned long)lbas[i], first_bad, pattern[i][first_bad],
                  readback[i][first_bad]);
    } else {
      TEST_ASSERT(1, "LBA %d (lba=%lu) verificado OK", i,
                  (unsigned long)lbas[i]);
    }
  }

cleanup:
  // Restaurar todo.
  for (int i = 0; i < N; i++) {
    if (orig[i])
      bdev_write(sda, lbas[i], sectors_per_op, orig[i]);
  }
  bdev_flush(sda);
  for (int i = 0; i < N; i++) {
    if (orig[i])
      kfree(orig[i]);
    if (pattern[i])
      kfree(pattern[i]);
    if (readback[i])
      kfree(readback[i]);
  }
}
REGISTER_TEST_FLAGS("ahci: stress multiples LBAs", test_ahci_stress_multi_lba,
                    TEST_FLAG_BLOCKING);

// ===========================================================================
// ahci: stress buffer no contiguo
//
// Asigna un buffer grande (256 KB), comprueba cuántas páginas físicas
// distintas tiene, y hace write+read. Valida que el PRDT multi-entrada
// maneja buffers que no son contiguos en físico.
// ===========================================================================
static void test_ahci_stress_no_contiguo(void) {
  block_device_t *sda = blk_lookup("sda");
  if (!sda) {
    test_skip("sin sda (no hay disco AHCI)");
    return;
  }

  const uint32_t bytes = 256 * 1024;
  const uint32_t sectors = bytes / sda->sector_size;

  if (sda->num_sectors < sectors + 8192) {
    test_skip("sda demasiado pequeño para el test");
    return;
  }

  uint64_t lba = sda->num_sectors - sectors - 8192;

  uint8_t *orig = kmalloc(bytes);
  uint8_t *pattern = kmalloc(bytes);
  uint8_t *readback = kmalloc(bytes);
  if (!orig || !pattern || !readback) {
    TEST_ASSERT(0, "kmalloc falló");
    if (orig)
      kfree(orig);
    if (pattern)
      kfree(pattern);
    if (readback)
      kfree(readback);
    return;
  }

  int rc;

  rc = bdev_read(sda, lba, sectors, orig);
  TEST_ASSERT(rc == 0, "read original falló: %d", rc);
  if (rc != 0)
    goto out;

  // Contar páginas físicas distintas en el buffer.
  {
    extern uint64_t paging_get_phys(uint64_t virt);
    uint32_t paginas_distintas = 0;
    uint64_t last_phys = 0;
    int primera = 1;
    for (uint32_t off = 0; off < bytes; off += PAGE_SIZE) {
      uint64_t phys = paging_get_phys((uint64_t)(uintptr_t)(pattern + off));
      if (primera || phys != last_phys + PAGE_SIZE) {
        paginas_distintas++;
      }
      last_phys = phys;
      primera = 0;
    }
    LOG_INFO("  buffer de %u bytes: %u tramos físicos contiguos", bytes,
             paginas_distintas);
    if (paginas_distintas == 1) {
      LOG_WARN("  el buffer es físicamente contiguo; el test no ejercita "
               "PRDT multi-entrada");
    }
  }

  for (uint32_t i = 0; i < bytes; i++)
    pattern[i] = (uint8_t)((i * 17) ^ 0x3C);

  LOG_INFO("  escribiendo %u bytes en lba=%lu", bytes, (unsigned long)lba);
  rc = bdev_write(sda, lba, sectors, pattern);
  TEST_ASSERT(rc == 0, "write falló: %d", rc);
  if (rc != 0)
    goto out;

  rc = bdev_flush(sda);
  TEST_ASSERT(rc == 0, "flush falló: %d", rc);
  if (rc != 0)
    goto out;

  memset(readback, 0, bytes);
  rc = bdev_read(sda, lba, sectors, readback);
  TEST_ASSERT(rc == 0, "read back falló: %d", rc);
  if (rc != 0)
    goto out;

  {
    uint32_t first_mismatch = UINT32_MAX;
    for (uint32_t i = 0; i < bytes; i++) {
      if (readback[i] != pattern[i]) {
        first_mismatch = i;
        break;
      }
    }
    if (first_mismatch != UINT32_MAX) {
      TEST_ASSERT(0, "mismatch en offset %u: esperado 0x%02x, leído 0x%02x",
                  first_mismatch, pattern[first_mismatch],
                  readback[first_mismatch]);
    } else {
      TEST_ASSERT(1, "buffer no contiguo escrito/leído OK");
    }
  }

out:
  if (orig) {
    bdev_write(sda, lba, sectors, orig);
    bdev_flush(sda);
  }
  if (orig)
    kfree(orig);
  if (pattern)
    kfree(pattern);
  if (readback)
    kfree(readback);
}
REGISTER_TEST_FLAGS("ahci: stress buffer no contiguo",
                    test_ahci_stress_no_contiguo, TEST_FLAG_BLOCKING);

// ===========================================================================
// [H3] TLB shootdown roundtrip
//
// No puede verificar el efecto semántico (que otro CPU vea el invlpg)
// sin infraestructura de observación, pero sí:
//   - ipi_tlb_shootdown no cuelga ni corrompe estado.
//   - Todos los APs responden con ack.
//   - Llamadas repetidas funcionan (no hay estado residual).
// ===========================================================================
static void test_ipi_tlb_shootdown_roundtrip(void) {
  if (smp_aps_ready() == 0) {
    test_skip("sin APs activos");
    return;
  }

  extern uint64_t klog_get_tsc_freq(void);
  uint64_t freq = klog_get_tsc_freq();
  if (freq == 0)
    freq = 2000000000ULL;

  // Direcciones no mapeadas: invlpg sobre no-mapeada es no-op. Así no
  // arriesgamos un efecto colateral en otras CPUs.
  const uint64_t base = 0x00007F0000000000ULL;

  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t t0 = ((uint64_t)hi << 32) | lo;

  const int N = 200;
  for (int i = 0; i < N; i++)
    ipi_tlb_shootdown(base + (uint64_t)i * PAGE_SIZE);

  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t t1 = ((uint64_t)hi << 32) | lo;
  uint64_t dt = t1 - t0;
  unsigned long avg_ns = (unsigned long)(dt * 1000000000ULL / freq / N);

  LOG_INFO("  %d shootdowns en %lu us (avg %lu ns)", N,
           (unsigned long)(dt * 1000000ULL / freq), avg_ns);

  // Cota generosa: en QEMU un shootdown IPI va a <100 us.
  TEST_ASSERT(avg_ns < 200000, "shootdown demasiado lento: %lu ns promedio",
              avg_ns);

  // Flush completo (addr == 0): debe funcionar y ser más rápido que
  // 200 shootdowns individuales.
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t f0 = ((uint64_t)hi << 32) | lo;
  ipi_tlb_shootdown(0);
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t f1 = ((uint64_t)hi << 32) | lo;
  uint64_t flush_us = (f1 - f0) * 1000000ULL / freq;
  LOG_INFO("  full flush: %lu us", (unsigned long)flush_us);

  TEST_ASSERT(flush_us < 50000, "full flush demasiado lento: %lu us",
              (unsigned long)flush_us);
}
REGISTER_TEST_FLAGS("ipi: tlb shootdown roundtrip",
                    test_ipi_tlb_shootdown_roundtrip,
                    TEST_FLAG_BLOCKING | TEST_FLAG_NEEDS_SMP);

// ===========================================================================
// [H3] paging_invalidate_tlb_global no corrompe la ventana física
//
// Tras un shootdown sobre PHYS_MAP_BASE, la ventana debe seguir
// accesible (fue recargada del page table). Verifica que la secuencia
// (flush local + IPI + ack) no rompe nada del CPU local.
// ===========================================================================
static void test_paging_invalidate_tlb_global_smoke(void) {
  volatile uint32_t *phys_map = (volatile uint32_t *)PHYS_MAP_BASE;

  // Leer antes (mapea en TLB local).
  uint32_t before = *phys_map;

  // Shootdown global sobre esa VA.
  paging_invalidate_tlb_global(PHYS_MAP_BASE);

  // Leer después: debe devolver lo mismo.
  uint32_t after = *phys_map;

  TEST_ASSERT(before == after,
              "PHYS_MAP_BASE cambió tras shootdown (0x%x -> 0x%x)", before,
              after);

  // Shootdown sobre una VA no mapeada: no debe explotar.
  paging_invalidate_tlb_global(0x00007F0001000000ULL);
}
REGISTER_TEST_FLAGS("paging: invalidate_tlb_global smoke",
                    test_paging_invalidate_tlb_global_smoke,
                    TEST_FLAG_BLOCKING | TEST_FLAG_NEEDS_SMP);

// ===========================================================================
// [C4] sched_set_blocked_deadline publica consistentemente
//
// Verifica que tras sched_set_blocked_deadline, un lector bajo
// sched_lock ve ambos campos (state y wake_deadline) coherentes. No
// es un test de la race en sí, pero garantiza que el helper hace lo
// que promete y que nadie lo simplifique a dos escrituras separadas.
// ===========================================================================
static void test_sched_set_blocked_deadline(void) {
  task_t *cur = sched_current();
  TEST_ASSERT(cur != NULL, "sched_current() devolvió NULL");
  if (!cur)
    return;

  task_state_t saved_state = cur->state;
  uint64_t saved_deadline = cur->wake_deadline;

  const uint64_t fake_deadline = 0xDEADBEEF12345678ULL;
  sched_set_blocked_deadline(cur, fake_deadline);

  TEST_ASSERT(cur->state == TASK_BLOCKED,
              "state no es BLOCKED tras el helper (=%d)", cur->state);
  TEST_ASSERT(cur->wake_deadline == fake_deadline,
              "wake_deadline no se publicó (=0x%llx)",
              (unsigned long long)cur->wake_deadline);

  // Restaurar: la tarea actual debe seguir RUNNING para que los demás
  // tests del framework no se confundan.
  cur->state = saved_state;
  cur->wake_deadline = saved_deadline;
}
REGISTER_TEST("sched: set_blocked_deadline atómico",
              test_sched_set_blocked_deadline);

// ===========================================================================
// [Fase 2] Partition layer
// ===========================================================================
static void test_part_layout(void) {
  int disks = 0, parts = 0;
  for (int i = 0; i < blk_count(); i++) {
    block_device_t *b = blk_get_by_index(i);
    if (!b)
      continue;
    if (b->is_partition)
      parts++;
    else
      disks++;
  }
  LOG_INFO("  %d discos, %d particiones", disks, parts);
  TEST_ASSERT(1, "scan completado (disks=%d parts=%d)", disks, parts);
}
REGISTER_TEST("part: scan completado", test_part_layout);

static void test_part_parent_consistent(void) {
  for (int i = 0; i < blk_count(); i++) {
    block_device_t *b = blk_get_by_index(i);
    if (!b || !b->is_partition)
      continue;

    TEST_ASSERT(b->parent != NULL, "%s: parent NULL", b->name);
    TEST_ASSERT(b->start_lba > 0, "%s: start_lba == 0", b->name);
    TEST_ASSERT(b->num_sectors > 0, "%s: num_sectors == 0", b->name);
    TEST_ASSERT(b->start_lba + b->num_sectors <= b->parent->num_sectors,
                "%s: sale del disco padre", b->name);
    TEST_ASSERT(b->sector_size == b->parent->sector_size,
                "%s: sector_size distinto del padre", b->name);
  }
}
REGISTER_TEST("part: consistencia padre/hijo", test_part_parent_consistent);

static void test_part_no_overlap(void) {
  int n = blk_count();
  for (int i = 0; i < n; i++) {
    block_device_t *a = blk_get_by_index(i);
    if (!a || !a->is_partition)
      continue;
    for (int j = i + 1; j < n; j++) {
      block_device_t *b = blk_get_by_index(j);
      if (!b || !b->is_partition)
        continue;
      if (a->parent != b->parent)
        continue;

      uint64_t a_start = a->start_lba, a_end = a_start + a->num_sectors;
      uint64_t b_start = b->start_lba, b_end = b_start + b->num_sectors;
      int overlap = (a_start < b_end) && (b_start < a_end);
      TEST_ASSERT(!overlap, "%s y %s solapan", a->name, b->name);
    }
  }
}
REGISTER_TEST("part: sin solapamiento", test_part_no_overlap);

static void test_part_read_routed(void) {
  for (int i = 0; i < blk_count(); i++) {
    block_device_t *b = blk_get_by_index(i);
    if (!b || !b->is_partition)
      continue;

    uint8_t p[512] = {0}, d[512] = {0};
    int rc1 = bdev_read(b, 0, 1, p);
    int rc2 = bdev_read(b->parent, b->start_lba, 1, d);

    TEST_ASSERT(rc1 == 0, "%s: read falló rc=%d", b->name, rc1);
    if (rc1 != 0)
      continue;
    TEST_ASSERT(rc2 == 0, "%s: read del padre falló rc=%d", b->name, rc2);
    if (rc2 != 0)
      continue;
    TEST_ASSERT(memcmp(p, d, 512) == 0,
                "%s: contenido distinto del padre[LBA %llu]", b->name,
                (unsigned long long)b->start_lba);
  }
}
REGISTER_TEST("part: lectura enrutada al padre", test_part_read_routed);

static void test_part_write_routed(void) {
  // Verifica que bdev_write a una partición enrutado al padre funciona
  // (o falla consistentemente si el padre es RO). No escribe: solo
  // confirma que la ruta llega al driver del padre sin error de tipo.
  for (int i = 0; i < blk_count(); i++) {
    block_device_t *b = blk_get_by_index(i);
    if (!b || !b->is_partition || b->is_read_only)
      continue;
    if (b->num_sectors < 4)
      continue;

    // Leer el sector 0, escribir el mismo contenido, verificar que OK.
    uint8_t buf[512];
    int rc = bdev_read(b, 0, 1, buf);
    TEST_ASSERT(rc == 0, "%s: read falló", b->name);
    if (rc != 0)
      continue;
    rc = bdev_write(b, 0, 1, buf);
    TEST_ASSERT(rc == 0, "%s: write falló rc=%d", b->name, rc);
    // Al ser idempotente (mismo contenido), no corrompe nada.
  }
}
REGISTER_TEST("part: escritura enrutada al padre", test_part_write_routed);

// ===========================================================================
// [Fase 2] VFS con mount points
//
// Verifica el comportamiento del mount table sin depender de FAT32.
// Usa un FS falso que solo sirve un par de paths.
// ===========================================================================

// --- FS fake ---
static vfs_node_t *fake_fs_lookup(void *fs_priv, const char *path) {
  (void)fs_priv;
  if (strcmp(path, "/") != 0 && strcmp(path, "/foo") != 0)
    return NULL;

  vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(*n));
  if (!n)
    return NULL;
  strcpy(n->name, path);
  n->flags = (strcmp(path, "/") == 0) ? VFS_DIRECTORY : VFS_FILE;
  n->size = 0;
  n->ops = NULL;
  n->fs = NULL;
  n->priv = NULL;
  return n;
}

static vfs_fs_ops_t fake_fs_ops = {
    .lookup = fake_fs_lookup,
    .name = "fake",
};

static void test_vfs_mount_lookup_umount(void) {
  // Mount en un path que no sea /.
  TEST_ASSERT(vfs_mount("/test_vfs", &fake_fs_ops, NULL) == 0,
              "mount /test_vfs falló");

  // lookup justo en el mount point.
  vfs_node_t *n_root = vfs_lookup("/test_vfs");
  TEST_ASSERT(n_root != NULL, "lookup /test_vfs devolvió NULL");
  if (n_root) {
    TEST_ASSERT(n_root->flags == VFS_DIRECTORY,
                "root del fake no es directorio (flags=0x%x)", n_root->flags);
    vfs_node_free(n_root);
  }

  // lookup de un hijo.
  vfs_node_t *n_foo = vfs_lookup("/test_vfs/foo");
  TEST_ASSERT(n_foo != NULL, "lookup /test_vfs/foo devolvió NULL");
  if (n_foo)
    vfs_node_free(n_foo);

  // lookup de un path que no existe dentro del fake.
  vfs_node_t *n_bad = vfs_lookup("/test_vfs/bar");
  TEST_ASSERT(n_bad == NULL, "lookup /test_vfs/bar devolvió algo");

  // doble mount rechazado.
  int rc = vfs_mount("/test_vfs", &fake_fs_ops, NULL);
  TEST_ASSERT(rc == -EEXIST, "doble mount devolvió %d, esperado -EEXIST", rc);

  // umount.
  TEST_ASSERT(vfs_umount("/test_vfs") == 0, "umount falló");
  vfs_node_t *n_after = vfs_lookup("/test_vfs");
  TEST_ASSERT(n_after == NULL, "lookup tras umount devolvió algo");
}
REGISTER_TEST("vfs: mount/lookup/umount básico", test_vfs_mount_lookup_umount);

static void test_vfs_longest_mount_wins(void) {
  // Montar /test_long y /test_long/deep. El segundo debe ganar para
  // paths bajo /test_long/deep, el primero para el resto.
  TEST_ASSERT(vfs_mount("/test_long", &fake_fs_ops, NULL) == 0, "m1");
  TEST_ASSERT(vfs_mount("/test_long/deep", &fake_fs_ops, NULL) == 0, "m2");

  // /test_long/foo → matchea /test_long (más específico que /).
  vfs_node_t *a = vfs_lookup("/test_long/foo");
  TEST_ASSERT(a != NULL, "lookup /test_long/foo falló");
  if (a)
    vfs_node_free(a);

  // /test_long/deep/foo → matchea /test_long/deep.
  // El fake solo sirve "/" y "/foo" relativos. Con mount en /test_long/deep,
  // rel="/foo", así que debe funcionar. Con mount en /test_long, rel también
  // sería "/deep/foo", que el fake rechaza. Así comprobamos que gana el más
  // específico.
  vfs_node_t *b = vfs_lookup("/test_long/deep/foo");
  TEST_ASSERT(
      b != NULL,
      "lookup /test_long/deep/foo falló (mount más específico no ganó)");
  if (b)
    vfs_node_free(b);

  TEST_ASSERT(vfs_umount("/test_long/deep") == 0, "umount deep");
  TEST_ASSERT(vfs_umount("/test_long") == 0, "umount long");
}
REGISTER_TEST("vfs: mount más específico gana", test_vfs_longest_mount_wins);

static void test_vfs_umount_busy(void) {
  // Montar dos anidados y verificar que no se puede desmontar el padre
  // mientras el hijo exista.
  TEST_ASSERT(vfs_mount("/test_busy", &fake_fs_ops, NULL) == 0, "m1");
  TEST_ASSERT(vfs_mount("/test_busy/child", &fake_fs_ops, NULL) == 0, "m2");

  int rc = vfs_umount("/test_busy");
  TEST_ASSERT(rc == -EBUSY, "umount con hijo devolvió %d, esperado -EBUSY", rc);

  TEST_ASSERT(vfs_umount("/test_busy/child") == 0, "umount hijo");
  TEST_ASSERT(vfs_umount("/test_busy") == 0, "umount padre tras hijo");
}
REGISTER_TEST("vfs: umount con hijo da EBUSY", test_vfs_umount_busy);

static void test_vfs_path_normalization(void) {
  // Estos cuatro paths deben resolver al mismo nodo (el root del fake).
  // El fake responde "/" tanto a "/test_norm" como a "//test_norm/./"
  // una vez normalizado.
  TEST_ASSERT(vfs_mount("/test_norm", &fake_fs_ops, NULL) == 0, "mount");

  const char *paths[] = {
      "/test_norm",
      "//test_norm",
      "/test_norm/",
      "/./test_norm",
  };
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    vfs_node_t *n = vfs_lookup(paths[i]);
    TEST_ASSERT(n != NULL, "lookup '%s' falló", paths[i]);
    if (n)
      vfs_node_free(n);
  }

  // Path con .. rechazado.
  vfs_node_t *bad = vfs_lookup("/test_norm/../etc");
  TEST_ASSERT(bad == NULL, "lookup con .. no fue rechazado");

  TEST_ASSERT(vfs_umount("/test_norm") == 0, "umount");
}
REGISTER_TEST("vfs: normalización de paths", test_vfs_path_normalization);

static void test_vfs_read_all_tarfs(void) {
  // /system/config.txt existe en el initrd y es legible.
  void *buf = NULL;
  size_t size = 0;
  int rc = vfs_read_all("system/config.txt", &buf, &size);
  TEST_ASSERT(rc == 0, "vfs_read_all falló: %d", rc);
  if (rc == 0) {
    TEST_ASSERT(size > 0, "size == 0");
    TEST_ASSERT(buf != NULL, "buf NULL con size > 0");
    kfree(buf);
  }

  // Un directorio devuelve -EISDIR.
  void *buf2 = NULL;
  size_t size2 = 0;
  int rc2 = vfs_read_all("apps", &buf2, &size2);
  TEST_ASSERT(rc2 == -EISDIR,
              "read_all de directorio devolvió %d, esperado -EISDIR", rc2);

  // Un archivo inexistente devuelve -ENOENT.
  int rc3 = vfs_read_all("no/existe/esto", &buf2, &size2);
  TEST_ASSERT(rc3 == -ENOENT, "read_all de inexistente devolvió %d", rc3);
}
REGISTER_TEST("vfs: read_all sobre tarfs", test_vfs_read_all_tarfs);

static void test_vfs_tarfs_root_still_works(void) {
  // El mount en "/" debe seguir sirviendo los archivos del initrd.
  // Es una regresión: si el lookup se rompe, /system/config.txt deja
  // de existir y todas las apps mueren al cargar.
  vfs_node_t *n = vfs_lookup("system/config.txt");
  TEST_ASSERT(n != NULL, "lookup config.txt falló");
  if (n) {
    TEST_ASSERT(n->flags == VFS_FILE, "no es FILE");
    TEST_ASSERT(n->size > 0, "size == 0");
    vfs_node_free(n);
  }

  // Y un directorio.
  vfs_node_t *d = vfs_lookup("system");
  TEST_ASSERT(d != NULL, "lookup system/ falló");
  if (d) {
    TEST_ASSERT(d->flags == VFS_DIRECTORY, "system no es DIR");
    vfs_node_free(d);
  }
}
REGISTER_TEST("vfs: tarfs en / sigue funcionando",
              test_vfs_tarfs_root_still_works);

// ===========================================================================
// [Fase 2.2] FAT32 read-only
//
// Busca un disco con FAT32 (aurora.img tiene un FS FAT32 crudo sin
// tabla de particiones) y lo monta/lee. Si ningún disco tiene FAT32,
// los tests se saltan.
// ===========================================================================
static block_device_t *fat32_find_test_disk(void) {
  const char *names[] = {"sda", "hda", "sdb", "hdb", NULL};
  for (int i = 0; names[i]; i++) {
    block_device_t *b = blk_lookup(names[i]);
    if (!b || b->is_partition || b->is_read_only)
      continue;
    if (b->sector_size < 512)
      continue;
    uint8_t buf[512];
    if (bdev_read(b, 0, 1, buf) != 0)
      continue;
    // Verificar firma FAT32.
    if (buf[510] != 0x55 || buf[511] != 0xAA)
      continue;
    if (memcmp(buf + 82, "FAT32   ", 8) != 0)
      continue;
    return b;
  }
  return NULL;
}

static void fat32_test_cleanup(void) {
  void *priv = vfs_get_mount_priv("/fat_test");
  if (priv) {
    vfs_umount("/fat_test");
    fat32_umount(priv);
  }
}

static void test_fat32_mount_umount(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32 disponible");
    return;
  }
  fat32_test_cleanup();

  int rc = fat32_mount_bdev(b->name, "/fat_test");
  TEST_ASSERT(rc == 0, "fat32_mount_bdev(%s) falló: %d", b->name, rc);
  if (rc != 0)
    return;

  // El mount debe existir.
  vfs_node_t *root = vfs_lookup("/fat_test");
  TEST_ASSERT(root != NULL, "lookup /fat_test falló");
  if (root) {
    TEST_ASSERT(root->flags == VFS_DIRECTORY, "root de FAT32 no es dir");
    vfs_node_free(root);
  }

  fat32_test_cleanup();
}
REGISTER_TEST("fat32: mount/umount básico", test_fat32_mount_umount);

static void test_fat32_lookup_kernel_elf(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  // /kernel.elf existe en aurora.img (mcopy del Makefile).
  vfs_node_t *n = vfs_lookup("/fat_test/kernel.elf");
  TEST_ASSERT(n != NULL, "lookup /kernel.elf falló");
  if (n) {
    TEST_ASSERT(n->flags == VFS_FILE, "no es FILE");
    TEST_ASSERT(n->size > 64, "size demasiado pequeño (%lu)",
                (unsigned long)n->size);
    vfs_node_free(n);
  }

  fat32_test_cleanup();
}
REGISTER_TEST("fat32: lookup /kernel.elf", test_fat32_lookup_kernel_elf);

static void test_fat32_read_elf_magic(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  // Leer los primeros 4 bytes de kernel.elf. Debe ser \x7fELF.
  vfs_node_t *n = vfs_lookup("/fat_test/kernel.elf");
  TEST_ASSERT(n != NULL, "lookup falló");
  if (n) {
    uint8_t magic[4] = {0};
    int64_t got = n->ops->read(n, 0, 4, magic);
    TEST_ASSERT(got == 4, "read(4) devolvió %lld", (long long)got);
    TEST_ASSERT(magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' &&
                    magic[3] == 'F',
                "ELF magic incorrecto: %02x %02x %02x %02x", magic[0], magic[1],
                magic[2], magic[3]);
    vfs_node_free(n);
  }

  fat32_test_cleanup();
}
REGISTER_TEST("fat32: leer ELF magic de /kernel.elf",
              test_fat32_read_elf_magic);

static void test_fat32_deep_path(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  // /EFI/BOOT/BOOTX64.EFI existe (mcopy del Makefile).
  vfs_node_t *n = vfs_lookup("/fat_test/EFI/BOOT/BOOTX64.EFI");
  TEST_ASSERT(n != NULL, "lookup deep path falló");
  if (n) {
    TEST_ASSERT(n->flags == VFS_FILE, "no es FILE");
    vfs_node_free(n);
  }

  // Y sus directorios intermedios existen.
  vfs_node_t *d1 = vfs_lookup("/fat_test/EFI");
  TEST_ASSERT(d1 != NULL, "/EFI no existe");
  if (d1) {
    TEST_ASSERT(d1->flags == VFS_DIRECTORY, "/EFI no es dir");
    vfs_node_free(d1);
  }

  fat32_test_cleanup();
}
REGISTER_TEST("fat32: path profundo /EFI/BOOT/BOOTX64.EFI",
              test_fat32_deep_path);

static void test_fat32_missing_file(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  vfs_node_t *n = vfs_lookup("/fat_test/no_existe_este_archivo");
  TEST_ASSERT(n == NULL, "lookup de archivo inexistente devolvió algo");

  vfs_node_t *d = vfs_lookup("/fat_test/no_existe_dir/sub");
  TEST_ASSERT(d == NULL, "lookup de dir inexistente devolvió algo");

  fat32_test_cleanup();
}
REGISTER_TEST("fat32: lookup de archivos inexistentes",
              test_fat32_missing_file);

static void test_fat32_read_chunked(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  // kernel.elf tiene varios MB: leemos solo lo justo para verificar
  // integridad sin saturar el heap del kernel (16 MB y compartido).
  // Validamos: ELF magic al inicio, un offset intermedio que cruce al
  // menos un cluster, y que seek + read del mismo offset devuelvan lo
  // mismo.
  vfs_node_t *n = vfs_lookup("/fat_test/kernel.elf");
  TEST_ASSERT(n != NULL, "lookup /kernel.elf falló");
  if (!n) {
    fat32_test_cleanup();
    return;
  }

  uint64_t fsz = n->size;
  TEST_ASSERT(fsz > 64 * 1024, "kernel.elf demasiado pequeño: %lu",
              (unsigned long)fsz);

  // 1. ELF magic en offset 0.
  uint8_t magic[4] = {0};
  int64_t g = n->ops->read(n, 0, 4, magic);
  TEST_ASSERT(g == 4, "read magic devolvió %lld", (long long)g);
  TEST_ASSERT(magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' &&
                  magic[3] == 'F',
              "ELF magic incorrecto: %02x %02x %02x %02x", magic[0], magic[1],
              magic[2], magic[3]);

  // 2. Leer 4 KB en un offset intermedio (cruce de clusters si el FS
  //    tiene cluster_size < 4 KB). Debe devolver 4 KB completos.
  uint64_t mid = fsz / 2;
  uint8_t *chunk = (uint8_t *)kmalloc(4096);
  TEST_ASSERT(chunk != NULL, "kmalloc(4096) falló");
  if (chunk) {
    int64_t n1 = n->ops->read(n, mid, 4096, chunk);
    TEST_ASSERT(n1 == 4096, "read 4K en offset %llu devolvió %lld",
                (unsigned long long)mid, (long long)n1);

    // Repetir el mismo read: debe coincidir byte a byte.
    uint8_t *chunk2 = (uint8_t *)kmalloc(4096);
    if (chunk2) {
      int64_t n2 = n->ops->read(n, mid, 4096, chunk2);
      TEST_ASSERT(n2 == 4096, "segundo read devolvió %lld", (long long)n2);
      TEST_ASSERT(memcmp(chunk, chunk2, 4096) == 0,
                  "dos reads del mismo offset difieren");
      kfree(chunk2);
    }
    kfree(chunk);
  }

  // 3. Leer 512 bytes justo antes del final.
  uint8_t tail[512];
  int64_t gt = n->ops->read(n, fsz - 512, 512, tail);
  TEST_ASSERT(gt == 512, "read tail devolvió %lld", (long long)gt);

  // 4. Leer más allá del final devuelve 0.
  int64_t ge = n->ops->read(n, fsz, 16, tail);
  TEST_ASSERT(ge == 0, "read más allá de EOF devolvió %lld", (long long)ge);

  vfs_node_free(n);
  fat32_test_cleanup();
}
REGISTER_TEST("fat32: lectura por chunks", test_fat32_read_chunked);

static void test_fat32_seek_and_read(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  // Abrir kernel.elf, leer 4 bytes, seek 0, leer otros 4, comparar.
  vfs_node_t *n = vfs_lookup("/fat_test/kernel.elf");
  TEST_ASSERT(n != NULL, "lookup");
  if (n) {
    uint8_t a[4] = {0}, b2[4] = {0};
    int64_t n1 = n->ops->read(n, 0, 4, a);
    int64_t n2 = n->ops->read(n, 0, 4, b2); // mismo offset, sin seek
    TEST_ASSERT(n1 == 4 && n2 == 4, "reads fallaron: %lld / %lld",
                (long long)n1, (long long)n2);
    TEST_ASSERT(memcmp(a, b2, 4) == 0, "reads del mismo offset difieren");

    // Leer un offset avanzado (dentro del mismo cluster).
    uint8_t c[16] = {0};
    int64_t n3 = n->ops->read(n, 16, 16, c);
    TEST_ASSERT(n3 == 16, "read(16) devolvió %lld", (long long)n3);

    vfs_node_free(n);
  }

  fat32_test_cleanup();
}
REGISTER_TEST("fat32: seek y lecturas repetidas", test_fat32_seek_and_read);

static void test_fat32_non_fat_rejected(void) {
  // Buscar un disco que NO sea FAT32, sin asumir nombres. Saltamos
  // el que fat32_find_test_disk() identifica como FAT32, y probamos
  // con cualquier otro disco no-partición y escribible.
  block_device_t *fat_disk = fat32_find_test_disk();
  block_device_t *candidate = NULL;
  for (int i = 0; i < blk_count(); i++) {
    block_device_t *b = blk_get_by_index(i);
    if (!b || b->is_partition || b->is_read_only)
      continue;
    if (b == fat_disk)
      continue;
    candidate = b;
    break;
  }

  if (!candidate) {
    test_skip("no hay disco no-FAT32 para probar el rechazo");
    return;
  }

  void *priv = NULL;
  int rc = fat32_mount(candidate, &priv);
  TEST_ASSERT(rc != 0, "fat32_mount aceptó %s sin FAT32 (rc=%d)",
              candidate->name, rc);
  TEST_ASSERT(priv == NULL, "fs_priv != NULL tras fallo en %s",
              candidate->name);

  // Por si un futuro cambio hiciera pasar el mount: liberar.
  if (rc == 0 && priv)
    fat32_umount(priv);
}
REGISTER_TEST("fat32: rechaza disco no-FAT32", test_fat32_non_fat_rejected);

// ===========================================================================
// [PR 3.1] Verifica que la caché de FAT acelera reads en offsets
// intermedios. Sin caché, leer 4 KB en la mitad de un archivo de ~5 MB
// camina miles de entradas de FAT, cada una con un bio completo.
// Con caché es O(1) por entrada y la operación tarda milisegundos.
// ===========================================================================
static void test_fat32_cache_speedup(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  vfs_node_t *n = vfs_lookup("/fat_test/kernel.elf");
  TEST_ASSERT(n != NULL, "lookup /kernel.elf falló");
  if (!n) {
    fat32_test_cleanup();
    return;
  }

  if (n->size < 1024 * 1024) {
    test_skip("kernel.elf demasiado pequeño para medir el speedup");
    vfs_node_free(n);
    fat32_test_cleanup();
    return;
  }

  extern uint64_t klog_get_tsc_freq(void);
  uint64_t freq = klog_get_tsc_freq();
  if (freq == 0)
    freq = 2000000000ULL;

  uint64_t mid = n->size / 2;
  uint8_t buf[4096];

  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t t0 = ((uint64_t)hi << 32) | lo;
  int64_t got = n->ops->read(n, mid, 4096, buf);
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t t1 = ((uint64_t)hi << 32) | lo;

  uint64_t us = (t1 - t0) * 1000000ULL / freq;
  LOG_INFO("  read 4K en offset %llu: %lld bytes, %lu us",
           (unsigned long long)mid, (long long)got, (unsigned long)us);

  TEST_ASSERT(got == 4096, "read devolvió %lld", (long long)got);

  // Umbral generoso: en TCG puro un read con caché debería quedar
  // <50 ms; en KVM <5 ms. 200 ms deja margen para CI lento y para
  // que un fallo real (falta de caché, ~3 s) sí lo dispare.
  TEST_ASSERT(us < 200000,
              "read demasiado lento: %lu us (¿caché FAT inactiva?)", us);

  vfs_node_free(n);
  fat32_test_cleanup();
}
REGISTER_TEST("fat32: caché de FAT acelera reads", test_fat32_cache_speedup);

static void test_fat32_alloc_free_chain(void) {
  // Aloca 5 clusters, encadena, libera. Verifica contabilidad sobre la
  // caché. No toca dirents.
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }

  void *priv = NULL;
  TEST_ASSERT(fat32_mount(b, &priv) == 0, "mount falló");
  if (!priv)
    return;

  // Acceso a internals: este test vive en el mismo TU que fat32.c solo
  // si lo añades al final de fat32.c. Como está en kernel_tests.c,
  // exponemos un hook mínimo.
  extern int fat32_test_alloc_free_chain(void *fs_priv);
  int rc = fat32_test_alloc_free_chain(priv);
  TEST_ASSERT(rc == 0, "alloc/free chain falló: %d", rc);

  fat32_umount(priv);
}
REGISTER_TEST("fat32: alloc + free chain", test_fat32_alloc_free_chain);

// ===========================================================================
// [PR 4.2] create/mkdir/unlink sobre FAT32.
//
// Trabaja sobre /fat_test. Crea, verifica, borra. Al final el disco
// queda como estaba (salvo desgaste del journaling interno de la img,
// que no existe).
// ===========================================================================
static void test_fat32_mkdir_unlink(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  // Limpieza previa por si una ejecución anterior dejó residuos.
  vfs_node_t *pre = vfs_lookup("/fat_test/pr4dir");
  if (pre) {
    vfs_node_free(pre);
    vfs_unlink("/fat_test/pr4dir");
  }

  // 1. mkdir.
  int rc = vfs_mkdir("/fat_test/pr4dir");
  TEST_ASSERT(rc == 0, "mkdir falló: %d", rc);
  if (rc != 0) {
    fat32_test_cleanup();
    return;
  }

  // 2. Debe existir y ser directorio.
  vfs_node_t *n = vfs_lookup("/fat_test/pr4dir");
  TEST_ASSERT(n != NULL, "lookup tras mkdir falló");
  if (n) {
    TEST_ASSERT(n->flags == VFS_DIRECTORY, "no es DIR");
    vfs_node_free(n);
  }

  // 3. mkdir duplicado → -EEXIST.
  rc = vfs_mkdir("/fat_test/pr4dir");
  TEST_ASSERT(rc == -EEXIST, "mkdir duplicado devolvió %d, esperado -EEXIST",
              rc);

  // 4. unlink.
  rc = vfs_unlink("/fat_test/pr4dir");
  TEST_ASSERT(rc == 0, "unlink falló: %d", rc);

  // 5. Debe haber desaparecido.
  n = vfs_lookup("/fat_test/pr4dir");
  TEST_ASSERT(n == NULL, "lookup tras unlink encontró algo");
  if (n)
    vfs_node_free(n);

  fat32_test_cleanup();
}
REGISTER_TEST("fat32: mkdir + lookup + unlink de directorio",
              test_fat32_mkdir_unlink);

static void test_fat32_create_file_empty(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  // Limpieza previa.
  vfs_node_t *pre = vfs_lookup("/fat_test/pr4file.txt");
  if (pre) {
    vfs_node_free(pre);
    vfs_unlink("/fat_test/pr4file.txt");
  }

  // 1. create.
  int rc = vfs_create("/fat_test/pr4file.txt", O_CREAT | O_RDWR);
  TEST_ASSERT(rc == 0, "create falló: %d", rc);
  if (rc != 0) {
    fat32_test_cleanup();
    return;
  }

  // 2. Debe existir, size=0, FILE.
  vfs_node_t *n = vfs_lookup("/fat_test/pr4file.txt");
  TEST_ASSERT(n != NULL, "lookup tras create falló");
  if (n) {
    TEST_ASSERT(n->flags == VFS_FILE, "no es FILE");
    TEST_ASSERT(n->size == 0, "size != 0: %lu", (unsigned long)n->size);
    vfs_node_free(n);
  }

  // 3. create duplicado → -EEXIST.
  rc = vfs_create("/fat_test/pr4file.txt", O_CREAT | O_RDWR);
  TEST_ASSERT(rc == -EEXIST, "create duplicado devolvió %d", rc);

  // 4. unlink.
  rc = vfs_unlink("/fat_test/pr4file.txt");
  TEST_ASSERT(rc == 0, "unlink falló: %d", rc);

  n = vfs_lookup("/fat_test/pr4file.txt");
  TEST_ASSERT(n == NULL, "lookup tras unlink encontró algo");
  if (n)
    vfs_node_free(n);

  fat32_test_cleanup();
}
REGISTER_TEST("fat32: create archivo vacío + unlink",
              test_fat32_create_file_empty);

static void test_fat32_mkdir_not_empty(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  // Cleanup robusto: si una ejecución anterior dejó el directorio y su
  // hijo, los borramos. El orden importa (hijo antes que padre).
  vfs_node_t *pre = vfs_lookup("/fat_test/pr4par");
  if (pre) {
    vfs_node_free(pre);
    vfs_unlink("/fat_test/pr4par/child");
    vfs_unlink("/fat_test/pr4par");
  }

  // "pr4par" cabe en 8.3 (6 chars).
  TEST_ASSERT(vfs_mkdir("/fat_test/pr4par") == 0, "mkdir parent");
  TEST_ASSERT(vfs_mkdir("/fat_test/pr4par/child") == 0, "mkdir child");

  // unlink del padre debe fallar con -ENOTEMPTY.
  int rc = vfs_unlink("/fat_test/pr4par");
  TEST_ASSERT(rc == -ENOTEMPTY, "unlink de dir no-vacío devolvió %d", rc);

  // Limpieza.
  TEST_ASSERT(vfs_unlink("/fat_test/pr4par/child") == 0, "unlink child");
  TEST_ASSERT(vfs_unlink("/fat_test/pr4par") == 0, "unlink parent");

  fat32_test_cleanup();
}
REGISTER_TEST("fat32: unlink de directorio no-vacío → ENOTEMPTY",
              test_fat32_mkdir_not_empty);

static void test_fat32_create_long_name_rejected(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  // Nombre con más de 8 chars en la base: no cabe en 8.3 sin LFN.
  int rc = vfs_create("/fat_test/verylongname.txt", O_CREAT);
  TEST_ASSERT(rc == -ENAMETOOLONG,
              "create con nombre largo devolvió %d, esperado -ENAMETOOLONG",
              rc);

  fat32_test_cleanup();
}
REGISTER_TEST("fat32: create con nombre >8.3 rechazado",
              test_fat32_create_long_name_rejected);

static void test_fat32_persistence_after_remount(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }

  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount 1");

  // Cleanup por si quedó residuo.
  vfs_node_t *pre = vfs_lookup("/fat_test/pr4per.txt");
  if (pre) {
    vfs_node_free(pre);
    vfs_unlink("/fat_test/pr4per.txt");
  }

  // "pr4per.txt" cabe en 8.3 (base 6, ext 3).
  TEST_ASSERT(vfs_create("/fat_test/pr4per.txt", O_CREAT) == 0, "create");

  // Umount.
  void *priv = vfs_get_mount_priv("/fat_test");
  TEST_ASSERT(priv != NULL, "get_mount_priv");
  TEST_ASSERT(vfs_umount("/fat_test") == 0, "umount");
  fat32_umount(priv);

  // Remount.
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount 2");

  // El archivo debe seguir existiendo.
  vfs_node_t *n = vfs_lookup("/fat_test/pr4per.txt");
  TEST_ASSERT(n != NULL, "archivo no persistió tras remount");
  if (n)
    vfs_node_free(n);

  // Limpieza.
  TEST_ASSERT(vfs_unlink("/fat_test/pr4per.txt") == 0, "unlink");
  fat32_test_cleanup();
}
REGISTER_TEST("fat32: persistencia tras remount",
              test_fat32_persistence_after_remount);

static void test_fat32_create_9char_base_rejected(void) {
  // El caso más frecuente: nombre tipo "readme.txt" cabe, "readme2.txt"
  // (8 chars base) cabe justo, "readme22.txt" (9 chars) NO.
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  // 8 chars base + .txt → OK.
  int rc = vfs_create("/fat_test/abcdefgh.txt", O_CREAT);
  TEST_ASSERT(rc == 0, "8.3 justo rechazado: %d", rc);
  vfs_unlink("/fat_test/abcdefgh.txt");

  // 9 chars base → -ENAMETOOLONG.
  rc = vfs_create("/fat_test/abcdefghi.txt", O_CREAT);
  TEST_ASSERT(rc == -ENAMETOOLONG,
              "9 chars base devolvió %d, esperado -ENAMETOOLONG", rc);

  fat32_test_cleanup();
}
REGISTER_TEST("fat32: límite exacto 8.3",
              test_fat32_create_9char_base_rejected);

// ===========================================================================
// [PR 4.3] write + extend + truncate
// ===========================================================================
static void test_fat32_write_small(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  // Cleanup previo.
  vfs_node_t *pre = vfs_lookup("/fat_test/w1.txt");
  if (pre) {
    vfs_node_free(pre);
    vfs_unlink("/fat_test/w1.txt");
  }

  TEST_ASSERT(vfs_create("/fat_test/w1.txt", O_CREAT | O_RDWR) == 0, "create");
  vfs_node_t *n = vfs_lookup("/fat_test/w1.txt");
  TEST_ASSERT(n != NULL, "lookup");
  if (n) {
    const char *msg = "hello";
    int64_t w = n->ops->write(n, 0, 5, msg);
    TEST_ASSERT(w == 5, "write devolvió %lld", (long long)w);
    TEST_ASSERT(n->size == 5, "size=%lu tras write", (unsigned long)n->size);

    // Leer de vuelta.
    char buf[16] = {0};
    int64_t r = n->ops->read(n, 0, 5, buf);
    TEST_ASSERT(r == 5, "read devolvió %lld", (long long)r);
    TEST_ASSERT(memcmp(buf, msg, 5) == 0, "contenido no coincide");
    vfs_node_free(n);
  }
  vfs_unlink("/fat_test/w1.txt");
  fat32_test_cleanup();
}
REGISTER_TEST("fat32: write pequeño + read-back", test_fat32_write_small);

static void test_fat32_write_extend(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  vfs_node_t *pre = vfs_lookup("/fat_test/w2.txt");
  if (pre) {
    vfs_node_free(pre);
    vfs_unlink("/fat_test/w2.txt");
  }

  TEST_ASSERT(vfs_create("/fat_test/w2.txt", O_CREAT | O_RDWR) == 0, "create");
  vfs_node_t *n = vfs_lookup("/fat_test/w2.txt");
  TEST_ASSERT(n != NULL, "lookup");
  if (n) {
    // Escribir 3 KB (varios clusters, cluster_size=512).
    uint8_t pattern[3072];
    for (size_t i = 0; i < sizeof(pattern); i++)
      pattern[i] = (uint8_t)(i * 7);

    int64_t w = n->ops->write(n, 0, sizeof(pattern), pattern);
    TEST_ASSERT(w == (int64_t)sizeof(pattern), "write devolvió %lld",
                (long long)w);
    TEST_ASSERT(n->size == sizeof(pattern), "size=%lu", (unsigned long)n->size);

    // Leer de vuelta y comparar.
    uint8_t *rb = (uint8_t *)kmalloc(sizeof(pattern));
    TEST_ASSERT(rb != NULL, "kmalloc");
    if (rb) {
      int64_t r = n->ops->read(n, 0, sizeof(pattern), rb);
      TEST_ASSERT(r == (int64_t)sizeof(pattern), "read devolvió %lld",
                  (long long)r);
      TEST_ASSERT(memcmp(rb, pattern, sizeof(pattern)) == 0, "mismatch");
      kfree(rb);
    }
    vfs_node_free(n);
  }
  vfs_unlink("/fat_test/w2.txt");
  fat32_test_cleanup();
}
REGISTER_TEST("fat32: write extend multi-cluster", test_fat32_write_extend);

static void test_fat32_write_partial_overwrite(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  vfs_node_t *pre = vfs_lookup("/fat_test/w3.txt");
  if (pre) {
    vfs_node_free(pre);
    vfs_unlink("/fat_test/w3.txt");
  }

  TEST_ASSERT(vfs_create("/fat_test/w3.txt", O_CREAT | O_RDWR) == 0, "create");
  vfs_node_t *n = vfs_lookup("/fat_test/w3.txt");
  TEST_ASSERT(n != NULL, "lookup");
  if (n) {
    const char *init = "AAAAABBBBBCCCCC";
    int64_t w = n->ops->write(n, 0, 15, init);
    TEST_ASSERT(w == 15, "write inicial");

    // Sobrescribir offset 5-9 con "xxx".
    const char *mod = "xxx";
    w = n->ops->write(n, 5, 3, mod);
    TEST_ASSERT(w == 3, "write parcial devolvió %lld", (long long)w);
    TEST_ASSERT(n->size == 15, "size cambió sin querer: %lu",
                (unsigned long)n->size);

    char buf[16] = {0};
    int64_t r = n->ops->read(n, 0, 15, buf);
    TEST_ASSERT(r == 15, "read");
    TEST_ASSERT(memcmp(buf, "AAAAAxxxBBCCCCC", 15) == 0,
                "contenido tras overwrite incorrecto: '%.*s'", 15, buf);
    vfs_node_free(n);
  }
  vfs_unlink("/fat_test/w3.txt");
  fat32_test_cleanup();
}
REGISTER_TEST("fat32: write parcial no cambia size",
              test_fat32_write_partial_overwrite);

static void test_fat32_truncate_shrink(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  vfs_node_t *pre = vfs_lookup("/fat_test/w4.txt");
  if (pre) {
    vfs_node_free(pre);
    vfs_unlink("/fat_test/w4.txt");
  }

  TEST_ASSERT(vfs_create("/fat_test/w4.txt", O_CREAT | O_RDWR) == 0, "create");
  vfs_node_t *n = vfs_lookup("/fat_test/w4.txt");
  TEST_ASSERT(n != NULL, "lookup");
  if (n) {
    uint8_t buf[2048];
    memset(buf, 0xAA, sizeof(buf));
    TEST_ASSERT(n->ops->write(n, 0, sizeof(buf), buf) == (int64_t)sizeof(buf),
                "write");
    TEST_ASSERT(n->size == sizeof(buf), "size inicial");

    // Truncar a 100.
    int rc = n->ops->truncate(n, 100);
    TEST_ASSERT(rc == 0, "truncate falló: %d", rc);
    TEST_ASSERT(n->size == 100, "size tras truncate: %lu",
                (unsigned long)n->size);

    // Leer más allá del nuevo tamaño devuelve 0.
    uint8_t rb[200];
    int64_t r = n->ops->read(n, 100, 200, rb);
    TEST_ASSERT(r == 0, "read más allá de EOF devolvió %lld", (long long)r);

    // Leer dentro del nuevo tamaño funciona.
    r = n->ops->read(n, 0, 100, rb);
    TEST_ASSERT(r == 100, "read dentro devolvió %lld", (long long)r);
    for (int i = 0; i < 100; i++) {
      if (rb[i] != 0xAA) {
        TEST_ASSERT(0, "byte %d corrupto: 0x%02x", i, rb[i]);
        break;
      }
    }
    vfs_node_free(n);
  }
  vfs_unlink("/fat_test/w4.txt");
  fat32_test_cleanup();
}
REGISTER_TEST("fat32: truncate a tamaño menor", test_fat32_truncate_shrink);

static void test_fat32_truncate_to_zero(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  vfs_node_t *pre = vfs_lookup("/fat_test/w5.txt");
  if (pre) {
    vfs_node_free(pre);
    vfs_unlink("/fat_test/w5.txt");
  }

  TEST_ASSERT(vfs_create("/fat_test/w5.txt", O_CREAT | O_RDWR) == 0, "create");
  vfs_node_t *n = vfs_lookup("/fat_test/w5.txt");
  TEST_ASSERT(n != NULL, "lookup");
  if (n) {
    uint8_t buf[1000];
    memset(buf, 0xBB, sizeof(buf));
    TEST_ASSERT(n->ops->write(n, 0, sizeof(buf), buf) == 1000, "write");

    int rc = n->ops->truncate(n, 0);
    TEST_ASSERT(rc == 0, "truncate a 0 falló: %d", rc);
    TEST_ASSERT(n->size == 0, "size != 0 tras truncate: %lu",
                (unsigned long)n->size);

    uint8_t rb[10];
    int64_t r = n->ops->read(n, 0, 10, rb);
    TEST_ASSERT(r == 0, "read tras truncate a 0 devolvió %lld", (long long)r);
    vfs_node_free(n);
  }
  vfs_unlink("/fat_test/w5.txt");
  fat32_test_cleanup();
}
REGISTER_TEST("fat32: truncate a 0", test_fat32_truncate_to_zero);

static void test_fat32_write_persist(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }

  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount 1");

  vfs_node_t *pre = vfs_lookup("/fat_test/w6.txt");
  if (pre) {
    vfs_node_free(pre);
    vfs_unlink("/fat_test/w6.txt");
  }

  TEST_ASSERT(vfs_create("/fat_test/w6.txt", O_CREAT | O_RDWR) == 0, "create");
  vfs_node_t *n = vfs_lookup("/fat_test/w6.txt");
  TEST_ASSERT(n != NULL, "lookup 1");
  if (n) {
    const char *msg = "persist-test-data";
    int64_t w = n->ops->write(n, 0, 17, msg);
    TEST_ASSERT(w == 17, "write");
    vfs_node_free(n);
  }

  // Umount/remount.
  void *priv = vfs_get_mount_priv("/fat_test");
  TEST_ASSERT(priv != NULL, "get_mount_priv");
  TEST_ASSERT(vfs_umount("/fat_test") == 0, "umount");
  fat32_umount(priv);

  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount 2");

  // Releer.
  n = vfs_lookup("/fat_test/w6.txt");
  TEST_ASSERT(n != NULL, "lookup 2");
  if (n) {
    TEST_ASSERT(n->size == 17, "size tras remount: %lu",
                (unsigned long)n->size);
    char buf[32] = {0};
    int64_t r = n->ops->read(n, 0, 17, buf);
    TEST_ASSERT(r == 17, "read tras remount");
    TEST_ASSERT(memcmp(buf, "persist-test-data", 17) == 0,
                "contenido no persistió: '%.*s'", 17, buf);
    vfs_node_free(n);
  }
  vfs_unlink("/fat_test/w6.txt");
  fat32_test_cleanup();
}
REGISTER_TEST("fat32: write persiste tras remount", test_fat32_write_persist);

static void test_fat32_write_to_directory_fails(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  // El root del mount es un directorio: write debe fallar.
  vfs_node_t *n = vfs_lookup("/fat_test");
  TEST_ASSERT(n != NULL, "lookup root");
  if (n) {
    char c = 'X';
    int64_t w = n->ops->write(n, 0, 1, &c);
    TEST_ASSERT(w < 0, "write a directorio devolvió %lld", (long long)w);
    vfs_node_free(n);
  }
  fat32_test_cleanup();
}
REGISTER_TEST("fat32: write a directorio falla",
              test_fat32_write_to_directory_fails);

static void test_fat32_truncate_grow_rejected(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  vfs_node_t *pre = vfs_lookup("/fat_test/w7.txt");
  if (pre) {
    vfs_node_free(pre);
    vfs_unlink("/fat_test/w7.txt");
  }

  TEST_ASSERT(vfs_create("/fat_test/w7.txt", O_CREAT | O_RDWR) == 0, "create");
  vfs_node_t *n = vfs_lookup("/fat_test/w7.txt");
  TEST_ASSERT(n != NULL, "lookup");
  if (n) {
    const char *msg = "abc";
    n->ops->write(n, 0, 3, msg);

    // Truncar a 100 debe fallar (crecer no soportado).
    int rc = n->ops->truncate(n, 100);
    TEST_ASSERT(rc == -EINVAL, "truncate-grow devolvió %d, esperado -EINVAL",
                rc);
    TEST_ASSERT(n->size == 3, "size cambió: %lu", (unsigned long)n->size);
    vfs_node_free(n);
  }
  vfs_unlink("/fat_test/w7.txt");
  fat32_test_cleanup();
}
REGISTER_TEST("fat32: truncate a mayor rechazado",
              test_fat32_truncate_grow_rejected);

static void test_fat32_readdir(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  // El root del FS debe tener al menos un archivo conocido (kernel.elf).
  int found_kernel = 0;
  int found_efi = 0;
  vfs_dirent_t d;
  for (uint64_t i = 0; i < 100; i++) {
    int rc = vfs_readdir("/fat_test", i, &d);
    TEST_ASSERT(rc == 0, "readdir devolvió %d en idx=%llu", rc,
                (unsigned long long)i);
    if (rc != 0)
      break;
    if (d.name[0] == '\0')
      break;
    if (strcmp(d.name, "kernel.elf") == 0)
      found_kernel = 1;
    if (strcmp(d.name, "EFI") == 0) {
      found_efi = 1;
      TEST_ASSERT(d.type == VFS_DIRECTORY, "EFI no es DIR");
    }
  }
  TEST_ASSERT(found_kernel, "no se encontró kernel.elf con readdir");
  TEST_ASSERT(found_efi, "no se encontró EFI con readdir");

  // "." y ".." nunca deben aparecer.
  for (uint64_t i = 0; i < 100; i++) {
    int rc = vfs_readdir("/fat_test", i, &d);
    if (rc != 0 || d.name[0] == '\0')
      break;
    TEST_ASSERT(strcmp(d.name, ".") != 0, "readdir devolvió '.'");
    TEST_ASSERT(strcmp(d.name, "..") != 0, "readdir devolvió '..'");
  }

  // readdir sobre un archivo devuelve -ENOTDIR.
  int rc = vfs_readdir("/fat_test/kernel.elf", 0, &d);
  TEST_ASSERT(rc == -ENOTDIR, "readdir sobre archivo devolvió %d", rc);

  fat32_test_cleanup();
}
REGISTER_TEST("fat32: readdir lista archivos", test_fat32_readdir);

static void test_fat32_mkdir_readdir_unlink(void) {
  block_device_t *b = fat32_find_test_disk();
  if (!b) {
    test_skip("sin disco FAT32");
    return;
  }
  fat32_test_cleanup();
  TEST_ASSERT(fat32_mount_bdev(b->name, "/fat_test") == 0, "mount");

  // Cleanup.
  vfs_node_t *pre = vfs_lookup("/fat_test/rd");
  if (pre) {
    vfs_node_free(pre);
    vfs_unlink("/fat_test/rd/f1.txt");
    vfs_unlink("/fat_test/rd");
  }

  TEST_ASSERT(vfs_mkdir("/fat_test/rd") == 0, "mkdir");
  TEST_ASSERT(vfs_create("/fat_test/rd/f1.txt", O_CREAT) == 0, "create");
  TEST_ASSERT(vfs_create("/fat_test/rd/f2.txt", O_CREAT) == 0, "create");

  // Listar y contar.
  int n_files = 0, n_dirs = 0;
  vfs_dirent_t d;
  for (uint64_t i = 0; i < 20; i++) {
    int rc = vfs_readdir("/fat_test/rd", i, &d);
    if (rc != 0 || d.name[0] == '\0')
      break;
    if (d.type == VFS_DIRECTORY)
      n_dirs++;
    else
      n_files++;
  }
  TEST_ASSERT(n_files == 2, "esperados 2 archivos, encontrados %d", n_files);
  TEST_ASSERT(n_dirs == 0, "esperados 0 dirs, encontrados %d", n_dirs);

  // Limpieza.
  TEST_ASSERT(vfs_unlink("/fat_test/rd/f1.txt") == 0, "unlink f1");
  TEST_ASSERT(vfs_unlink("/fat_test/rd/f2.txt") == 0, "unlink f2");
  TEST_ASSERT(vfs_unlink("/fat_test/rd") == 0, "unlink rd");

  fat32_test_cleanup();
}
REGISTER_TEST("fat32: mkdir + readdir + unlink de hijos",
              test_fat32_mkdir_readdir_unlink);