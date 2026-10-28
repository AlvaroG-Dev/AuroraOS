// kernel/kur_loader.c
//
// Loader de .kur (Fase 3.2). Añade sobre 3.1:
//   - Verificación SHA-256 del fichero (si KUR_FLAG_HASHED).
//   - Registro de exports del módulo (`.kur_ksymtab`) en la tabla dinámica.
//   - Resolución de imports: ksymtab estático -> dynsym dinámico.
//   - Refcounts entre módulos (n_dependents, n_imports_from).
//   - kur_loader_unload(): llama kur_exit, elimina exports, libera VA.
//
// [LinuxKPI L0.4] Límites ampliados para módulos grandes (drivers de Linux):
//   - Sin tope de secciones (antes 64 en total): tabla sec_off[] por índice,
//     con índices extendidos (e_shnum/e_shstrndx en shdr[0]).
//   - Se carga toda sección SHF_ALLOC (PROGBITS, NOBITS, INIT/FINI_ARRAY,
//     NOTE, unwind...). Se ejecuta .preinit_array/.init_array antes de
//     kur_init y .fini_array tras kur_exit.
//   - Relocaciones: + NONE, PC64, GOTPCREL, GOTPCRELX, REX_GOTPCRELX (con una
//     GOT propia al final de la imagen). Un símbolo de una sección no cargada
//     es error (antes se resolvía en silencio a la base del módulo).
//   - Símbolos `__start_SEC`/`__stop_SEC` de secciones con nombre de
//     identificador C, como hace ld en un enlace final, y
//     `_GLOBAL_OFFSET_TABLE_` (la GOT del módulo).
//   - Undefined débil (STB_WEAK) sin definición -> 0.
//   - Tabla dinámica de exports con hash, sin límite de entradas ni de
//     longitud de nombre. Un export no puede tapar un símbolo del kernel.

#include "kur_loader.h"
#include "elf.h"
#include "uaccess.h"
#include "heap.h"
#include "klog.h"
#include "ksymtab.h"
#include "paging.h"
#include "pmm.h"
#include "sha256.h"
#include "spinlock.h"
#include "string.h"
#include "vfs.h"     // [FASE 3.3] vfs_read_all, vfs_lookup, vfs_readdir_node
#include <stddef.h>
#include <stdint.h>

#define SHT_PROGBITS      1
#define SHT_SYMTAB        2
#define SHT_STRTAB        3
#define SHT_RELA          4
#define SHT_NOBITS        8
#define SHT_INIT_ARRAY    14
#define SHT_FINI_ARRAY    15
#define SHT_PREINIT_ARRAY 16
#define SHF_ALLOC    0x2u
#define SHN_UNDEF    0
#define SHN_LORESERVE 0xFF00
#define SHN_ABS      0xFFF1
#define SHN_COMMON   0xFFF2
#define SHN_XINDEX   0xFFFF
#define STB_WEAK     2

#define R_X86_64_NONE          0
#define R_X86_64_64            1
#define R_X86_64_PC32          2
#define R_X86_64_PLT32         4
#define R_X86_64_GOTPCREL      9
#define R_X86_64_32            10
#define R_X86_64_32S           11
#define R_X86_64_PC64          24
#define R_X86_64_GOTPCRELX     41
#define R_X86_64_REX_GOTPCRELX 42

#define KUR_NOT_LOADED  UINT64_MAX   // sec_off[] / sym_addrs[]: sin dirección
#define KUR_MAX_ALIGN   0x200000ULL  // la base de cada módulo va a 2 MiB
#define DYNSYM_BUCKETS  1024

struct kur_dynsym {
  const char *name;   // apunta a la imagen del módulo (vive lo que él)
  uint64_t addr;
  uint8_t  owner;     // slot de g_modules[] que lo exporta
  struct kur_dynsym *next;
};

static spinlock_t g_kur_lock = {.locked = 0};
static spinlock_t g_dynsym_lock = {.locked = 0};
static volatile int g_loader_ready = 0;
static volatile int g_load_in_progress = 0;
static uint64_t g_module_cursor = 0;
static struct kur_module g_modules[KUR_MAX_MODULES];
static struct kur_dynsym *g_dynsym[DYNSYM_BUCKETS];
static int g_dynsym_count = 0;

static inline uint64_t align_up(uint64_t v, uint64_t a) {
  return (v + a - 1) & ~(a - 1);
}

static size_t bounded_strlen(const char *s, size_t max) {
  size_t n = 0;
  while (n < max && s[n]) n++;
  return n;
}

static uint32_t dynsym_hash(const char *s) {
  uint32_t h = 2166136261u;  // FNV-1a
  while (*s) {
    h ^= (uint8_t)*s++;
    h *= 16777619u;
  }
  return h & (DYNSYM_BUCKETS - 1);
}

static uint64_t next_module_base(size_t size) {
  if (g_module_cursor == 0)
    g_module_cursor = KUR_MODULE_BASE;
  uint64_t base = g_module_cursor;
  g_module_cursor = align_up(base + size, 0x200000ULL);
  return base;
}

