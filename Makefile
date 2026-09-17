# Makefile global - Aurora OS

.PHONY: all bootloader kernel user image run run-debug clean sysroot initrd.tar iso

all: image

sysroot:
	@mkdir -p sysroot/system/icons sysroot/system/wallpapers
	@if [ ! -f sysroot/system/config.txt ]; then \
		echo "Aurora OS v0.1.0 Initramfs Config" > sysroot/system/config.txt; \
	fi
	@if [ -d assets ]; then \
		echo "[Makefile] Copiando assets a sysroot..."; \
		cp -r assets/* sysroot/ 2>/dev/null || true; \
	fi

bootloader:
	$(MAKE) -C bootloader install

user:
	$(MAKE) -C user all

initrd.tar: sysroot user
	@echo "[Makefile] Generando initrd.tar desde sysroot/"
	tar --format=ustar -cf kernel/initrd.tar -C sysroot .

kernel: initrd.tar
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
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=OVMF_VARS.fd \
		-drive format=raw,file=aurora.img \
		-serial stdio \
		-m 512M \
		-cpu qemu64 \
		-no-reboot -no-shutdown

iso: image
	@echo "[Makefile] Generando ISO UEFI/BIOS híbrida (aurora.iso)..."
	@mkdir -p iso_root
	@cp aurora.img iso_root/efi.img
	@xorriso -as mkisofs \
		-V "AURORA_OS" \
		-e efi.img \
		-no-emul-boot \
		-isohybrid-gpt-basdat \
		-isohybrid-apm-hfsplus \
		-o aurora.iso iso_root
	@rm -rf iso_root
	@echo "[Makefile] ISO regenerada para VirtualBox: aurora.iso"

run-iso: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=OVMF_VARS.fd \
		-cdrom aurora.iso \
		-serial stdio \
		-m 512M \
		-cpu qemu64 \
		-no-reboot -no-shutdown
		
debug: image
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=OVMF_VARS.fd \
		-drive format=raw,file=aurora.img \
		-d int \
		-m 512M \
		-cpu qemu64 \
		-no-reboot -no-shutdown

cpu_max: image
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=OVMF_VARS.fd \
		-drive format=raw,file=aurora.img \
		-serial stdio \
		-m 512M \
		-cpu max \
		-no-reboot -no-shutdown

clean:
	$(MAKE) -C bootloader clean
	$(MAKE) -C kernel clean
	$(MAKE) -C user clean
	rm -rf sysroot kernel/initrd.tar aurora.img esp
