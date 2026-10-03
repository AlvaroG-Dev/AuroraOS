# bootloader/Makefile
# Bootloader UEFI de Aurora OS — versión sin gnu-efi.
# Compilado con clang + lld-link, target Windows MSVC (MS ABI nativa UEFI).

TARGET  = BOOTX64.EFI
SRC     = efi_main.c

CC      = clang
LD      = lld-link

CLANG_TARGET = x86_64-pc-windows-msvc

# -ffreestanding:      sin libc
# -fno-stack-protector: sin __stack_chk_*
# -fshort-wchar:        wchar_t = 16 bits (CHAR16 UEFI)
# -mno-red-zone:        sin red zone
# -mno-stack-arg-probe: sin llamada a __chkstk
CFLAGS = -target $(CLANG_TARGET) \
         -ffreestanding -fno-stack-protector -fshort-wchar \
         -mno-red-zone -mno-stack-arg-probe \
         -Wall -Wextra \
         -g0 -O2

# -subsystem:efi_application: Subsystem = 10 (EFI Application)
# -entry:efi_main:            punto de entrada
# -nodefaultlib:              sin CRT
# -dll:                       formato PE/DLL (es lo que UEFI espera)
# -filealign:512:             alineación de fichero que OVMF acepta
LDFLAGS = -subsystem:efi_application \
          -entry:efi_main \
          -nodefaultlib \
          -dll \
          -filealign:512

all: $(TARGET)

$(TARGET): $(SRC) efi_min.h
	@echo "  [clang]    target  = $(CLANG_TARGET)"
	@echo "  [lld-link] subsys  = efi_application"
	$(CC) $(CFLAGS) -c $(SRC) -o efi_main.o
	$(LD) $(LDFLAGS) efi_main.o -out:$(TARGET)
	rm -f efi_main.o

install: $(TARGET)
	mkdir -p ../esp/EFI/BOOT
	cp $(TARGET) ../esp/EFI/BOOT/BOOTX64.EFI

clean:
	rm -f $(TARGET) *.o

.PHONY: all install clean