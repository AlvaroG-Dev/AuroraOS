# Makefile global - Aurora OS

.PHONY: all bootloader kernel image run run-debug clean sysroot

all: image

sysroot:
	@# 1. Crear la estructura base en sysroot
	@mkdir -p sysroot/system/icons sysroot/system/wallpapers
	@if [ ! -f sysroot/system/config.txt ]; then \
		echo "Aurora OS v0.1.0 Initramfs Config" > sysroot/system/config.txt; \
	fi
	@# 2. Copiar assets persistentes (icons, wallpapers, etc.) a sysroot si existe la carpeta
	@if [ -d assets ]; then \
		echo "[Makefile] Copiando assets a sysroot..."; \
		cp -r assets/* sysroot/ 2>/dev/null || true; \
	fi
	@# 3. Empaquetar carpetas e iconos en initrd.tar
	tar --format=ustar -cf kernel/initrd.tar -C sysroot .

bootloader:
	$(MAKE) -C bootloader install

kernel: sysroot
	$(MAKE) -C kernel

image: bootloader kernel
	mkdir -p esp/EFI/BOOT
	cp bootloader/BOOTX64.EFI esp/EFI/BOOT/
	cp kernel/kernel.elf esp/
	dd if=/dev/zero of=aurora.img bs=1M count=64
	mkfs.fat -F 32 aurora.img
	mcopy -i aurora.img -s esp/EFI ::
	mcopy -i aurora.img -s esp/kernel.elf ::

run: image
	@test -f OVMF_VARS.fd || cp /usr/share/OVMF/OVMF_VARS.fd . 2>/dev/null || echo "WARNING: OVMF_VARS.fd no encontrado"
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
		-drive if=pflash,format=raw,file=OVMF_VARS.fd \
		-drive format=raw,file=aurora.img \
		-serial stdio \
		-m 512M \
		-cpu qemu64 \
		-no-reboot -no-shutdown

clean:
	$(MAKE) -C bootloader clean
	$(MAKE) -C kernel clean
	rm -rf sysroot kernel/initrd.tar aurora.img esp