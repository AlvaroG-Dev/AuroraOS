# user/Makefile — Userland nativo (Aurora ABI).
#
# Solo compila las apps que siguen en user/apps/. Las que se han movido
# a user/musl/apps/ se detectan por ausencia de apps/<name>/main.c y se
# saltan automáticamente. Así no hay que tocar este Makefile al portar
# una app a musl: basta con mover el directorio.

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

# Lista "ideal" de apps nativas. Se filtra por existencia de
# apps/<name>/main.c para permitir mover apps a user/musl/ sin tocar
# este Makefile.
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

STANDALONE_APPS = syscall_linux_test
SPECIAL_APPS = elf_rotest

APPS = $(CORE_APPS) $(STANDALONE_APPS) $(SPECIAL_APPS)

LIB_OBJS = crt0.o syscall.o \
           lib/string.o lib/malloc.o lib/file.o lib/ipc.o lib/process.o

all: $(APPS)

crt0.o: crt0.asm
	$(AS) -f elf64 $< -o $@

syscall.o: syscall.c syscall.h
	$(CC) $(CFLAGS) -c $< -o $@

lib/%.o: lib/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Pattern rule para apps nativas (linkean LIB_OBJS + link.ld).
$(CORE_APPS): %: apps/%/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c $< -o $@.o
	$(LD) -o $@ $(LIB_OBJS) $@.o $(LDFLAGS)
	@mkdir -p $(SYSDIR)
	cp $@ $(SYSDIR)/

# Standalone: define su propio _start, sin LIB_OBJS ni crt0.
syscall_linux_test: apps/syscall_linux_test/main.c
	$(CC) $(CFLAGS) -c $< -o $@.o
	$(LD) -o $@ $@.o $(LDFLAGS)
	@mkdir -p $(SYSDIR)
	cp $@ $(SYSDIR)/

# elf_rotest: link.ld propio para forzar segmentos RO.
elf_rotest: apps/elf_rotest/main.c apps/elf_rotest/link.ld $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/elf_rotest/main.c -o $@.o
	$(LD) -o $@ $(LIB_OBJS) $@.o -T apps/elf_rotest/link.ld $(LDFLAGS)
	@mkdir -p $(SYSDIR)
	cp $@ $(SYSDIR)/

clean:
	rm -f *.o lib/*.o
	rm -f $(APPS)
	rm -f $(addprefix $(SYSDIR)/,$(APPS))

.PHONY: all clean