static int validate_manifest(const struct kur_manifest *m) {
  if (!m) return -EINVAL;
  if (m->magic != KUR_MAGIC) {
    LOG_ERR("[KUR] bad magic 0x%x (esperado 0x%x)", m->magic, KUR_MAGIC);
    return -ENOEXEC;
  }
  if (m->abi_version != KUR_ABI_VERSION) {
    LOG_ERR("[KUR] ABI mismatch: módulo v%u, kernel v%u", m->abi_version,
            KUR_ABI_VERSION);
    return -ENOEXEC;
  }
  if ((m->flags & ~KUR_FLAG_HASHED) != 0) {
    LOG_ERR("[KUR] flags=0x%x no soportados", m->flags);
    return -ENOEXEC;
  }
  if (m->name_len == 0 || m->name_len >= KUR_MAX_NAME) {
    LOG_ERR("[KUR] name_len inválido: %u", m->name_len);
    return -ENOEXEC;
  }
  if (m->name[m->name_len] != '\0') {
    LOG_ERR("[KUR] name no termina en NUL en name_len");
    return -ENOEXEC;
  }
  return 0;
}

// Vista validada de las tablas del ELF que usa el loader.
struct kur_elf {
  const uint8_t *data;
  size_t size;
  const Elf64_Shdr *shdrs;
  uint32_t shnum;
  uint32_t shstrndx;
};

// Nombre de la sección i (o "" si no es válido).
static const char *sec_name(const struct kur_elf *e, uint32_t i) {
  if (e->shstrndx == 0 || e->shstrndx >= e->shnum || i >= e->shnum)
    return "";
  const Elf64_Shdr *strh = &e->shdrs[e->shstrndx];
  if (strh->sh_type != SHT_STRTAB || e->shdrs[i].sh_name >= strh->sh_size)
    return "";
  const char *s = (const char *)e->data + strh->sh_offset + e->shdrs[i].sh_name;
  size_t rem = strh->sh_size - e->shdrs[i].sh_name;
  return bounded_strlen(s, rem) < rem ? s : "";
}

static const Elf64_Sym *sym_find(const struct kur_elf *e, const char *name) {
  for (uint32_t i = 0; i < e->shnum; i++) {
    if (e->shdrs[i].sh_type != SHT_SYMTAB) continue;
    if (e->shdrs[i].sh_link >= e->shnum) continue;
    const Elf64_Sym *syms = (const Elf64_Sym *)(e->data + e->shdrs[i].sh_offset);
    size_t n = e->shdrs[i].sh_size / sizeof(Elf64_Sym);
    const Elf64_Shdr *strhdr = &e->shdrs[e->shdrs[i].sh_link];
    const char *strs = (const char *)(e->data + strhdr->sh_offset);
    for (size_t j = 0; j < n; j++) {
      if (syms[j].st_name >= strhdr->sh_size) continue;
      if (strcmp(strs + syms[j].st_name, name) == 0)
        return &syms[j];
    }
  }
  return NULL;
}

// Comprueba que todas las secciones con contenido caen dentro del fichero.
static int check_sections_in_file(const struct kur_elf *e) {
  for (uint32_t i = 1; i < e->shnum; i++) {
    const Elf64_Shdr *s = &e->shdrs[i];
    if (s->sh_type == SHT_NOBITS || s->sh_size == 0) continue;
    if (s->sh_offset > e->size || s->sh_size > e->size - s->sh_offset) {
      LOG_ERR("[KUR] sección %u fuera del fichero", i);
      return -ENOEXEC;
    }
  }
  return 0;
}

static int verify_manifest_hash(const void *data, size_t size,
                                uint64_t hash_file_off,
                                const struct kur_manifest *mf) {
  if (!(mf->flags & KUR_FLAG_HASHED)) return 0;
  if (hash_file_off > size || size - hash_file_off < 32) {
    LOG_ERR("[KUR] hash offset fuera del fichero");
    return -ENOEXEC;
  }
  sha256_ctx_t ctx;
  sha256_init(&ctx);
  sha256_update(&ctx, (const uint8_t *)data, hash_file_off);
  static const uint8_t zeros[32] = {0};
  sha256_update(&ctx, zeros, 32);
  sha256_update(&ctx, (const uint8_t *)data + hash_file_off + 32,
                size - hash_file_off - 32);
  uint8_t digest[32];
  sha256_final(&ctx, digest);
  if (memcmp(digest, mf->hash, 32) != 0) {
    LOG_ERR("[KUR] hash SHA-256 mismatch: archivo corrupto o manipulado");
    return -ENOEXEC;
  }
  LOG_INFO("[KUR] hash SHA-256 OK");
  return 0;
}

// Búsqueda en la tabla dinámica de símbolos (exports de otros módulos).
// Si owner_out != NULL, escribe el slot del módulo que exporta el símbolo.
static uint64_t kur_dynsym_lookup(const char *name, int *owner_out) {
  uint64_t addr = 0;
  int owner = -1;
  unsigned long flags = spin_lock_irqsave(&g_dynsym_lock);
  for (struct kur_dynsym *d = g_dynsym[dynsym_hash(name)]; d; d = d->next) {
    if (strcmp(d->name, name) == 0) {
      addr = d->addr;
      owner = d->owner;
      break;
    }
  }
  spin_unlock_irqrestore(&g_dynsym_lock, flags);
  if (owner_out) *owner_out = owner;
  return addr;
}

// Quita de la tabla dinámica los exports del slot `owner`.
static int dynsym_remove_owner(int owner) {
  int removed = 0;
  unsigned long flags = spin_lock_irqsave(&g_dynsym_lock);
  for (int b = 0; b < DYNSYM_BUCKETS; b++) {
    struct kur_dynsym **pp = &g_dynsym[b];
    while (*pp) {
      struct kur_dynsym *d = *pp;
      if (d->owner == (uint8_t)owner) {
        *pp = d->next;
        kfree(d);
        removed++;
      } else {
        pp = &d->next;
      }
    }
  }
  g_dynsym_count -= removed;
  spin_unlock_irqrestore(&g_dynsym_lock, flags);
  return removed;
}

