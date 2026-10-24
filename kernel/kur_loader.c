// kernel/kur_loader.c
//
// Loader de .kur (Fase 3.2). Añade sobre 3.1:
//   - Verificación SHA-256 del fichero (si KUR_FLAG_HASHED).
//   - Registro de exports del módulo (`.kur_ksymtab`) en la tabla dinámica.
//   - Resolución de imports: ksymtab estático -> dynsym dinámico.
//   - Refcounts entre módulos (n_dependents, n_imports_from).
//   - kur_loader_unload(): llama kur_exit, elimina exports, libera VA.

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

#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_NOBITS   8
#define SHF_ALLOC    0x2u
#define SHN_UNDEF    0
#define SHN_ABS      0xFFF1
#define SHN_COMMON   0xFFF2

#define R_X86_64_64    1
#define R_X86_64_PC32  2
#define R_X86_64_PLT32 4
#define R_X86_64_32    10
#define R_X86_64_32S   11

#define MAX_LOADED_SECS 64

struct loaded_section {
  uint32_t shndx;
  uint32_t sh_type;
  uint64_t sh_size;
  uint64_t load_offset;
};

struct kur_dynsym {
  char     name[64];
  uint64_t addr;
  uint8_t  owner;    // slot de g_modules[] que lo exporta
  uint8_t  in_use;
};

static spinlock_t g_kur_lock = {.locked = 0};
static spinlock_t g_dynsym_lock = {.locked = 0};
static volatile int g_loader_ready = 0;
static volatile int g_load_in_progress = 0;
static uint64_t g_module_cursor = 0;
static struct kur_module g_modules[KUR_MAX_MODULES];
static struct kur_dynsym g_dynsym[KUR_MAX_DYNSYMS];

static inline uint64_t align_up(uint64_t v, uint64_t a) {
  return (v + a - 1) & ~(a - 1);
}

