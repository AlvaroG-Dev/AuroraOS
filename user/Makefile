# user/Makefile — Userland nativo (Aurora ABI).
#
# Solo compila las apps que siguen en user/apps/. Las que se han movido
# a user/musl/apps/ se detectan por ausencia de apps/<name>/main.c y se
# saltan automáticamente.

AS = nasm

CROSS := $(shell which x86_64-elf-gcc 2>/dev/null)
ifeq ($(CROSS),)
    CC = gcc
    CFLAGS_FREESTANDING_PIC = -fno-pic
else
    CC = x86_64-elf-gcc
    CFLAGS_FREESTANDING_PIC =
endif

CFLAGS_COMMON = -ffreestanding -O2 -Wall -Wextra -fno-exceptions \
                -fno-stack-protector -nostdlib -nostartfiles -nodefaultlibs \
                -I. -I./lib -mcmodel=small -m64 -fstack-clash-protection

CFLAGS = $(CFLAGS_COMMON) $(CFLAGS_FREESTANDING_PIC)

LD = $(CC)
LDFLAGS = -nostdlib -T link.ld -no-pie -m64 \
          -Wl,-z,max-page-size=0x1000,-z,common-page-size=0x1000,--build-id=none

SYSDIR := ../sysroot/apps

NATIVE_ALL = hello calc filetest ipctest pftest waitpid_stress \
             mmap_race mmap_race_worker vma_overlap vma_isolation \
             vma_isolation_worker munmap_partial \
             elf_validation elf_permtest \
             sbrk_shrink_test sbrk_shrink calloc_overflow \
             shell ls cat echo touch wc head grep tee sort \
             segvtest segvtest_stack segvtest_nx segvtest_mmap_ro segvtest_kernel \
             win_blit_fault \
             mkdir rm cp pwd mv kill

CORE_APPS := $(foreach a,$(NATIVE_ALL),$(if $(wildcard apps/$a/main.c),$a))
CORE_APPS := $(filter-out shell,$(CORE_APPS))

STANDALONE_APPS = syscall_linux_test
SPECIAL_APPS = elf_rotest

APPS = $(CORE_APPS) $(STANDALONE_APPS) $(SPECIAL_APPS)

LIB_OBJS = crt0.o syscall.o \
           lib/string.o lib/malloc.o lib/file.o lib/ipc.o lib/process.o \
           lib/env.o

SHELL_LIBS = $(LIB_OBJS) lib/font_system.o

all: $(APPS) shell

crt0.o: crt0.asm
	$(AS) -f elf64 $< -o $@

syscall.o: syscall.c syscall.h
	$(CC) $(CFLAGS) -c $< -o $@

lib/%.o: lib/%.c
	$(CC) $(CFLAGS) -c $< -o $@

lib/font_system.o: lib/font_system.c lib/font_system.h lib/font_aa.h
	$(CC) $(CFLAGS) -c $< -o $@

lib/env.o: lib/env.c lib/env.h lib/string.h lib/malloc.h
	$(CC) $(CFLAGS) -c $< -o $@

$(CORE_APPS): %: apps/%/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c $< -o $@.o
	$(LD) -o $@ $(LIB_OBJS) $@.o $(LDFLAGS)
	@mkdir -p $(SYSDIR)
	cp $@ $(SYSDIR)/

shell: apps/shell/main.c $(SHELL_LIBS)
	$(CC) $(CFLAGS) -c $< -o $@.o
	$(LD) -o $@ $(SHELL_LIBS) $@.o $(LDFLAGS)
	@mkdir -p $(SYSDIR)
	cp $@ $(SYSDIR)/

syscall_linux_test: apps/syscall_linux_test/main.c
	$(CC) $(CFLAGS) -c $< -o $@.o
	$(LD) -o $@ $@.o $(LDFLAGS)
	@mkdir -p $(SYSDIR)
	cp $@ $(SYSDIR)/

elf_rotest: apps/elf_rotest/main.c apps/elf_rotest/link.ld $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/elf_rotest/main.c -o $@.o
	$(LD) -o $@ $(LIB_OBJS) $@.o -T apps/elf_rotest/link.ld $(LDFLAGS)
	@mkdir -p $(SYSDIR)
	cp $@ $(SYSDIR)/

.PHONY: regen-font
regen-font:
	@mkdir -p lib
	@{ \
	  echo '// user/lib/font_system.c — GENERADO. No editar a mano.'; \
	  echo '// Regenerar con: make -C user regen-font'; \
	  echo '#include "font_system.h"'; \
	  sed -e 's|#include "../font_aa.h"||' \
	      ../kernel/gfx/fonts/font_jetbrains_regular14.h; \
	  echo ''; \
	  echo 'const font_aa_t *font_system_mono(void) {'; \
	  echo '  return &font_jetbrainsmono_regular_regular_14;'; \
	  echo '}'; \
	} > lib/font_system.c
	@echo "[font] lib/font_system.c regenerado"

clean:
	rm -f *.o lib/*.o
	rm -f $(APPS) shell
	rm -f $(addprefix $(SYSDIR)/,$(APPS)) $(SYSDIR)/shell

.PHONY: all clean regen-font