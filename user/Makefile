# ============================================================================
# user/Makefile — Aurora OS userland
# ============================================================================
# Organización:
#   CORE_APPS       apps regulares: linkean contra LIB_OBJS + link.ld común.
#   UTIL_APPS       como CORE_APPS, pero el binario se llama como el dir
#                   (mkdir, rm, cp, pwd, mv, kill). En la versión vieja eran
#                   app_mkdir → mkdir, etc.; ahora es 1:1.
#   STANDALONE_APPS define su propio _start y NO linkea LIB_OBJS.
#   elf_rotest      link.ld propio para forzar segmentos read-only.
#   hello_musl      compilado con x86_64-linux-musl-gcc, sin LIB_OBJS.
# ============================================================================

AS = nasm

# --- Toolchain --------------------------------------------------------------
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
# Páginas de 4 KiB (Aurora OS), sin build-id en la imagen cargable.
LDFLAGS = -nostdlib -T link.ld -no-pie -m64 \
          -Wl,-z,max-page-size=0x1000,-z,common-page-size=0x1000,--build-id=none

# Toolchain musl (opcional, solo para hello_musl).
MUSL_CC := ../toolchain/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc

SYSDIR := ../sysroot/apps

# --- Apps -------------------------------------------------------------------
CORE_APPS = hello calc filetest ipctest pftest waitpid_stress \
            mmap_race mmap_race_worker vma_overlap vma_isolation \
            vma_isolation_worker munmap_partial \
            elf_validation elf_permtest \
            sbrk_shrink_test sbrk_shrink calloc_overflow \
            shell ls cat \
            echo touch wc head grep tee sort \
            segvtest segvtest_stack segvtest_nx segvtest_mmap_ro segvtest_kernel \
            win_blit_fault

UTIL_APPS = mkdir rm cp pwd mv kill

STANDALONE_APPS = syscall_linux_test

SPECIAL_APPS = elf_rotest hello_musl

APPS = $(CORE_APPS) $(UTIL_APPS) $(STANDALONE_APPS) $(SPECIAL_APPS)

# --- Objetos de la libc propia ---------------------------------------------
LIB_OBJS = crt0.o syscall.o \
           lib/string.o lib/malloc.o lib/file.o lib/ipc.o lib/process.o

# ============================================================================
# Reglas
# ============================================================================

all: $(APPS)

# --- Librería común ---------------------------------------------------------
crt0.o: crt0.asm
	$(AS) -f elf64 $< -o $@

syscall.o: syscall.c syscall.h
	$(CC) $(CFLAGS) -c $< -o $@

lib/%.o: lib/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# --- Regla de patrón para todas las apps "normales" ------------------------
# Cubre CORE_APPS y UTIL_APPS: mismo layout apps/<name>/main.c.
$(CORE_APPS) $(UTIL_APPS): %: apps/%/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c $< -o $@.o
	$(LD) -o $@ $(LIB_OBJS) $@.o $(LDFLAGS)
	@mkdir -p $(SYSDIR)
	cp $@ $(SYSDIR)/

# --- Apps standalone (sin LIB_OBJS, con su propio _start) ------------------
syscall_linux_test: apps/syscall_linux_test/main.c
	$(CC) $(CFLAGS) -c $< -o $@.o
	$(LD) -o $@ $@.o $(LDFLAGS)
	@mkdir -p $(SYSDIR)
	cp $@ $(SYSDIR)/

# --- elf_rotest: link.ld propio --------------------------------------------
elf_rotest: apps/elf_rotest/main.c apps/elf_rotest/link.ld $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/elf_rotest/main.c -o $@.o
	$(LD) -o $@ $(LIB_OBJS) $@.o -T apps/elf_rotest/link.ld $(LDFLAGS)
	@mkdir -p $(SYSDIR)
	cp $@ $(SYSDIR)/

# --- hello_musl: toolchain musl --------------------------------------------
# Reglas:
#   -static -no-pie   → ELF ET_EXEC, sin PT_INTERP, sin relocs dinámicas.
#   -fno-stack-protector → musl no tiene __stack_chk_fail en static sin SSP.
#   Sin LIB_OBJS: musl trae su propio crt1.o + libc.a + link script.
#
# Si el toolchain no está descargado, `make all` no debe romperse:
# imprimimos un SKIP y salimos con éxito. Para forzar el error, usar
# `make hello_musl-strict`.
hello_musl: apps/hello_musl/main.c
	@if [ ! -x "$(MUSL_CC)" ]; then \
		echo "SKIP hello_musl: falta $(MUSL_CC)"; \
		echo "  Descarga el toolchain:"; \
		echo "    mkdir -p ../toolchain && cd ../toolchain"; \
		echo "    wget https://musl.cc/x86_64-linux-musl-cross.tgz"; \
		echo "    tar xzf x86_64-linux-musl-cross.tgz"; \
		exit 0; \
	fi
	@mkdir -p $(SYSDIR)
	$(MUSL_CC) -static -no-pie -O2 -fno-stack-protector \
		-o $@ apps/hello_musl/main.c
	cp $@ $(SYSDIR)/
	@echo "==> OK: $(SYSDIR)/hello_musl"
	@file $(SYSDIR)/hello_musl

hello_musl-strict: apps/hello_musl/main.c
	@test -x "$(MUSL_CC)" || { \
		echo "ERROR: toolchain musl no encontrado en $(MUSL_CC)"; exit 1; }
	@mkdir -p $(SYSDIR)
	$(MUSL_CC) -static -no-pie -O2 -fno-stack-protector \
		-o hello_musl apps/hello_musl/main.c
	cp hello_musl $(SYSDIR)/
	@file $(SYSDIR)/hello_musl

# --- Limpieza --------------------------------------------------------------
clean:
	rm -f *.o lib/*.o
	rm -f $(APPS)
	rm -f $(addprefix $(SYSDIR)/,$(APPS))

# mkdir/rm/... son también comandos del sistema: aunque no colisionan
# con ficheros del árbol, forzamos a make a no buscarlos en PATH.
.PHONY: all clean hello_musl hello_musl-strict \
        mkdir rm cp pwd mv kill