static size_t bounded_strlen(const char *s, size_t max) {
  size_t n = 0;
  while (n < max && s[n]) n++;
  return n;
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

static const Elf64_Sym *sym_find(const uint8_t *base, const Elf64_Shdr *shdrs,
                                 int n_shdrs, const char *name) {
  for (int i = 0; i < n_shdrs; i++) {
    if (shdrs[i].sh_type != SHT_SYMTAB) continue;
    if (shdrs[i].sh_link >= (uint32_t)n_shdrs) continue;
    const Elf64_Sym *syms = (const Elf64_Sym *)(base + shdrs[i].sh_offset);
    size_t n = shdrs[i].sh_size / sizeof(Elf64_Sym);
    const Elf64_Shdr *strhdr = &shdrs[shdrs[i].sh_link];
    const char *strs = (const char *)(base + strhdr->sh_offset);
    for (size_t j = 0; j < n; j++) {
      if (syms[j].st_name >= strhdr->sh_size) continue;
      if (strcmp(strs + syms[j].st_name, name) == 0)
        return &syms[j];
    }
  }
  return NULL;
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
  for (int i = 0; i < KUR_MAX_DYNSYMS; i++) {
    if (!g_dynsym[i].in_use) continue;
    if (strcmp(g_dynsym[i].name, name) == 0) {
      addr = g_dynsym[i].addr;
      owner = g_dynsym[i].owner;
      break;
    }
  }
  spin_unlock_irqrestore(&g_dynsym_lock, flags);
  if (owner_out) *owner_out = owner;
  return addr;
}

static int find_section_by_name(const char *name, const Elf64_Shdr *shdrs,
                                int n_shdrs, int shstrndx, const void *data,
                                size_t size, uint32_t *out_idx) {
  if (shstrndx <= 0 || shstrndx >= n_shdrs) return -1;
  const Elf64_Shdr *strh = &shdrs[shstrndx];
  if (strh->sh_type != SHT_STRTAB) return -1;
  if (strh->sh_offset + strh->sh_size > size) return -1;
  const char *strs = (const char *)((const uint8_t *)data + strh->sh_offset);
  for (int i = 0; i < n_shdrs; i++) {
    if (shdrs[i].sh_name >= strh->sh_size) continue;
    const char *sname = strs + shdrs[i].sh_name;
    size_t remaining = strh->sh_size - shdrs[i].sh_name;
    size_t nlen = bounded_strlen(sname, remaining);
    if (nlen < remaining && strcmp(sname, name) == 0) {
      *out_idx = (uint32_t)i;
      return 0;
    }
  }
  return -1;
}

// Registra los exports del módulo en la tabla dinámica.
// Se llama DESPUÉS de aplicar relocaciones y de kur_init OK.
static int register_module_exports(struct kur_module *mod, const void *data,
                                   size_t size, const Elf64_Shdr *shdrs,
                                   int n_shdrs, int shstrndx,
                                   const struct loaded_section *secs,
                                   int n_secs, uint64_t base) {
  uint32_t exp_idx;
  if (find_section_by_name(KUR_EXPORT_SECTION, shdrs, n_shdrs, shstrndx,
                           data, size, &exp_idx) != 0)
    return 0;  // módulo sin exports: OK

  const Elf64_Shdr *exp_sh = &shdrs[exp_idx];
  uint64_t exp_off = 0;
  int found = 0;
  for (int i = 0; i < n_secs; i++) {
    if (secs[i].shndx == exp_idx) {
      exp_off = secs[i].load_offset;
      found = 1;
      break;
    }
  }
  if (!found) {
    LOG_ERR("[KUR] %s presente pero no allocable", KUR_EXPORT_SECTION);
    return -ENOEXEC;
  }
  size_t n_entries = exp_sh->sh_size / sizeof(struct kur_export_entry);
  const struct kur_export_entry *entries =
      (const struct kur_export_entry *)((const uint8_t *)base + exp_off);

  for (size_t i = 0; i < n_entries; i++) {
    const char *name = entries[i].name;
    uint64_t addr = entries[i].addr;
    if (!name || !name[0]) {
      LOG_ERR("[KUR] export[%zu]: sin nombre", i);
      return -ENOEXEC;
    }
    size_t nlen = bounded_strlen(name, 63);
    if (nlen == 0) {
      LOG_ERR("[KUR] export[%zu]: nombre vacío", i);
      return -ENOEXEC;
    }
    unsigned long flags = spin_lock_irqsave(&g_dynsym_lock);
    int slot = -1;
    int dup = 0;
    for (int j = 0; j < KUR_MAX_DYNSYMS; j++) {
      if (g_dynsym[j].in_use && strcmp(g_dynsym[j].name, name) == 0) {
        dup = 1;
        break;
      }
      if (!g_dynsym[j].in_use && slot < 0) slot = j;
    }
    if (dup) {
      spin_unlock_irqrestore(&g_dynsym_lock, flags);
      LOG_ERR("[KUR] export duplicado: '%s'", name);
      return -EEXIST;
    }
    if (slot < 0) {
      spin_unlock_irqrestore(&g_dynsym_lock, flags);
      LOG_ERR("[KUR] tabla dynsym llena (max=%d)", KUR_MAX_DYNSYMS);
      return -ENOMEM;
    }
    memcpy(g_dynsym[slot].name, name, nlen);
    g_dynsym[slot].name[nlen] = '\0';
    g_dynsym[slot].addr = addr;
    g_dynsym[slot].owner = (uint8_t)mod->index;
    g_dynsym[slot].in_use = 1;
    spin_unlock_irqrestore(&g_dynsym_lock, flags);
    LOG_INFO("[KUR] export '%s' @ %p", name, (void *)addr);
  }
  return 0;
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
  if (ehdr->e_shoff > size || ehdr->e_shentsize != sizeof(Elf64_Shdr) ||
      ehdr->e_shnum == 0 || ehdr->e_shnum > MAX_LOADED_SECS) {
    LOG_ERR("[KUR] section header table inválida");
    return -ENOEXEC;
  }
  if (ehdr->e_shoff + (uint64_t)ehdr->e_shnum * sizeof(Elf64_Shdr) > size) {
    LOG_ERR("[KUR] section headers fuera del fichero");
    return -ENOEXEC;
  }

  const Elf64_Shdr *shdrs =
      (const Elf64_Shdr *)((const uint8_t *)data + ehdr->e_shoff);

  // -- 1. Manifest ---------------------------------------------------------
  const Elf64_Sym *mfs =
      sym_find(data, shdrs, ehdr->e_shnum, KUR_SYM_MANIFEST);
  if (!mfs || mfs->st_shndx == SHN_UNDEF || mfs->st_shndx >= ehdr->e_shnum) {
    LOG_ERR("[KUR] falta '%s' definido", KUR_SYM_MANIFEST);
    return -ENOEXEC;
  }
  const Elf64_Shdr *mfsh = &shdrs[mfs->st_shndx];
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

  int rc = validate_manifest(mf);
  if (rc < 0) return rc;
  LOG_INFO("[KUR] manifest OK: name='%s' abi=%u flags=0x%x",
           mf->name, mf->abi_version, mf->flags);

  // -- 1b. Hash SHA-256 ----------------------------------------------------
  uint64_t hash_file_off = manifest_off + offsetof(struct kur_manifest, hash);
  rc = verify_manifest_hash(data, size, hash_file_off, mf);
  if (rc < 0) return rc;

  // -- 2. init/exit --------------------------------------------------------
  const Elf64_Sym *init_sym = sym_find(data, shdrs, ehdr->e_shnum, KUR_SYM_INIT);
  if (!init_sym || init_sym->st_shndx == SHN_UNDEF) {
    LOG_ERR("[KUR] falta '%s'", KUR_SYM_INIT);
    return -ENOEXEC;
  }
  const Elf64_Sym *exit_sym = sym_find(data, shdrs, ehdr->e_shnum, KUR_SYM_EXIT);

  // -- 3. Layout ----------------------------------------------------------
  struct loaded_section secs[MAX_LOADED_SECS];
  int n_secs = 0;
  uint64_t total = 0;
  for (int i = 0; i < ehdr->e_shnum; i++) {
    const Elf64_Shdr *s = &shdrs[i];
    if (!(s->sh_flags & SHF_ALLOC) || s->sh_size == 0) continue;
    if (s->sh_type != SHT_PROGBITS && s->sh_type != SHT_NOBITS) continue;
    if (n_secs >= MAX_LOADED_SECS) return -ENOEXEC;
    uint64_t align = s->sh_addralign ? s->sh_addralign : 16;
    if (align > 4096) align = 4096;
    uint64_t off = align_up(total, align);
    secs[n_secs].shndx = (uint32_t)i;
    secs[n_secs].sh_type = s->sh_type;
    secs[n_secs].sh_size = s->sh_size;
    secs[n_secs].load_offset = off;
    n_secs++;
    total = off + s->sh_size;
  }
  if (total == 0) {
    LOG_ERR("[KUR] sin secciones allocables");
    return -ENOEXEC;
  }

  // -- 4. Alloc VA --------------------------------------------------------
  uint64_t base = next_module_base(total);
  uint64_t npages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
  if (vmm_alloc_pages(base, npages, PTE_WRITABLE | PTE_GLOBAL) != 0) {
    LOG_ERR("[KUR] vmm_alloc_pages(%lu págs) falló en %p",
            (unsigned long)npages, (void *)base);
    return -ENOMEM;
  }
  memset((void *)base, 0, total);
  LOG_INFO("[KUR] cargado en %p size=%lu", (void *)base,
           (unsigned long)total);

  // -- 5. Copiar PROGBITS -------------------------------------------------
  for (int i = 0; i < n_secs; i++) {
    if (secs[i].sh_type == SHT_NOBITS) continue;
    const Elf64_Shdr *s = &shdrs[secs[i].shndx];
    if (s->sh_offset + s->sh_size > size) {
      LOG_ERR("[KUR] sección %u fuera del fichero", secs[i].shndx);
      goto fail;
    }
    memcpy((uint8_t *)base + secs[i].load_offset,
           (const uint8_t *)data + s->sh_offset, s->sh_size);
  }

  // -- 6. Pre-resolver símbolos ------------------------------------------
  const Elf64_Shdr *symtab_hdr = NULL;
  for (int i = 0; i < ehdr->e_shnum; i++) {
    if (shdrs[i].sh_type == SHT_SYMTAB) { symtab_hdr = &shdrs[i]; break; }
  }
  if (!symtab_hdr) {
    LOG_ERR("[KUR] sin .symtab");
    goto fail;
  }
  size_t n_syms = symtab_hdr->sh_size / sizeof(Elf64_Sym);
  const Elf64_Sym *syms =
      (const Elf64_Sym *)((const uint8_t *)data + symtab_hdr->sh_offset);
  const char *strs =
      (const char *)((const uint8_t *)data +
                     shdrs[symtab_hdr->sh_link].sh_offset);

  uint64_t *sym_addrs = (uint64_t *)kmalloc(n_syms * sizeof(uint64_t));
  if (!sym_addrs) { rc = -ENOMEM; goto fail; }
  for (size_t i = 0; i < n_syms; i++) {
    const Elf64_Sym *s = &syms[i];
    if (i == 0) { sym_addrs[i] = 0; continue; }

    if (s->st_shndx == SHN_UNDEF) {
      if (s->st_name == 0) {
        LOG_ERR("[KUR] sym[%zu]: undefined sin nombre", i);
        kfree(sym_addrs); goto fail;
      }
      const char *nm = strs + s->st_name;
      // 1) ksymtab estático (kernel).
      uint64_t a = ksymtab_lookup(nm);
      int owner = -1;
      // 2) dynsym dinámico (otros módulos).
      if (a == 0)
        a = kur_dynsym_lookup(nm, &owner);
      if (a == 0) {
        LOG_ERR("[KUR] símbolo sin resolver: '%s'", nm);
        kfree(sym_addrs); goto fail;
      }
      sym_addrs[i] = a;
      if (owner >= 0 && owner != slot) {
        // [FIX 3.2] Marcar el exportador como "en uso" para que unload
        // rechace con -EBUSY mientras este módulo viva. Se deshace en
        // el fail: y en kur_loader_unload().
        if (owner >= KUR_MAX_MODULES || !g_modules[owner].in_use) {
          LOG_ERR("[KUR] dynsym apunta a slot %d inválido (in_use=%d)",
                  owner, g_modules[owner].in_use);
          kfree(sym_addrs);
          goto fail;
        }
        out->n_imports_from[owner]++;
        g_modules[owner].n_dependents++;
      }
    } else if (s->st_shndx == SHN_ABS) {
      sym_addrs[i] = s->st_value;
    } else if (s->st_shndx == SHN_COMMON) {
      LOG_ERR("[KUR] SHN_COMMON no soportado (usa -fno-common)");
      kfree(sym_addrs); goto fail;
    } else {
      uint64_t sec_off = 0;
      for (int k = 0; k < n_secs; k++) {
        if (secs[k].shndx == s->st_shndx) {
          sec_off = secs[k].load_offset;
          break;
        }
      }
      sym_addrs[i] = base + sec_off + s->st_value;
    }
  }

  // -- 7. Aplicar relocaciones SHT_RELA ----------------------------------
  for (int i = 0; i < ehdr->e_shnum; i++) {
    const Elf64_Shdr *rh = &shdrs[i];
    if (rh->sh_type != SHT_RELA) continue;
    uint64_t target_off = 0;
    int found = 0;
    for (int k = 0; k < n_secs; k++) {
      if (secs[k].shndx == rh->sh_info) {
        target_off = secs[k].load_offset;
        found = 1;
        break;
      }
    }
    if (!found) continue;
    if (rh->sh_offset + rh->sh_size > size) {
      LOG_ERR("[KUR] rela sección %d fuera del fichero", i);
      kfree(sym_addrs); goto fail;
    }
    const Elf64_Rela *relas =
        (const Elf64_Rela *)((const uint8_t *)data + rh->sh_offset);
    size_t n_relas = rh->sh_size / sizeof(Elf64_Rela);
    for (size_t j = 0; j < n_relas; j++) {
      const Elf64_Rela *r = &relas[j];
      uint32_t sym_idx = (uint32_t)(r->r_info >> 32);
      uint32_t type = (uint32_t)(r->r_info & 0xFFFFFFFFu);
      if (sym_idx >= n_syms) {
        LOG_ERR("[KUR] rela %zu: sym_idx fuera de rango", j);
        kfree(sym_addrs); goto fail;
      }
      uint64_t S = sym_addrs[sym_idx];
      int64_t A = r->r_addend;
      uint64_t P = base + target_off + r->r_offset;
      switch (type) {
      case R_X86_64_64:
        *(uint64_t *)P = S + A;
        break;
      case R_X86_64_PC32:
      case R_X86_64_PLT32: {
        int64_t v = (int64_t)S + A - (int64_t)P;
        if (v > INT32_MAX || v < INT32_MIN) {
          LOG_ERR("[KUR] PC32 fuera de rango: %lld", (long long)v);
          kfree(sym_addrs); goto fail;
        }
        *(int32_t *)P = (int32_t)v;
        break;
      }
      case R_X86_64_32: {
        uint64_t v = S + A;
        if (v > 0xFFFFFFFFULL) {
          LOG_ERR("[KUR] R_X86_64_32 overflow: 0x%lx", (unsigned long)v);
          kfree(sym_addrs); goto fail;
        }
        *(uint32_t *)P = (uint32_t)v;
        break;
      }
      case R_X86_64_32S: {
        int64_t v = (int64_t)S + A;
        if (v > INT32_MAX || v < INT32_MIN) {
          LOG_ERR("[KUR] R_X86_64_32S overflow: %lld", (long long)v);
          kfree(sym_addrs); goto fail;
        }
        *(int32_t *)P = (int32_t)v;
        break;
      }
      default:
        LOG_ERR("[KUR] reloc tipo %u no soportado (rela %zu)", type, j);
        kfree(sym_addrs); goto fail;
      }
    }
  }
  kfree(sym_addrs);

  // -- 8. kur_init -------------------------------------------------------
  uint64_t init_off = 0;
  for (int k = 0; k < n_secs; k++) {
    if (secs[k].shndx == init_sym->st_shndx) {
      init_off = secs[k].load_offset;
      break;
    }
  }
  void *init_addr = (void *)(base + init_off + init_sym->st_value);
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
  if (exit_sym && exit_sym->st_shndx != SHN_UNDEF) {
    uint64_t eoff = 0;
    for (int k = 0; k < n_secs; k++) {
      if (secs[k].shndx == exit_sym->st_shndx) {
        eoff = secs[k].load_offset;
        break;
      }
    }
    out->exit_fn = (void *)(base + eoff + exit_sym->st_value);
  }

  // -- 10. Registrar exports (ya con módulo cargado y kur_init OK) -------
  rc = register_module_exports(out, data, size, shdrs, ehdr->e_shnum,
                               ehdr->e_shstrndx, secs, n_secs, base);
  if (rc < 0) {
    // Rollback: descargar y fallar.
    goto fail;
  }

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
  {
    unsigned long flags = spin_lock_irqsave(&g_dynsym_lock);
    for (int i = 0; i < KUR_MAX_DYNSYMS; i++) {
      if (g_dynsym[i].in_use && g_dynsym[i].owner == (uint8_t)out->index)
        g_dynsym[i].in_use = 0;
    }
    spin_unlock_irqrestore(&g_dynsym_lock, flags);
  }
  vmm_free_pages(base, npages);
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
  LOG_INFO("[KUR] loader listo (ABI v%u, max %d módulos, max %d dynsyms)",
           KUR_ABI_VERSION, KUR_MAX_MODULES, KUR_MAX_DYNSYMS);
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

  // Eliminar dynsym entries de este módulo.
  int removed = 0;
  {
    unsigned long flags = spin_lock_irqsave(&g_dynsym_lock);
    for (int i = 0; i < KUR_MAX_DYNSYMS; i++) {
      if (g_dynsym[i].in_use && g_dynsym[i].owner == (uint8_t)mod->index) {
        g_dynsym[i].in_use = 0;
        removed++;
      }
    }
    spin_unlock_irqrestore(&g_dynsym_lock, flags);
  }

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
  int n = 0;
  unsigned long flags = spin_lock_irqsave(&g_dynsym_lock);
  for (int i = 0; i < KUR_MAX_DYNSYMS; i++)
    if (g_dynsym[i].in_use) n++;
  spin_unlock_irqrestore(&g_dynsym_lock, flags);
  return n;
}