// Primera sección cuyo nombre es `name`, o 0.
static uint32_t find_section_by_name(const struct kur_elf *e, const char *name) {
  for (uint32_t i = 1; i < e->shnum; i++) {
    if (strcmp(sec_name(e, i), name) == 0)
      return i;
  }
  return 0;
}

// Registra los exports del módulo en la tabla dinámica.
// Se llama DESPUÉS de aplicar relocaciones y de kur_init OK.
static int register_module_exports(struct kur_module *mod,
                                   const struct kur_elf *e,
                                   const uint64_t *sec_off, uint64_t base) {
  uint32_t exp_idx = find_section_by_name(e, KUR_EXPORT_SECTION);
  if (exp_idx == 0)
    return 0;  // módulo sin exports: OK
  if (sec_off[exp_idx] == KUR_NOT_LOADED) {
    LOG_ERR("[KUR] %s presente pero no allocable", KUR_EXPORT_SECTION);
    return -ENOEXEC;
  }
  size_t n_entries = e->shdrs[exp_idx].sh_size / sizeof(struct kur_export_entry);
  const struct kur_export_entry *entries =
      (const struct kur_export_entry *)(base + sec_off[exp_idx]);

  for (size_t i = 0; i < n_entries; i++) {
    const char *name = entries[i].name;
    uint64_t addr = entries[i].addr;
    if (!name || bounded_strlen(name, 4096) == 0 ||
        bounded_strlen(name, 4096) == 4096) {
      LOG_ERR("[KUR] export[%zu]: nombre vacío o inválido", i);
      return -ENOEXEC;
    }
    if (ksymtab_lookup(name) != 0) {
      LOG_ERR("[KUR] export '%s' tapa un símbolo del kernel", name);
      return -EEXIST;
    }
    struct kur_dynsym *d = (struct kur_dynsym *)kmalloc(sizeof(*d));
    if (!d) return -ENOMEM;
    d->name = name;
    d->addr = addr;
    d->owner = (uint8_t)mod->index;
    uint32_t h = dynsym_hash(name);
    unsigned long flags = spin_lock_irqsave(&g_dynsym_lock);
    for (struct kur_dynsym *o = g_dynsym[h]; o; o = o->next) {
      if (strcmp(o->name, name) == 0) {
        spin_unlock_irqrestore(&g_dynsym_lock, flags);
        kfree(d);
        LOG_ERR("[KUR] export duplicado: '%s'", name);
        return -EEXIST;
      }
    }
    d->next = g_dynsym[h];
    g_dynsym[h] = d;
    g_dynsym_count++;
    spin_unlock_irqrestore(&g_dynsym_lock, flags);
    LOG_DEBUG("[KUR] export '%s' @ %p", name, (void *)addr);
  }
  if (n_entries)
    LOG_INFO("[KUR] '%s': %zu exports registrados", mod->name, n_entries);
  return 0;
}

static int is_got_reloc(uint32_t type) {
  return type == R_X86_64_GOTPCREL || type == R_X86_64_GOTPCRELX ||
         type == R_X86_64_REX_GOTPCRELX;
}

// Llama a las funciones de las secciones del tipo dado, en orden.
static void run_array_sections(const struct kur_elf *e, const uint64_t *sec_off,
                               uint64_t base, uint32_t type) {
  for (uint32_t i = 1; i < e->shnum; i++) {
    if (e->shdrs[i].sh_type != type || sec_off[i] == KUR_NOT_LOADED) continue;
    const uint64_t *fns = (const uint64_t *)(base + sec_off[i]);
    size_t n = e->shdrs[i].sh_size / sizeof(uint64_t);
    for (size_t k = 0; k < n; k++) {
      if (fns[k] && fns[k] != UINT64_MAX)
        ((void (*)(void))fns[k])();
    }
  }
}

// Resuelve `__start_SEC` / `__stop_SEC` contra las secciones cargadas.
static uint64_t start_stop_symbol(const struct kur_elf *e,
                                  const uint64_t *sec_off, uint64_t base,
                                  const char *nm) {
  int stop = 0;
  const char *sec;
  if (strncmp(nm, "__start_", 8) == 0) {
    sec = nm + 8;
  } else if (strncmp(nm, "__stop_", 7) == 0) {
    sec = nm + 7;
    stop = 1;
  } else {
    return 0;
  }
  uint32_t i = find_section_by_name(e, sec);
  if (i == 0 || sec_off[i] == KUR_NOT_LOADED) return 0;
  return base + sec_off[i] + (stop ? e->shdrs[i].sh_size : 0);
}

