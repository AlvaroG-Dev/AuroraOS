# bootloader/bios/Makefile
AS       = nasm
CC       = gcc
LD       = ld
OBJCOPY  = objcopy

CFLAGS_32 = -m32 -march=i686 -mtune=generic \
            -ffreestanding -fno-stack-protector \
            -fno-pic -fno-pie -fno-asynchronous-unwind-tables \
            -fno-builtin -fno-omit-frame-pointer \
            -Wall -Wextra -O2 -g -gdwarf-4 \
            -I. -I../include \
            -fno-strict-aliasing -Wno-unused-parameter \
            -Wno-array-bounds -Wno-int-to-pointer-cast \
            -Wno-pointer-to-int-cast -Wno-overflow

LDFLAGS_32 = -m elf_i386 -T stage2_ld.ld --build-id=none \
             -nostdlib --no-undefined

OBJS = stage2.o int13_asm.o \
       bios_main.o string.o int13.o e820.o vbe.o vbe_asm.o \
       iso9660.o fat16.o elf_min.o paging.o bump.o config.o trampoline.o splash.o

TARGET      = bios.img
STAGE1      = stage1.bin
STAGE1_MBR  = stage1_mbr.bin
STAGE2      = stage2.bin
STAGE2_ELF  = stage2.elf
STAGE2_SECTORS ?= 64

.PHONY: all clean

all: $(TARGET) $(STAGE1_MBR)

$(TARGET): $(STAGE1) $(STAGE2)
	cat $(STAGE1) $(STAGE2) > $@
	truncate -s 65536 $@
	@echo "  [bios] $(TARGET) listo:"
	@ls -l $@

$(STAGE1): stage1.asm
	$(AS) -f bin $< -o $@
	@if [ $$(stat -c%s $@) -ne 512 ]; then \
		echo "  [bios] ERROR: stage1.bin != 512 bytes"; exit 1; \
	fi

$(STAGE1_MBR): stage1_mbr.asm $(STAGE2)
	@SIZE=$$(stat -c%s $(STAGE2)); \
	 MAX=$$(( $(STAGE2_SECTORS) * 512 )); \
	 if [ $$SIZE -gt $$MAX ]; then \
	     echo "  [bios] ERROR: stage2.bin ($$SIZE B) > STAGE2_SECTORS*512 ($$MAX B)"; \
	     echo "  [bios]        sube STAGE2_SECTORS en bootloader/bios/Makefile"; \
	     exit 1; \
	 fi; \
	 echo "  [bios] stage1_mbr: stage2=$$SIZE B, capacidad=$$MAX B"
	$(AS) -D STAGE2_SECTORS=$(STAGE2_SECTORS) -f bin $< -o $@
	@if [ $$(stat -c%s $@) -ne 512 ]; then \
		echo "  [bios] ERROR: stage1_mbr.bin != 512 bytes"; exit 1; \
	fi

$(STAGE2_ELF): $(OBJS) stage2_ld.ld
	$(LD) $(LDFLAGS_32) -o $@ $(OBJS)

$(STAGE2): $(STAGE2_ELF)
	$(OBJCOPY) -O binary $< $@

%.o: %.asm
	$(AS) -f elf32 -g -F dwarf $< -o $@

%.o: %.c
	$(CC) $(CFLAGS_32) -c $< -o $@

clean:
	rm -f $(OBJS) $(STAGE1) $(STAGE1_MBR) $(STAGE2) $(STAGE2_ELF) $(TARGET)