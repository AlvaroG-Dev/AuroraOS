# kernel/Makefile
# Detecta automaticamente el toolchain disponible

AS      = nasm

# Detectar compilador cruzado o nativo
CROSS := $(shell which x86_64-elf-gcc 2>/dev/null)
ifeq ($(CROSS),)
    CC = gcc
    # Flags para compilar kernel con gcc nativo
    CFLAGS = -ffreestanding -O2 -Wall -Wextra -fno-exceptions -mno-red-zone \
             -mcmodel=kernel -fno-stack-protector -fno-asynchronous-unwind-tables \
             -nostdlib -nostartfiles -nodefaultlibs -I.
else
    CC = x86_64-elf-gcc
    CFLAGS = -ffreestanding -O2 -Wall -Wextra -fno-exceptions -mno-red-zone \
             -mcmodel=kernel -fno-stack-protector -I.
endif

LD      = $(CC)
LDFLAGS = -nostdlib -T linker.ld

OBJS = boot.o isr_stubs.o switch.o main.o serial.o gdt.o idt.o paging.o pmm.o heap.o sched.o ps2.o rtc.o initrd.o tarfs.o bmp.o \
       gfx/gfx.o gfx/window.o gfx/compositor.o gfx/taskbar.o gfx/font_aa.o gfx/font_manager.o
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
