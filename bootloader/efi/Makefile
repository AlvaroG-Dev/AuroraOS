# bootloader/efi/Makefile
# Bootloader UEFI de Aurora OS. clang + lld-link, target Windows MSVC
# (MS ABI nativa UEFI).

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
# -I../include:         kernel_boot_info.h compartida
CFLAGS = -target $(CLANG_TARGET) \
         -ffreestanding -fno-stack-protector -fshort-wchar \
         -mno-red-zone -mno-stack-arg-probe \
         -Wall -Wextra -I../include \
         -g0 -O2

LDFLAGS = -subsystem:efi_application \
          -entry:efi_main \
          -nodefaultlib \
          -dll \
          -filealign:512

.PHONY: all install clean

all: $(TARGET)

$(TARGET): $(SRC) efi_min.h ../include/kernel_boot_info.h
	@echo "  [clang]    target  = $(CLANG_TARGET)"
	@echo "  [lld-link] subsys  = efi_application"
	$(CC) $(CFLAGS) -c $(SRC) -o efi_main.o
	$(LD) $(LDFLAGS) efi_main.o -out:$(TARGET)
	rm -f efi_main.o

install: $(TARGET)
	@echo "  [efi] $(TARGET) listo (no copia; lo hace el Makefile global)"

clean:
	rm -f $(TARGET) *.o