static int kur_load_internal(const void *data, size_t size, int slot,
                             struct kur_module *out) {
  if (!data || size < sizeof(Elf64_Ehdr) || size > KUR_MAX_SIZE)
    return -EINVAL;

  memset(out, 0, sizeof(*out));
  out->index = slot;

  const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;
  if (memcmp(ehdr->e_ident, ELFMAG, 4) != 0) {
    LOG_ERR("[KUR] no es ELF");
    return -ENOEXEC;
  }
  if (ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
      ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
      ehdr->e_ident[EI_VERSION] != EV_CURRENT ||
      ehdr->e_machine != EM_X86_64) {
    LOG_ERR("[KUR] ELF no es x86-64 LSB");
    return -ENOEXEC;
  }
  if (ehdr->e_type != 1u) {
    LOG_ERR("[KUR] e_type=%u, esperado ET_REL(1)", ehdr->e_type);
    return -ENOEXEC;
  }
  if (ehdr->e_shoff == 0 || ehdr->e_shoff > size ||
      ehdr->e_shentsize != sizeof(Elf64_Shdr) ||
      size - ehdr->e_shoff < sizeof(Elf64_Shdr)) {
    LOG_ERR("[KUR] section header table inválida");
    return -ENOEXEC;
  }

  struct kur_elf e;
  e.data = (const uint8_t *)data;
  e.size = size;
  e.shdrs = (const Elf64_Shdr *)(e.data + ehdr->e_shoff);
  // Índices extendidos: con >= SHN_LORESERVE secciones, e_shnum = 0 y el
  // número real va en shdr[0].sh_size (y e_shstrndx en shdr[0].sh_link).
  e.shnum = ehdr->e_shnum ? ehdr->e_shnum : (uint32_t)e.shdrs[0].sh_size;
  e.shstrndx = ehdr->e_shstrndx == SHN_XINDEX ? e.shdrs[0].sh_link
                                              : ehdr->e_shstrndx;
  if (e.shnum == 0 ||
      (uint64_t)e.shnum > (size - ehdr->e_shoff) / sizeof(Elf64_Shdr)) {
    LOG_ERR("[KUR] section headers fuera del fichero");
    return -ENOEXEC;
  }
  int rc = check_sections_in_file(&e);
  if (rc < 0) return rc;

  // -- 1. Manifest ---------------------------------------------------------
  const Elf64_Sym *mfs = sym_find(&e, KUR_SYM_MANIFEST);
  if (!mfs || mfs->st_shndx == SHN_UNDEF || mfs->st_shndx >= e.shnum) {
    LOG_ERR("[KUR] falta '%s' definido", KUR_SYM_MANIFEST);
    return -ENOEXEC;
  }
  const Elf64_Shdr *mfsh = &e.shdrs[mfs->st_shndx];
  if (mfsh->sh_offset > size ||
      mfsh->sh_size < sizeof(struct kur_manifest)) {
    LOG_ERR("[KUR] sección del manifest demasiado pequeña");
    return -ENOEXEC;
  }
  uint64_t manifest_off = mfsh->sh_offset + mfs->st_value;
  if (manifest_off + sizeof(struct kur_manifest) > size) {
    LOG_ERR("[KUR] manifest fuera del fichero");
    return -ENOEXEC;
  }
  const struct kur_manifest *mf =
      (const struct kur_manifest *)((const uint8_t *)data + manifest_off);

  rc = validate_manifest(mf);
  if (rc < 0) return rc;
  LOG_INFO("[KUR] manifest OK: name='%s' abi=%u flags=0x%x",
           mf->name, mf->abi_version, mf->flags);

  // -- 1b. Hash SHA-256 ----------------------------------------------------
  uint64_t hash_file_off = manifest_off + offsetof(struct kur_manifest, hash);
  rc = verify_manifest_hash(data, size, hash_file_off, mf);
  if (rc < 0) return rc;

  // -- 2. init/exit y tabla de símbolos -------------------------------------
  const Elf64_Sym *init_sym = sym_find(&e, KUR_SYM_INIT);
  if (!init_sym || init_sym->st_shndx == SHN_UNDEF ||
      init_sym->st_shndx >= e.shnum) {
    LOG_ERR("[KUR] falta '%s'", KUR_SYM_INIT);
    return -ENOEXEC;
  }
  const Elf64_Sym *exit_sym = sym_find(&e, KUR_SYM_EXIT);

  const Elf64_Shdr *symtab_hdr = NULL;
  for (uint32_t i = 0; i < e.shnum; i++) {
    if (e.shdrs[i].sh_type == SHT_SYMTAB) { symtab_hdr = &e.shdrs[i]; break; }
  }
  if (!symtab_hdr || symtab_hdr->sh_link >= e.shnum) {
    LOG_ERR("[KUR] sin .symtab");
    return -ENOEXEC;
  }
  size_t n_syms = symtab_hdr->sh_size / sizeof(Elf64_Sym);
  const Elf64_Sym *syms = (const Elf64_Sym *)(e.data + symtab_hdr->sh_offset);
  const Elf64_Shdr *strtab_hdr = &e.shdrs[symtab_hdr->sh_link];
  const char *strs = (const char *)(e.data + strtab_hdr->sh_offset);

  uint64_t *sec_off = (uint64_t *)kmalloc(e.shnum * sizeof(uint64_t));
  uint64_t *sym_addrs = (uint64_t *)kmalloc((n_syms + 1) * sizeof(uint64_t));
  uint32_t *got_slot = (uint32_t *)kcalloc(n_syms + 1, sizeof(uint32_t));
  if (!sec_off || !sym_addrs || !got_slot) {
    kfree(sec_off); kfree(sym_addrs); kfree(got_slot);
    return -ENOMEM;
  }
  uint64_t base = 0, npages = 0;
  rc = -ENOEXEC;

  // -- 3. Layout: toda sección SHF_ALLOC, y la GOT al final ----------------
  uint64_t total = 0;
  for (uint32_t i = 0; i < e.shnum; i++) {
    const Elf64_Shdr *s = &e.shdrs[i];
    sec_off[i] = KUR_NOT_LOADED;
    if (i == 0 || !(s->sh_flags & SHF_ALLOC)) continue;
    // Una sección vacía también tiene dirección (una relocación puede apuntar
    // a su símbolo de sección, p. ej. el final de un array vacío): ocupa 0 bytes.
    uint64_t align = s->sh_addralign ? s->sh_addralign : 1;
    if (align & (align - 1)) align = 16;
    if (align > KUR_MAX_ALIGN) {
      LOG_ERR("[KUR] sección %u: alineación %lu > 2 MiB", i,
              (unsigned long)align);
      goto out_free;
    }
    sec_off[i] = align_up(total, align);
    total = sec_off[i] + s->sh_size;
  }
  if (total == 0) {
    LOG_ERR("[KUR] sin secciones allocables");
    goto out_free;
  }
  uint32_t n_got = 0;
  for (uint32_t i = 0; i < e.shnum; i++) {
    const Elf64_Shdr *rh = &e.shdrs[i];
    if (rh->sh_type != SHT_RELA || rh->sh_info >= e.shnum ||
        sec_off[rh->sh_info] == KUR_NOT_LOADED)
      continue;
    const Elf64_Rela *relas = (const Elf64_Rela *)(e.data + rh->sh_offset);
    size_t n = rh->sh_size / sizeof(Elf64_Rela);
    for (size_t j = 0; j < n; j++) {
      uint32_t si = (uint32_t)(relas[j].r_info >> 32);
      if (is_got_reloc((uint32_t)relas[j].r_info) && si < n_syms &&
          got_slot[si] == 0)
        got_slot[si] = ++n_got;
    }
  }
  uint64_t got_off = align_up(total, 8);
  if (n_got) total = got_off + (uint64_t)n_got * 8;

  // -- 4. Alloc VA --------------------------------------------------------
  base = next_module_base(total);
  npages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
  if (vmm_alloc_pages(base, npages, PTE_WRITABLE | PTE_GLOBAL) != 0) {
    LOG_ERR("[KUR] vmm_alloc_pages(%lu págs) falló en %p",
            (unsigned long)npages, (void *)base);
    npages = 0;
    rc = -ENOMEM;
    goto out_free;
  }
  memset((void *)base, 0, total);
  LOG_INFO("[KUR] cargado en %p size=%lu (%u secciones, %u entradas GOT)",
           (void *)base, (unsigned long)total, e.shnum, n_got);

  // -- 5. Copiar contenido ------------------------------------------------
  for (uint32_t i = 1; i < e.shnum; i++) {
    if (sec_off[i] == KUR_NOT_LOADED || e.shdrs[i].sh_type == SHT_NOBITS)
      continue;
    memcpy((uint8_t *)base + sec_off[i], e.data + e.shdrs[i].sh_offset,
           e.shdrs[i].sh_size);
  }

  // -- 6. Pre-resolver símbolos ------------------------------------------
  sym_addrs[0] = 0;
  for (size_t i = 1; i < n_syms; i++) {
    const Elf64_Sym *s = &syms[i];
    if (s->st_name >= strtab_hdr->sh_size) {
      LOG_ERR("[KUR] sym[%zu]: nombre fuera de .strtab", i);
      goto fail;
    }
    const char *nm = strs + s->st_name;
    if (s->st_shndx == SHN_UNDEF) {
      if (s->st_name == 0) {
        LOG_ERR("[KUR] sym[%zu]: undefined sin nombre", i);
        goto fail;
      }
      // 0) __start_SEC / __stop_SEC del propio módulo, y la GOT propia
      // (_GLOBAL_OFFSET_TABLE_ lo referencia el ensamblador con GOTPCREL).
      uint64_t a = start_stop_symbol(&e, sec_off, base, nm);
      if (a == 0 && strcmp(nm, "_GLOBAL_OFFSET_TABLE_") == 0)
        a = base + got_off;
      // 1) ksymtab estático (kernel).
      if (a == 0)
        a = ksymtab_lookup(nm);
      int owner = -1;
      // 2) dynsym dinámico (otros módulos).
      if (a == 0)
        a = kur_dynsym_lookup(nm, &owner);
      if (a == 0 && (s->st_info >> 4) != STB_WEAK) {
        LOG_ERR("[KUR] símbolo sin resolver: '%s'", nm);
        goto fail;
      }
      sym_addrs[i] = a;
      if (owner >= 0 && owner != slot) {
        // [FIX 3.2] Marcar el exportador como "en uso" para que unload
        // rechace con -EBUSY mientras este módulo viva. Se deshace en
        // el fail: y en kur_loader_unload().
        if (owner >= KUR_MAX_MODULES || !g_modules[owner].in_use) {
          LOG_ERR("[KUR] dynsym apunta a slot %d inválido", owner);
          goto fail;
        }
        out->n_imports_from[owner]++;
        g_modules[owner].n_dependents++;
      }
    } else if (s->st_shndx == SHN_ABS) {
      sym_addrs[i] = s->st_value;
    } else if (s->st_shndx == SHN_COMMON) {
      LOG_ERR("[KUR] SHN_COMMON no soportado (usa -fno-common)");
      goto fail;
    } else if (s->st_shndx >= SHN_LORESERVE) {
      LOG_ERR("[KUR] sym '%s': st_shndx 0x%x no soportado", nm, s->st_shndx);
      goto fail;
    } else if (s->st_shndx >= e.shnum ||
               sec_off[s->st_shndx] == KUR_NOT_LOADED) {
      sym_addrs[i] = KUR_NOT_LOADED;  // p. ej. símbolo de sección de debug
    } else {
      sym_addrs[i] = base + sec_off[s->st_shndx] + s->st_value;
    }
  }

  // -- 7. Aplicar relocaciones SHT_RELA ----------------------------------
  for (uint32_t i = 0; i < e.shnum; i++) {
    const Elf64_Shdr *rh = &e.shdrs[i];
    if (rh->sh_type != SHT_RELA) continue;
    if (rh->sh_info >= e.shnum) {
      LOG_ERR("[KUR] rela sección %u: destino inválido", i);
      goto fail;
    }
    // Relocaciones de secciones no cargadas (debug): se ignoran.
    if (sec_off[rh->sh_info] == KUR_NOT_LOADED) continue;
    uint64_t target_off = sec_off[rh->sh_info];
    uint64_t target_size = e.shdrs[rh->sh_info].sh_size;
    const Elf64_Rela *relas = (const Elf64_Rela *)(e.data + rh->sh_offset);
    size_t n_relas = rh->sh_size / sizeof(Elf64_Rela);
    for (size_t j = 0; j < n_relas; j++) {
      const Elf64_Rela *r = &relas[j];
      uint32_t sym_idx = (uint32_t)(r->r_info >> 32);
      uint32_t type = (uint32_t)(r->r_info & 0xFFFFFFFFu);
      if (type == R_X86_64_NONE) continue;
      if (sym_idx >= n_syms) {
        LOG_ERR("[KUR] rela %zu: sym_idx fuera de rango", j);
        goto fail;
      }
      if (r->r_offset > target_size || target_size - r->r_offset < 4 ||
          ((type == R_X86_64_64 || type == R_X86_64_PC64) &&
           target_size - r->r_offset < 8)) {
        LOG_ERR("[KUR] rela %zu: offset 0x%lx fuera de la sección", j,
                (unsigned long)r->r_offset);
        goto fail;
      }
      uint64_t S = sym_addrs[sym_idx];
      if (S == KUR_NOT_LOADED) {
        LOG_ERR("[KUR] rela %zu: símbolo '%s' en sección no cargada", j,
                strs + syms[sym_idx].st_name);
        goto fail;
      }
      int64_t A = r->r_addend;
      uint64_t P = base + target_off + r->r_offset;
      switch (type) {
      case R_X86_64_64:
        *(uint64_t *)P = S + A;
        break;
      case R_X86_64_PC64:
        *(int64_t *)P = (int64_t)S + A - (int64_t)P;
        break;
      case R_X86_64_GOTPCREL:
      case R_X86_64_GOTPCRELX:
      case R_X86_64_REX_GOTPCRELX: {
        uint64_t G = base + got_off + (uint64_t)(got_slot[sym_idx] - 1) * 8;
        *(uint64_t *)G = S;
        S = G;
      }
        __attribute__((fallthrough)); // PC32 contra la entrada de la GOT
      case R_X86_64_PC32:
      case R_X86_64_PLT32: {
        int64_t v = (int64_t)S + A - (int64_t)P;
        if (v > INT32_MAX || v < INT32_MIN) {
          LOG_ERR("[KUR] PC32 fuera de rango: %lld", (long long)v);
          goto fail;
        }
        *(int32_t *)P = (int32_t)v;
        break;
      }
      case R_X86_64_32: {
        uint64_t v = S + A;
        if (v > 0xFFFFFFFFULL) {
          LOG_ERR("[KUR] R_X86_64_32 overflow: 0x%lx", (unsigned long)v);
          goto fail;
        }
        *(uint32_t *)P = (uint32_t)v;
        break;
      }
      case R_X86_64_32S: {
        int64_t v = (int64_t)S + A;
        if (v > INT32_MAX || v < INT32_MIN) {
          LOG_ERR("[KUR] R_X86_64_32S overflow: %lld", (long long)v);
          goto fail;
        }
        *(int32_t *)P = (int32_t)v;
        break;
      }
      default:
        LOG_ERR("[KUR] reloc tipo %u no soportado (rela %zu)", type, j);
        goto fail;
      }
    }
  }

  // -- 8. Constructores y kur_init ----------------------------------------
  run_array_sections(&e, sec_off, base, SHT_PREINIT_ARRAY);
  run_array_sections(&e, sec_off, base, SHT_INIT_ARRAY);
  if (sec_off[init_sym->st_shndx] == KUR_NOT_LOADED) {
    LOG_ERR("[KUR] %s en sección no cargada", KUR_SYM_INIT);
    goto fail;
  }
  void *init_addr =
      (void *)(base + sec_off[init_sym->st_shndx] + init_sym->st_value);
  LOG_INFO("[KUR] llamando %s en %p", KUR_SYM_INIT, init_addr);
  int (*init_fn)(void) = (int (*)(void))init_addr;
  int irc = init_fn();
  if (irc != 0) {
    LOG_ERR("[KUR] %s devolvió %d", KUR_SYM_INIT, irc);
    rc = (irc < 0) ? irc : -ENOEXEC;
    goto fail;
  }

  // -- 9. Rellenar handle ------------------------------------------------
  for (int k = 0; k < KUR_MAX_NAME && mf->name[k]; k++)
    out->name[k] = mf->name[k];
  out->name[KUR_MAX_NAME - 1] = '\0';
  out->base_va = base;
  out->base_size = total;
  out->init_fn = init_addr;
  if (exit_sym && exit_sym->st_shndx != SHN_UNDEF &&
      exit_sym->st_shndx < e.shnum &&
      sec_off[exit_sym->st_shndx] != KUR_NOT_LOADED)
    out->exit_fn =
        (void *)(base + sec_off[exit_sym->st_shndx] + exit_sym->st_value);
  for (uint32_t i = 1; i < e.shnum; i++) {
    if (e.shdrs[i].sh_type == SHT_FINI_ARRAY && sec_off[i] != KUR_NOT_LOADED) {
      out->fini_array = base + sec_off[i];
      out->fini_count = e.shdrs[i].sh_size / sizeof(uint64_t);
      break;
    }
  }

  // -- 10. Registrar exports (ya con módulo cargado y kur_init OK) -------
  rc = register_module_exports(out, &e, sec_off, base);
  if (rc < 0) {
    // Rollback: descargar y fallar.
    goto fail;
  }

  kfree(sec_off);
  kfree(sym_addrs);
  kfree(got_slot);
  return 0;

fail:
  // Rollback de imports (n_dependents en otros módulos).
  for (int b = 0; b < KUR_MAX_MODULES; b++) {
    if (out->n_imports_from[b] > 0) {
      if (g_modules[b].n_dependents >= out->n_imports_from[b])
        g_modules[b].n_dependents -= out->n_imports_from[b];
      else
        g_modules[b].n_dependents = 0;
      out->n_imports_from[b] = 0;
    }
  }
  // Rollback de exports (por si register_module_exports falló a medias).
  dynsym_remove_owner(out->index);
out_free:
  if (npages)
    vmm_free_pages(base, npages);
  kfree(sec_off);
  kfree(sym_addrs);
  kfree(got_slot);
  return rc ? rc : -ENOEXEC;
}

