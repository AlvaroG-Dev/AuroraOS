# kernel/Makefile
# Detecta automaticamente el toolchain disponible

AS      = nasm

CROSS := $(shell which x86_64-elf-gcc 2>/dev/null)
ifeq ($(CROSS),)
    CC = gcc
    CFLAGS = -ffreestanding -O2 -Wall -Wextra -fno-exceptions -mno-red-zone \
             -mcmodel=kernel -fno-stack-protector -fno-asynchronous-unwind-tables \
             -fno-pic -nostdlib -nostartfiles -nodefaultlibs -I. \
             -fno-omit-frame-pointer
else
    CC = x86_64-elf-gcc
    CFLAGS = -ffreestanding -O2 -Wall -Wextra -fno-exceptions -mno-red-zone \
             -mcmodel=kernel -fno-stack-protector -I. \
             -fno-omit-frame-pointer
endif

LD      = $(CC)
LDFLAGS = -nostdlib -T linker.ld -no-pie -Wl,--build-id=none

OBJS = boot.o isr_stubs.o switch.o syscall.o syscall_entry.o main.o string.o serial.o gdt.o idt.o paging.o pmm.o \
        buddy.o spinlock.o heap.o slab.o sched.o ps2.o rtc.o initrd.o tarfs.o vfs.o ipc.o bmp.o pci.o \
        process.o elf.o klog.o pf.o panic.o driver.o input.o wait.o tty.o test.o acpi.o apic.o time.o gfx/gfx.o gfx/window.o \
        gfx/compositor.o gfx/taskbar.o gfx/font_aa.o gfx/font_manager.o gfx/winsrv.o \
        tests/pmm_stress.o tests/kernel_tests.o
TARGET  = kernel.elf

all: $(TARGET)

$(TARGET): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	$(AS) -f elf64 $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean