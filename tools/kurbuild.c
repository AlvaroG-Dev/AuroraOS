// tools/kurbuild.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <elf.h>
#include "sha256.h"

#define KUR_MAGIC       0x4B555221u
#define KUR_ABI_VERSION 2u
#define KUR_MAX_NAME    64
#define KUR_FLAG_HASHED 0x0001u

struct kur_manifest {
  uint32_t magic;
  uint16_t abi_version;
  uint16_t flags;
  uint32_t name_len;
  uint32_t reserved;
  char     name[KUR_MAX_NAME];
  uint8_t  hash[32];
};

static void usage(const char *prog) {
  fprintf(stderr,
          "uso: %s [-i|--info] [-H|--hash] <input.o|input.kur> [<output.kur>]\n"
          "  -i, --info   imprime el manifiesto y símbolos\n"
          "  -H, --hash   calcula SHA-256, marca flags|HASHED, embebe el hash\n"
          "               requiere <output>\n",
          prog);
}

static const Elf64_Sym *find_sym(const Elf64_Ehdr *eh, const char *name) {
  const Elf64_Shdr *sh = (const Elf64_Shdr *)((const char *)eh + eh->e_shoff);
  for (int i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type != SHT_SYMTAB) continue;
    if (sh[i].sh_link >= eh->e_shnum) continue;
    const Elf64_Sym *syms = (const Elf64_Sym *)((const char *)eh + sh[i].sh_offset);
    const Elf64_Shdr *strh = &sh[sh[i].sh_link];
    const char *strs = (const char *)((const char *)eh + strh->sh_offset);
    int n = (int)(sh[i].sh_size / sizeof(Elf64_Sym));
    for (int j = 0; j < n; j++) {
      if (syms[j].st_name >= strh->sh_size) continue;
      if (strcmp(strs + syms[j].st_name, name) == 0) return &syms[j];
    }
  }
  return NULL;
}

static void dump_syms(const Elf64_Ehdr *eh, int only_undef) {
  const Elf64_Shdr *sh = (const Elf64_Shdr *)((const char *)eh + eh->e_shoff);
  for (int i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type != SHT_SYMTAB) continue;
    if (sh[i].sh_link >= eh->e_shnum) continue;
    const Elf64_Sym *syms = (const Elf64_Sym *)((const char *)eh + sh[i].sh_offset);
    const Elf64_Shdr *strh = &sh[sh[i].sh_link];
    const char *strs = (const char *)((const char *)eh + strh->sh_offset);
    int n = (int)(sh[i].sh_size / sizeof(Elf64_Sym));
    for (int j = 0; j < n; j++) {
      if (syms[j].st_name >= strh->sh_size) continue;
      const char *nm = strs + syms[j].st_name;
      if (!nm[0]) continue;
      int is_undef = (syms[j].st_shndx == SHN_UNDEF);
      if (only_undef && !is_undef) continue;
      if (!only_undef && is_undef) continue;
      printf("  %-40s shndx=%-5u value=0x%lx\n", nm, syms[j].st_shndx,
             (unsigned long)syms[j].st_value);
    }
  }
}