void kur_loader_init(void) {
  if (g_loader_ready) return;
  spin_init(&g_kur_lock);
  spin_init(&g_dynsym_lock);
  ksymtab_init();
  extern uint8_t __bss_end;
  if ((uint64_t)&__bss_end >= KUR_MODULE_BASE) {
    LOG_ERR("[KUR] KUR_MODULE_BASE=0x%lx colisiona con __bss_end=0x%lx",
            (unsigned long)KUR_MODULE_BASE, (unsigned long)&__bss_end);
  }
  LOG_INFO("[KUR] loader listo (ABI v%u, max %d módulos, max %u MiB)",
           KUR_ABI_VERSION, KUR_MAX_MODULES, KUR_MAX_SIZE >> 20);
  g_loader_ready = 1;
}

int kur_loader_load_buffer(const void *data, size_t size,
                           struct kur_module **out) {
  if (!out) return -EINVAL;
  *out = NULL;
  if (!g_loader_ready) {
    LOG_ERR("[KUR] loader no inicializado (falta kur_loader_init)");
    return -ENOSYS;
  }
  if (__atomic_exchange_n(&g_load_in_progress, 1, __ATOMIC_ACQ_REL)) {
    LOG_WARN("[KUR] carga concurrente rechazada");
    return -EBUSY;
  }

  int slot = -1;
  for (int i = 0; i < KUR_MAX_MODULES; i++) {
    if (!g_modules[i].in_use) { slot = i; break; }
  }
  if (slot < 0) {
    __atomic_store_n(&g_load_in_progress, 0, __ATOMIC_RELEASE);
    LOG_ERR("[KUR] sin slots libres (max=%d)", KUR_MAX_MODULES);
    return -ENOMEM;
  }

  struct kur_module tmp;
  int rc = kur_load_internal(data, size, slot, &tmp);
  if (rc < 0) {
    __atomic_store_n(&g_load_in_progress, 0, __ATOMIC_RELEASE);
    return rc;
  }

  tmp.in_use = 1;

  unsigned long flags = spin_lock_irqsave(&g_kur_lock);
  g_modules[slot] = tmp;
  *out = &g_modules[slot];
  spin_unlock_irqrestore(&g_kur_lock, flags);

  __atomic_store_n(&g_load_in_progress, 0, __ATOMIC_RELEASE);
  LOG_INFO("[KUR] '%s' cargado en slot %d", tmp.name, slot);
  return 0;
}

