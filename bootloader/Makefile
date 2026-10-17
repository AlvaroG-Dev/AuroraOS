# bootloader/Makefile
# Orquesta el build del bootloader UEFI (efi/) y BIOS (bios/).

.PHONY: all efi bios install clean

all: efi bios

efi:
	$(MAKE) -C efi

bios:
	$(MAKE) -C bios

# install sólo es informativo. El Makefile global coge los binarios
# directamente de efi/BOOTX64.EFI y bios/bios.img. No hay que copiar
# nada aquí porque el # Makefile global se encarga del layout del ESP.
install: efi bios
	@echo "  [bootloader] efi/BOOTX64.EFI  listo"
	@echo "  [bootloader] bios/bios.img    listo"

clean:
	$(MAKE) -C efi clean
	$(MAKE) -C bios clean