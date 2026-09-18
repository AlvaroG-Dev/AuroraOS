# Makefile global - Aurora OS

.PHONY: all bootloader kernel user image run run-debug run-smp run-smp-debug \
        run-smp-kvm run-smp-kvm-debug clean sysroot initrd.tar iso run-iso

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

# ---------------------------------------------------------------------------
# QEMU targets
# ---------------------------------------------------------------------------
# Flags comunes para todos los targets.
#
# -no-reboot      : en triple fault, QEMU no reinicia
# -no-shutdown    : en triple fault, QEMU no cierra la ventana, la deja
#                   pausada para inspección
#
# Nota: NO uses `-debugcon stdio` junto con `-serial stdio` porque
# ambos escriben a stdout y se entremezclan.
# ---------------------------------------------------------------------------
QEMU_FLAGS_COMMON = \
	-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
	-drive if=pflash,format=raw,file=OVMF_VARS.fd \
	-m 512M \
	-no-reboot -no-shutdown

QEMU_FLAGS_DEBUG = \
	-debugcon file:debug.log \
	-d int,cpu_reset \
	-D qemu_debug.log

# Flags para KVM: -enable-kvm usa la aceleración, -cpu host expone la CPU
# real del host (con SMEP, SMAP, VT-x, etc.). Con esto, INIT-SIPI-SIPI
# funciona igual que en hardware real.
QEMU_FLAGS_KVM = \
	-enable-kvm \
	-cpu host

# ---------------------------------------------------------------------------
# TCG (emulación pura por software, sin KVM)
# ---------------------------------------------------------------------------

# 1 CPU, sin debug.
run: image
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		-cpu max \
		-drive format=raw,file=aurora.img \
		-serial stdio

# 1 CPU con debug.
run-debug: image
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		-cpu max \
		$(QEMU_FLAGS_DEBUG) \
		-drive format=raw,file=aurora.img \
		-serial file:serial.log

# 4 CPUs sin debug (TCG, no arrancará APs).
run-smp: image
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		-cpu max \
		-smp 4 \
		-drive format=raw,file=aurora.img \
		-serial stdio

# 4 CPUs con debug (TCG, no arrancará APs, solo para diagnóstico).
run-smp-debug: image
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		-cpu max \
		$(QEMU_FLAGS_DEBUG) \
		-smp 4 \
		-drive format=raw,file=aurora.img \
		-serial file:serial.log
	@echo ""
	@echo "=== Trampoline (debug.log) ==="
	@cat debug.log 2>/dev/null || echo "(vacío)"
	@echo ""
	@echo "=== Últimas líneas de serial.log ==="
	@tail -40 serial.log 2>/dev/null || echo "(vacío)"

# ---------------------------------------------------------------------------
# KVM (virtualización por hardware). ESTE es el que quieres para SMP.
# ---------------------------------------------------------------------------

# 4 CPUs con KVM. El que quieres para probar SMP de verdad.
run-smp-kvm: image
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		-smp 8 \
		-drive format=raw,file=aurora.img \
		-serial stdio

# 4 CPUs con KVM y debug. El que quieres para depurar SMP con logs.
run-smp-kvm-debug: image
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_FLAGS_DEBUG) \
		-smp 4 \
		-drive format=raw,file=aurora.img \
		-serial file:serial.log
	@echo ""
	@echo "=== Trampoline (debug.log) ==="
	@cat debug.log 2>/dev/null || echo "(vacío)"
	@echo ""
	@echo "=== Últimas líneas de serial.log ==="
	@tail -60 serial.log 2>/dev/null || echo "(vacío)"

# ---------------------------------------------------------------------------
# ISO (para VirtualBox, VMware, etc.)
# ---------------------------------------------------------------------------
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
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		-smp 4 \
		-cdrom aurora.iso \
		-serial stdio

clean:
	$(MAKE) -C bootloader clean
	$(MAKE) -C kernel clean
	$(MAKE) -C user clean
	rm -rf sysroot kernel/initrd.tar aurora.img esp
	rm -f serial.log debug.log qemu_debug.log