int kur_loader_unload(struct kur_module *mod) {
  if (!mod || !mod->in_use) return -EINVAL;
  if (__atomic_exchange_n(&g_load_in_progress, 1, __ATOMIC_ACQ_REL)) {
    LOG_WARN("[KUR] unload concurrente rechazado");
    return -EBUSY;
  }
  if (mod->n_dependents > 0) {
    LOG_WARN("[KUR] '%s' tiene %u símbolos en uso por otros módulos",
             mod->name, mod->n_dependents);
    __atomic_store_n(&g_load_in_progress, 0, __ATOMIC_RELEASE);
    return -EBUSY;
  }

  LOG_INFO("[KUR] descargando '%s' (slot %d)", mod->name, mod->index);

  if (mod->exit_fn) {
    void (*exit_fn)(void) = (void (*)(void))mod->exit_fn;
    LOG_INFO("[KUR] llamando %s en %p", KUR_SYM_EXIT, mod->exit_fn);
    exit_fn();
  }

  // Destructores (.fini_array, en orden inverso).
  if (mod->fini_array) {
    const uint64_t *fns = (const uint64_t *)mod->fini_array;
    for (size_t k = mod->fini_count; k > 0; k--) {
      if (fns[k - 1] && fns[k - 1] != UINT64_MAX)
        ((void (*)(void))fns[k - 1])();
    }
  }

  // Eliminar dynsym entries de este módulo.
  int removed = dynsym_remove_owner(mod->index);

  // Decrementar refcounts en módulos que nos importaban.
  for (int b = 0; b < KUR_MAX_MODULES; b++) {
    if (mod->n_imports_from[b] > 0) {
      if (g_modules[b].n_dependents >= mod->n_imports_from[b]) {
        g_modules[b].n_dependents -= mod->n_imports_from[b];
      } else {
        LOG_WARN("[KUR] underflow en n_dependents del slot %d", b);
        g_modules[b].n_dependents = 0;
      }
      mod->n_imports_from[b] = 0;
    }
  }

  uint64_t npages = (mod->base_size + PAGE_SIZE - 1) / PAGE_SIZE;
  vmm_free_pages(mod->base_va, npages);
  LOG_INFO("[KUR] '%s' descargado (%d exports removidos)", mod->name, removed);

  unsigned long flags = spin_lock_irqsave(&g_kur_lock);
  memset(mod, 0, sizeof(*mod));
  spin_unlock_irqrestore(&g_kur_lock, flags);

  __atomic_store_n(&g_load_in_progress, 0, __ATOMIC_RELEASE);
  return 0;
}

