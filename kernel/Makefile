# kernel/Makefile
# Detecta automaticamente el toolchain disponible

AS      = nasm

CROSS := $(shell which x86_64-elf-gcc 2>/dev/null)
ifeq ($(CROSS),)
    CC = gcc
    CFLAGS = -ffreestanding -O2 -Wall -Wextra \
             -fno-exceptions -mno-red-zone \
             -mcmodel=kernel -fno-stack-protector \
             -fno-asynchronous-unwind-tables -fno-pic \
             -nostdlib -nostartfiles -nodefaultlibs -I. \
             -fno-omit-frame-pointer -g -O0 \
             -march=x86-64 -mtune=generic \
             -mno-avx -mno-avx2 -mno-avx512f \
             -Wno-unused-parameter -Wframe-larger-than=2048
else
    CC = x86_64-elf-gcc
    CFLAGS = -ffreestanding -O2 -Wall -Wextra \
             -fno-exceptions -mno-red-zone -mcmodel=kernel \
             -fno-stack-protector -I. \
             -fno-omit-frame-pointer -g -O0 \
             -march=x86-64 -mtune=generic \
             -mno-avx -mno-avx2 -mno-avx512f \
             -Wno-unused-parameter -Wframe-larger-than=2048
endif

LD      = $(CC)
LDFLAGS = -nostdlib -T linker.ld -no-pie -Wl,--build-id=none

OBJS = boot.o isr_stubs.o switch.o syscall.o syscall_entry.o main.o string.o serial.o gdt.o idt.o paging.o pmm.o \
        buddy.o spinlock.o heap.o slab.o sched.o ps2.o rtc.o initrd.o tarfs.o vfs.o ipc.o bmp.o pci.o cpu.o simd.o \
        process.o elf.o klog.o pf.o panic.o driver.o input.o wait.o tty.o test.o acpi.o apic.o time.o block.o ata_pio.o \
        ata_common.o atapi.o ata_dma.o ahci.o completion.o part.o fat32.o pipe.o pty.o gfx/gfx.o gfx/window.o \
        ipi.o gfx/compositor.o gfx/taskbar.o gfx/font_aa.o gfx/font_manager.o gfx/winsrv.o gfx/gfx_filter.o \
        uaccess.o uaccess_asm.o uaccess_faults.o signal.o procfs.o futex.o swap.o sysctl.o epoll.o signalfd.o \
        memfd.o \
        sysfs.o tests/pmm_stress.o tests/kernel_tests.o \
        smp_trampoline.o smp_boot.o vdso.o vdso_blob.o tmpfs.o
TARGET  = kernel.elf

all: $(TARGET)

$(TARGET): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

linker.ld: linker.ld.in memlayout.h
	cpp -P -undef -D__LINKER__ -I. -x c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	$(AS) -f elf64 $< -o $@

initrd.o: initrd.asm initrd.tar

# ---------------------------------------------------------------------------
# Trampoline SMP: se ensambla a binario plano y se convierte a .o con
# objcopy. Los símbolos se renombran a smp_trampoline_{start,end,size}.
# ---------------------------------------------------------------------------
smp_trampoline.bin: smp_trampoline.asm
	$(AS) -f bin $< -o $@

smp_trampoline.o: smp_trampoline.bin
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	        --rename-section .data=.smp_trampoline,alloc,load,readonly,data,contents \
	        --redefine-sym _binary_smp_trampoline_bin_start=smp_trampoline_start \
	        --redefine-sym _binary_smp_trampoline_bin_end=smp_trampoline_end \
	        --redefine-sym _binary_smp_trampoline_bin_size=smp_trampoline_size \
	        $< $@

# ---------------------------------------------------------------------------
# vDSO: shared object embebido como blob.
#
# Se construye con el toolchain del host (gcc + ld + objcopy) y se
# embebe como blob (mismo truco que smp_trampoline.o). En runtime el
# kernel copia el blob a una pagina y la mapea en cada exec.
#
# El vDSO no tiene GOT/PLT/relocations: solo immediatos. Por eso puede
# vivir a VMA fija sin que el kernel tenga que aplicar parches.
# ---------------------------------------------------------------------------
VDSO_OBJ      = vdso/vdso.o
VDSO_SO       = vdso/vdso.so
VDSO_BLOB_OBJ = vdso_blob.o

$(VDSO_OBJ): vdso/vdso.S
	$(CC) -c -fno-pic -fno-stack-protector -mno-red-zone \
	      -ffreestanding -nostdlib -fno-asynchronous-unwind-tables \
	      $< -o $@

$(VDSO_SO): $(VDSO_OBJ) vdso/vdso.ld vdso/vdso.map
	ld -shared -T vdso/vdso.ld \
	   --version-script=vdso/vdso.map \
	   --hash-style=sysv \
	   --build-id=none -nostdlib \
	   -o $@ $(VDSO_OBJ)

# objcopy del .so ENTERO (no -O binary: queremos conservar el ELF
# header, program headers y secciones dinamicas).
$(VDSO_BLOB_OBJ): $(VDSO_SO)
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	        --rename-section .data=.vdso_blob,alloc,load,readonly,data,contents \
	        --redefine-sym _binary_vdso_vdso_so_start=vdso_blob_start \
	        --redefine-sym _binary_vdso_vdso_so_end=vdso_blob_end \
	        --redefine-sym _binary_vdso_vdso_so_size=vdso_blob_size \
	        $(VDSO_SO) $@

clean:
	rm -f $(OBJS) $(TARGET) smp_trampoline.bin linker.ld \
	      $(VDSO_OBJ) $(VDSO_SO) $(VDSO_BLOB_OBJ)

.PHONY: all clean