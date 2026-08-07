# bootloader/Makefile
# Compila el bootloader UEFI usando gcc + objcopy

TARGET  = BOOTX64.EFI
SRC     = efi_main.c

CC      = gcc
OBJCOPY = objcopy

# === Detectar rutas de gnu-efi ===
EFI_INC := $(shell pkg-config --cflags-only-I gnu-efi 2>/dev/null | sed 's/-I//')
ifeq ($(EFI_INC),)
    ifneq (,$(wildcard /usr/include/efi/efi.h))
        EFI_INC = /usr/include/efi
    else ifneq (,$(wildcard /usr/local/include/efi/efi.h))
        EFI_INC = /usr/local/include/efi
    endif
endif

EFI_ARCH_INC := $(EFI_INC)/x86_64

EFI_LIB := $(shell pkg-config --libs-only-L gnu-efi 2>/dev/null | sed 's/-L//')
ifeq ($(EFI_LIB),)
    ifneq (,$(wildcard /usr/lib/crt0-efi-x86_64.o))
        EFI_LIB = /usr/lib
    else ifneq (,$(wildcard /usr/lib64/crt0-efi-x86_64.o))
        EFI_LIB = /usr/lib64
    endif
endif

# === Flags ===
CFLAGS = -I$(EFI_INC) -I$(EFI_ARCH_INC) -I$(EFI_INC)/protocol \
    -ffreestanding -fno-stack-protector -fpic -fshort-wchar \
    -mno-red-zone -maccumulate-outgoing-args \
    -DEFI_FUNCTION_WRAPPER

LDFLAGS = -nostdlib -Wl,-T,$(EFI_LIB)/elf_x86_64_efi.lds \
    -Wl,-shared -Wl,-Bsymbolic -L$(EFI_LIB) \
    $(EFI_LIB)/crt0-efi-x86_64.o

LIBS = -lefi -lgnuefi

OBJCOPY_FLAGS = -j .text -j .sdata -j .data -j .dynamic -j .dynsym \
                -j .rel -j .rela -j .reloc \
                --target efi-app-x86_64

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@.so $< $(LIBS)
	$(OBJCOPY) $(OBJCOPY_FLAGS) $@.so $@
	rm -f $@.so

install: $(TARGET)
	mkdir -p ../esp/EFI/BOOT
	cp $(TARGET) ../esp/EFI/BOOT/BOOTX64.EFI

clean:
	rm -f $(TARGET) *.so

.PHONY: all install clean