// ===========================================================================
// [FASE 3.3] Carga desde VFS.
// ===========================================================================

int kur_loader_load_file(const char *path, struct kur_module **out) {
  if (!path || !out)
    return -EINVAL;
  *out = NULL;

  void *buf = NULL;
  size_t size = 0;
  int rc = vfs_read_all(path, &buf, &size);
  if (rc < 0) {
    LOG_ERR("[KUR] no se pudo leer '%s': %d", path, rc);
    return rc;
  }
  if (!buf || size == 0) {
    LOG_ERR("[KUR] '%s' vacío", path);
    if (buf) kfree(buf);
    return -ENOEXEC;
  }

  LOG_DEBUG("[KUR] '%s' leído (%lu bytes), cargando...", path,
            (unsigned long)size);
  rc = kur_loader_load_buffer(buf, size, out);
  kfree(buf);
  return rc;
}

#define KUR_MAX_SCAN  32
#define KUR_NAME_MAX  64

int kur_load_all(const char *dir) {
  if (!dir)
    return -EINVAL;

  vfs_node_t *d = vfs_lookup(dir);
  if (!d) {
    LOG_ERR("[KUR] directorio '%s' no existe", dir);
    return -ENOENT;
  }
  if (!(d->flags & VFS_DIRECTORY)) {
    vfs_node_free(d);
    LOG_ERR("[KUR] '%s' no es directorio", dir);
    return -ENOTDIR;
  }

  char (*names)[KUR_NAME_MAX] =
      (char (*)[KUR_NAME_MAX])kmalloc(KUR_MAX_SCAN * KUR_NAME_MAX);
  if (!names) {
    vfs_node_free(d);
    return -ENOMEM;
  }
  int n = 0;

  vfs_dirent_t ent;
  for (uint64_t i = 0; i < 4096 && n < KUR_MAX_SCAN; i++) {
    int rc = vfs_readdir_node(d, i, &ent);
    if (rc != 0) {
      LOG_WARN("[KUR] readdir('%s') index=%lu: %d", dir,
               (unsigned long)i, rc);
      break;
    }
    if (ent.name[0] == '\0')
      break;
    if (ent.type != VFS_FILE)
      continue;
    size_t nlen = strlen(ent.name);
    if (nlen < 5 || strcmp(ent.name + nlen - 4, ".kur") != 0)
      continue;
    if (nlen >= KUR_NAME_MAX) {
      LOG_WARN("[KUR] nombre demasiado largo: %s", ent.name);
      continue;
    }
    memcpy(names[n], ent.name, nlen + 1);
    n++;
  }
  vfs_node_free(d);

  // Insertion sort (n <= 32). Asegura hello.kur antes que hello2.kur
  // para que las dependencias entre módulos se resuelvan.
  for (int i = 1; i < n; i++) {
    char tmp[KUR_NAME_MAX];
    memcpy(tmp, names[i], KUR_NAME_MAX);
    int j = i - 1;
    while (j >= 0 && strcmp(names[j], tmp) > 0) {
      memcpy(names[j + 1], names[j], KUR_NAME_MAX);
      j--;
    }
    memcpy(names[j + 1], tmp, KUR_NAME_MAX);
  }

  int loaded = 0, failed = 0;
  for (int i = 0; i < n; i++) {
    char full[VFS_PATH_MAX];
    size_t dl = strlen(dir);
    size_t nl = strlen(names[i]);
    if (dl + 1 + nl + 1 > sizeof(full)) {
      LOG_WARN("[KUR] path demasiado largo: %s/%s", dir, names[i]);
      failed++;
      continue;
    }
    memcpy(full, dir, dl);
    full[dl] = '/';
    memcpy(full + dl + 1, names[i], nl + 1);

    LOG_INFO("[KUR] cargando desde VFS: '%s'", full);
    struct kur_module *m = NULL;
    int rc = kur_loader_load_file(full, &m);
    if (rc == 0) {
      loaded++;
    } else {
      LOG_ERR("[KUR] '%s' falló: %d", full, rc);
      failed++;
    }
  }

  LOG_INFO("[KUR] kur_load_all('%s'): %d cargados, %d fallidos", dir,
           loaded, failed);
  kfree(names);
  return loaded;
}

int kur_loader_count(void) {
  int n = 0;
  for (int i = 0; i < KUR_MAX_MODULES; i++)
    if (g_modules[i].in_use) n++;
  return n;
}

struct kur_module *kur_loader_get(int index) {
  if (index < 0 || index >= KUR_MAX_MODULES) return NULL;
  return g_modules[index].in_use ? &g_modules[index] : NULL;
}

struct kur_module *kur_loader_find_by_name(const char *name) {
  if (!name) return NULL;
  for (int i = 0; i < KUR_MAX_MODULES; i++) {
    if (g_modules[i].in_use && strcmp(g_modules[i].name, name) == 0)
      return &g_modules[i];
  }
  return NULL;
}

int kur_dynsym_count(void) {
  unsigned long flags = spin_lock_irqsave(&g_dynsym_lock);
  int n = g_dynsym_count;
  spin_unlock_irqrestore(&g_dynsym_lock, flags);
  return n;
}

uint64_t kur_loader_lookup_symbol(const char *name) {
  return name ? kur_dynsym_lookup(name, NULL) : 0;
}