int main(int argc, char **argv) {
  int info = 0, do_hash = 0;
  const char *in = NULL, *out = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-i") || !strcmp(argv[i], "--info")) info = 1;
    else if (!strcmp(argv[i], "-H") || !strcmp(argv[i], "--hash")) do_hash = 1;
    else if (!in) in = argv[i];
    else if (!out) out = argv[i];
    else { usage(argv[0]); return 2; }
  }
  if (!in) { usage(argv[0]); return 2; }
  if (do_hash && !out) {
    fprintf(stderr, "kurbuild: -H requiere <output>\n");
    usage(argv[0]);
    return 2;
  }

  FILE *f = fopen(in, "rb");
  if (!f) { perror("fopen(in)"); return 1; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < (long)sizeof(Elf64_Ehdr)) {
    fclose(f); fprintf(stderr, "kurbuild: archivo demasiado pequeño\n");
    return 1;
  }
  uint8_t *data = malloc((size_t)sz);
  if (!data) { fclose(f); return 1; }
  if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
    fclose(f); free(data); return 1;
  }
  fclose(f);

  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
  if (memcmp(eh->e_ident, ELFMAG, 4) != 0) {
    fprintf(stderr, "kurbuild: no es ELF\n"); free(data); return 1;
  }
  if (eh->e_ident[EI_CLASS] != ELFCLASS64 ||
      eh->e_ident[EI_DATA] != ELFDATA2LSB ||
      eh->e_machine != EM_X86_64) {
    fprintf(stderr, "kurbuild: no es ELF64 LSB x86-64\n");
    free(data); return 1;
  }
  if (eh->e_type != ET_REL) {
    fprintf(stderr, "kurbuild: e_type=%u, esperado ET_REL(1)\n", eh->e_type);
    free(data); return 1;
  }

  const Elf64_Sym *ms = find_sym(eh, "__kur_manifest");
  if (!ms || ms->st_shndx == SHN_UNDEF) {
    fprintf(stderr, "kurbuild: falta __kur_manifest\n"); free(data); return 1;
  }
  const Elf64_Sym *is = find_sym(eh, "kur_init");
  if (!is || is->st_shndx == SHN_UNDEF) {
    fprintf(stderr, "kurbuild: falta kur_init\n"); free(data); return 1;
  }

  const Elf64_Shdr *sh = (const Elf64_Shdr *)((const char *)data + eh->e_shoff);
  if (ms->st_shndx >= eh->e_shnum) {
    fprintf(stderr, "kurbuild: shndx inválido\n"); free(data); return 1;
  }
  const Elf64_Shdr *mfs = &sh[ms->st_shndx];
  if (mfs->sh_offset + mfs->sh_size > (uint64_t)sz) {
    fprintf(stderr, "kurbuild: manifest fuera del archivo\n");
    free(data); return 1;
  }
  if (mfs->sh_size < sizeof(struct kur_manifest)) {
    fprintf(stderr, "kurbuild: manifest pequeño (%lu < %lu)\n",
            (unsigned long)mfs->sh_size,
            (unsigned long)sizeof(struct kur_manifest));
    free(data); return 1;
  }

  uint64_t manifest_off = mfs->sh_offset + ms->st_value;
  if (manifest_off + sizeof(struct kur_manifest) > (uint64_t)sz) {
    fprintf(stderr, "kurbuild: manifest fuera del archivo (2)\n");
    free(data); return 1;
  }
  struct kur_manifest *mf = (struct kur_manifest *)(data + manifest_off);

  if (mf->magic != KUR_MAGIC) {
    fprintf(stderr, "kurbuild: magic 0x%x (esperado 0x%x)\n",
            mf->magic, KUR_MAGIC);
    free(data); return 1;
  }
  if (mf->abi_version != KUR_ABI_VERSION) {
    fprintf(stderr, "kurbuild: ABI v%u (esperado v%u)\n",
            mf->abi_version, KUR_ABI_VERSION);
    free(data); return 1;
  }
  if (mf->name_len == 0 || mf->name_len >= KUR_MAX_NAME) {
    fprintf(stderr, "kurbuild: name_len %u\n", mf->name_len);
    free(data); return 1;
  }

  if (info) {
    printf("kur module: %s\n", mf->name);
    printf("  abi_version: %u\n", mf->abi_version);
    printf("  flags:       0x%04x%s\n", mf->flags,
           (mf->flags & KUR_FLAG_HASHED) ? " (HASHED)" : "");
    printf("  size:        %ld bytes\n", sz);
    printf("  e_shnum:     %u\n", eh->e_shnum);
    printf("  kur_init:    st_value=0x%lx\n", (unsigned long)is->st_value);
    printf("  símbolos definidos:\n");
    dump_syms(eh, 0);
    printf("  símbolos indefinidos:\n");
    dump_syms(eh, 1);
    if (mf->flags & KUR_FLAG_HASHED) {
      printf("  hash:        ");
      for (int i = 0; i < 32; i++) printf("%02x", mf->hash[i]);
      printf("\n");
    }
  }

  if (do_hash) {
    memset(mf->hash, 0, 32);
    mf->flags |= KUR_FLAG_HASHED;
    sha256(data, (size_t)sz, mf->hash);
    printf("kurbuild: SHA-256 = ");
    for (int i = 0; i < 32; i++) printf("%02x", mf->hash[i]);
    printf("\n");
  }

  if (out) {
    FILE *o = fopen(out, "wb");
    if (!o) { perror("fopen(out)"); free(data); return 1; }
    if (fwrite(data, 1, (size_t)sz, o) != (size_t)sz) {
      fclose(o); free(data); return 1;
    }
    fclose(o);
    printf("kurbuild: %s -> %s (%ld bytes)\n", in, out, sz);
  }
  free(data);
  return 0;
}