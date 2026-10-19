# bootloader/efi/Makefile
TARGET  = BOOTX64.EFI
SRCS    = efi_main.c splash.c
OBJS    = $(SRCS:.c=.o)
HDRS    = efi_min.h splash.h ../include/kernel_boot_info.h

CC      = clang
LD      = lld-link

CLANG_TARGET = x86_64-pc-windows-msvc

# Añadimos las rutas a los fonts del kernel
CFLAGS = -target $(CLANG_TARGET) \
         -ffreestanding -fno-stack-protector -fshort-wchar \
         -mno-red-zone -mno-stack-arg-probe \
         -Wall -Wextra -I../include -I. \
         -I../../kernel/gfx -I../../kernel/gfx/fonts \
         -g0 -O2

LDFLAGS = -subsystem:efi_application \
          -entry:efi_main \
          -nodefaultlib \
          -dll \
          -filealign:512

.PHONY: all install clean

all: $(TARGET)

$(TARGET): $(OBJS)
	@echo "  [lld-link] subsys  = efi_application"
	$(LD) $(LDFLAGS) $(OBJS) -out:$(TARGET)
	rm -f $(OBJS)

%.o: %.c $(HDRS)
	@echo "  [clang]    target  = $(CLANG_TARGET)  ($<)"
	$(CC) $(CFLAGS) -c $< -o $@

install: $(TARGET)
	@echo "  [efi] $(TARGET) listo (no copia; lo hace el Makefile global)"

clean:
	rm -f $(TARGET) *.o