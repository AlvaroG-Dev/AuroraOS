// user/apps/hello_musl/main.c
//
// PR 3b — Primer binario compilado con musl (x86_64-linux-musl-gcc).
// Depende del auxv que process.c construye en setup_arg_block:
//   AT_PHDR, AT_PHNUM, AT_PHENT, AT_PAGESZ, AT_ENTRY, AT_RANDOM, AT_EXECFN.
//
// Si imprime las dos líneas y sale con código 0, el ABI Linux del kernel
// es suficiente para que musl complete __libc_start_main.

#include <stdio.h>
#include <unistd.h>

int main(void) {
  printf("hello from musl (pid=%d)\n", (int)getpid());
  fflush(stdout);
  write(1, "musl: write(2) directo OK\n", 26);
  return 0;
}