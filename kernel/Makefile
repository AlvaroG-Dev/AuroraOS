# kernel/Makefile
# Detecta automaticamente el toolchain disponible

AS      = nasm

CROSS := $(shell which x86_64-elf-gcc 2>/dev/null)
ifeq ($(CROSS),)
    CC = gcc
    CFLAGS = -ffreestanding -O2 -Wall -Wextra -Werror \
             -fno-exceptions -mno-red-zone \
             -mcmodel=kernel -fno-stack-protector \
             -fno-asynchronous-unwind-tables -fno-pic \
             -nostdlib -nostartfiles -nodefaultlibs -I. \
             -fno-omit-frame-pointer -g -O0 \
             -Wno-unused-parameter -Wframe-larger-than=2048
else
    CC = x86_64-elf-gcc
    CFLAGS = -ffreestanding -O2 -Wall -Wextra -Werror \
             -fno-exceptions -mno-red-zone -mcmodel=kernel \
             -fno-stack-protector -I. \
             -fno-omit-frame-pointer -g -O0 \
             -Wno-unused-parameter -Wframe-larger-than=2048
endif

LD      = $(CC)
LDFLAGS = -nostdlib -T linker.ld -no-pie -Wl,--build-id=none

OBJS = boot.o isr_stubs.o switch.o syscall.o syscall_entry.o main.o string.o serial.o gdt.o idt.o paging.o pmm.o \
        buddy.o spinlock.o heap.o slab.o sched.o ps2.o rtc.o initrd.o tarfs.o vfs.o ipc.o bmp.o pci.o \
        process.o elf.o klog.o pf.o panic.o driver.o input.o wait.o tty.o test.o acpi.o apic.o time.o block.o ata_pio.o gfx/gfx.o gfx/window.o \
        ipi.o gfx/compositor.o gfx/taskbar.o gfx/font_aa.o gfx/font_manager.o gfx/winsrv.o \
        uaccess.o uaccess_asm.o uaccess_faults.o \
        tests/pmm_stress.o tests/kernel_tests.o \
        smp_trampoline.o smp_boot.o
TARGET  = kernel.elf

all: $(TARGET)

$(TARGET): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	$(AS) -f elf64 $< -o $@

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

clean:
	rm -f $(OBJS) $(TARGET) smp_trampoline.bin

.PHONY: all clean