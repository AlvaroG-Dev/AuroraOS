# Makefile global - Aurora OS

.PHONY: all bootloader kernel user user-native user-musl stage-clean prebuilt image \
        run run-debug run-smp run-smp-debug \
        run-smp-kvm run-smp-kvm-debug clean sysroot initrd.tar iso run-iso \
        run-smp-kvm-ahci run-smp-kvm-ahci-debug run-kvm-ahci \
        run-smp-kvm-ahci-3disk

all: image

sysroot:
	@mkdir -p sysroot/system/icons sysroot/system/wallpapers
	@mkdir -p sysroot/etc
	@if [ ! -f sysroot/system/config.txt ]; then \
		echo "Aurora OS v0.1.0 Initramfs Config" > sysroot/system/config.txt; \
	fi
	@if [ -d assets ]; then \
		echo "[Makefile] Copiando assets a sysroot..."; \
		cp -r assets/* sysroot/ 2>/dev/null || true; \
	fi
	@if [ -d etc ]; then \
		echo "[Makefile] Copiando etc/ a sysroot/etc/..."; \
		cp -f etc/passwd etc/group etc/shadow etc/hosts etc/resolv.conf sysroot/etc/ 2>/dev/null || true; \
	fi

bootloader:
	$(MAKE) -C bootloader install

# ---------------------------------------------------------------------------
# Userland: nativo (user/) + musl (user/musl/).
#
# user/ contiene solo programas nativos específicos de Aurora y tests.
# Las utilidades POSIX generales las proporciona BusyBox; los programas
# POSIX adicionales propios se compilan con musl en user/musl/. Ambos usan
# el mismo staging centralizado y colocan ejecutables en /bin.
# ---------------------------------------------------------------------------
user: user-native user-musl

user-native: stage-clean
	$(MAKE) -C user all

user-musl: user-native
	@if [ -f user/musl/Makefile ]; then \
		echo "[Makefile] Build musl userland..."; \
		$(MAKE) -C user/musl all; \
	else \
		echo "[Makefile] user/musl/Makefile no existe, saltando"; \
	fi

stage-clean:
	@rm -rf sysroot/bin sysroot/sbin sysroot/usr/bin sysroot/usr/sbin
	@mkdir -p sysroot/bin sysroot/sbin sysroot/usr/bin sysroot/usr/sbin

# ---------------------------------------------------------------------------
# Prebuilt Linux userland.
#
# Executables placed under prebuilt/ubuntu/bin and prebuilt/ubuntu/usr/bin are
# installed without recompilation. ELF dependencies are resolved through the
# central lib/ pool by scripts/stage_elf.py.
# ---------------------------------------------------------------------------
prebuilt: user
	@if [ -d prebuilt/ubuntu ]; then \
		for dir in bin usr/bin; do \
			if [ -d prebuilt/ubuntu/$$dir ]; then \
				find prebuilt/ubuntu/$$dir -type f -perm -111 -exec \
					python3 scripts/stage_elf.py {} sysroot /$$dir \; ; \
			fi; \
		done; \
	fi
# ---------------------------------------------------------------------------
# ELF malformado para probar p_offset + p_filesz fuera del archivo.
# ---------------------------------------------------------------------------
elf_malformed: user
	@mkdir -p sysroot/bin
	@if [ -f sysroot/bin/filetest ]; then \
		cp sysroot/bin/filetest sysroot/bin/elf_malformed; \
		printf '\\377\\377\\377\\377\\377\\377\\377\\377' | \
			dd of=sysroot/bin/elf_malformed bs=1 seek=96 count=8 \
			conv=notrunc status=none; \
	fi

# ---------------------------------------------------------------------------
# initrd.tar: construye el tarfs desde sysroot/.
#
# Recopila en un único sitio:
#   - BusyBox (binarios + applets) desde third_party/busybox-*/_install/
#   - tcc (JIT compiler) desde tcc_stage/ si existe
#   - ld-musl + headers de musl desde tcc_stage/lib/ y tcc_stage/usr/include/
#   - tcc runtime (runmain.o, libtcc1.a, include/) desde tcc_stage/usr/lib/tcc/
#   - libtcc.so desde tcc_stage/usr/lib/ (opcional, tcc static-pie no lo usa)
#   - /root/hello.c de ejemplo para tcc -run
#
# Userland executables are staged by user/Makefile and user/musl/Makefile
# directly into sysroot/bin. This target only assembles runtime components.
# ---------------------------------------------------------------------------
initrd.tar: sysroot user prebuilt elf_malformed
	@echo "[Makefile] Preparando estructura de sysroot/..."
	@mkdir -p sysroot/bin sysroot/sbin \
	          sysroot/usr/bin sysroot/usr/sbin \
	          sysroot/usr/lib sysroot/usr/include \
	          sysroot/data sysroot/lib sysroot/lib64 sysroot/root
	@echo "[Makefile] Copiando busybox al tarfs..."
	@if [ -f third_party/busybox-1.36.1/_install/bin/busybox ]; then \
		cp -a -n third_party/busybox-1.36.1/_install/bin/. sysroot/bin/; \
		cp -a -n third_party/busybox-1.36.1/_install/usr/. sysroot/usr/; \
		if [ -d third_party/busybox-1.36.1/_install/sbin ]; then \
			cp -a -n third_party/busybox-1.36.1/_install/sbin/. sysroot/sbin/; \
		fi; \
	fi
	@echo "[Makefile] Copiando tcc (JIT) desde tcc_stage/ si existe..."
	@if [ -d tcc_stage ]; then \
		if [ -f tcc_stage/usr/bin/tcc ]; then \
			mkdir -p sysroot/usr/bin; \
			cp -f tcc_stage/usr/bin/tcc sysroot/usr/bin/tcc; \
			echo "[Makefile]   tcc -> sysroot/usr/bin/tcc"; \
		fi; \
		if [ -f tcc_stage/usr/lib/libtcc.so ]; then \
			mkdir -p sysroot/usr/lib; \
			cp -f tcc_stage/usr/lib/libtcc.so sysroot/usr/lib/libtcc.so; \
			echo "[Makefile]   libtcc.so -> sysroot/usr/lib/libtcc.so"; \
		fi; \
	else \
		echo "[Makefile]   (tcc_stage/ no existe, saltando tcc)"; \
	fi
	@echo "[Makefile] Copiando runtime de tcc (runmain.o, libtcc1.a, include)..."
	@if [ -d tcc_stage/usr/lib/tcc ]; then \
		mkdir -p sysroot/usr/lib/tcc; \
		cp -r tcc_stage/usr/lib/tcc/. sysroot/usr/lib/tcc/; \
		echo "[Makefile]   tcc runtime -> sysroot/usr/lib/tcc/"; \
	fi
	@echo "[Makefile] Copiando libc.a y stubs para tcc..."
	@if [ -f tcc_stage/usr/lib/libc.a ]; then \
		mkdir -p sysroot/usr/lib; \
		cp -f tcc_stage/usr/lib/libc.a sysroot/usr/lib/libc.a; \
		echo "[Makefile]   libc.a -> sysroot/usr/lib/"; \
		for stub in libm.a libpthread.a libdl.a librt.a libcrypt.a \
		            libresolv.a libutil.a libxnet.a libssp_nonshared.a; do \
			if [ -f tcc_stage/usr/lib/$stub ]; then \
				cp -f tcc_stage/usr/lib/$stub sysroot/usr/lib/$stub; \
			fi; \
		done; \
	else \
		echo "[Makefile]   (tcc_stage/usr/lib/libc.a no existe, tcc no podra linkar)"; \
	fi
	@echo "[Makefile] Copiando dynamic linker de musl a sysroot/lib/..."
	@if [ -f tcc_stage/lib/ld-musl-x86_64.so.1 ]; then \
		cp -f tcc_stage/lib/ld-musl-x86_64.so.1 sysroot/lib/ld-musl-x86_64.so.1; \
		echo "[Makefile]   ld-musl desde tcc_stage/ (compilado en local)"; \
		if [ -f tcc_stage/lib/libc.so ]; then \
			cp -f tcc_stage/lib/libc.so sysroot/lib/libc.so; \
		fi; \
	else \
		MUSL_LIB=toolchain/x86_64-linux-musl-cross/x86_64-linux-musl/lib; \
		if [ -f $$MUSL_LIB/libc.so ]; then \
			cp -f $$MUSL_LIB/libc.so sysroot/lib/ld-musl-x86_64.so.1; \
			cp -f $$MUSL_LIB/libc.so sysroot/lib/libc.so; \
			echo "[Makefile]   ld-musl desde toolchain (fallback)"; \
		else \
			echo "[Makefile] WARN: no hay musl en tcc_stage/lib/ ni en el toolchain"; \
		fi; \
	fi
	@echo "[Makefile] Copiando headers de musl para tcc..."
	@if [ -d tcc_stage/usr/include ]; then \
		mkdir -p sysroot/usr/include; \
		cp -r tcc_stage/usr/include/. sysroot/usr/include/; \
		echo "[Makefile]   headers -> sysroot/usr/include/"; \
	else \
		echo "[Makefile]   (tcc_stage/usr/include no existe, tcc no podra compilar)"; \
	fi
	@echo "[Makefile] Creando /root/hello.c para tcc -run (si no existe)..."
	@if [ ! -f sysroot/root/hello.c ]; then \
		printf '#include <stdio.h>\n\nint main(void) {\n    printf("hola desde codigo JIT\\n");\n    for (int i = 0; i < 5; i++)\n        printf("  iter %%d\\n", i);\n    return 0;\n}\n' > sysroot/root/hello.c; \
		echo "[Makefile]   /root/hello.c creado"; \
	else \
		echo "[Makefile]   /root/hello.c ya existe, no se toca"; \
	fi
	@echo "[Makefile] Generando initrd.tar desde sysroot/"
	tar --format=ustar --owner=0 --group=0 -cf kernel/initrd.tar -C sysroot .

kernel: initrd.tar
	$(MAKE) -C kernel

image: bootloader kernel user
	mkdir -p esp/EFI/BOOT esp/etc
	cp bootloader/BOOTX64.EFI esp/EFI/BOOT/
	cp kernel/kernel.elf esp/
	cp bootloader/aurora.conf esp/etc/aurora.conf
	dd if=/dev/zero of=aurora.img bs=1M count=64
	mkfs.fat -F 32 aurora.img
	mcopy -i aurora.img -s esp/EFI ::
	mcopy -i aurora.img -s esp/kernel.elf ::
	mcopy -i aurora.img -s esp/etc ::

# ---------------------------------------------------------------------------
# QEMU flags
# ---------------------------------------------------------------------------
QEMU_FLAGS_COMMON = \
	-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
	-drive if=pflash,format=raw,file=OVMF_VARS.fd \
	-m 512M \
	-no-reboot -no-shutdown

QEMU_FLAGS_DEBUG = \
	-debugcon file:debug.log \
	-d int,cpu_reset \
	-D qemu_debug.log \
	-s -S

QEMU_FLAGS_KVM = \
	-enable-kvm \
	-cpu host

QEMU_STORAGE_PC = \
	-drive if=none,id=disk0,format=raw,file=aurora.img \
	-device ide-hd,drive=disk0,bus=ide.0,unit=0 \
	-drive if=none,id=cdrom0,format=raw,file=aurora.iso \
	-device ide-cd,drive=cdrom0,bus=ide.1,unit=0 \
	-drive if=none,id=testdisk,format=raw,file=tests/test_disk.img \
	-device ide-hd,drive=testdisk,bus=ide.1,unit=1

QEMU_STORAGE_Q35 = \
	-drive if=none,id=disk0,format=raw,file=aurora.img \
	-device ide-hd,drive=disk0,bus=ide.0 \
	-drive if=none,id=testdisk,format=raw,file=tests/test_disk.img \
	-device ide-hd,drive=testdisk,bus=ide.1 \
	-drive if=none,id=cdrom0,format=raw,file=aurora.iso \
	-device ide-cd,drive=cdrom0,bus=ide.2

QEMU_FLAGS_Q35_AHCI = \
	-machine q35 \
	-device ahci,id=ahci \
	-drive if=none,id=disk0,format=raw,file=aurora.img \
	-device ide-hd,drive=disk0,bus=ahci.0

# ---------------------------------------------------------------------------
# TCG
# ---------------------------------------------------------------------------
run: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_STORAGE_PC) \
		-cpu max \
		-serial stdio

run-debug: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_STORAGE_PC) \
		$(QEMU_FLAGS_DEBUG) \
		-cpu max \
		-serial file:serial.log

run-smp: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_STORAGE_PC) \
		-cpu max \
		-smp 4 \
		-serial stdio

run-smp-debug: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_STORAGE_PC) \
		$(QEMU_FLAGS_DEBUG) \
		-cpu max \
		-smp 4 \
		-serial file:serial.log
	@echo ""
	@echo "=== Trampoline (debug.log) ==="
	@cat debug.log 2>/dev/null || echo "(vacío)"
	@echo ""
	@echo "=== Últimas líneas de serial.log ==="
	@tail -40 serial.log 2>/dev/null || echo "(vacío)"

# ---------------------------------------------------------------------------
# KVM
# ---------------------------------------------------------------------------
run-smp-kvm: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_STORAGE_PC) \
		$(QEMU_FLAGS_KVM) \
		-smp 8 \
		-serial stdio

run-smp-kvm-debug: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_STORAGE_PC) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_FLAGS_DEBUG) \
		-smp 8 \
		-serial file:serial.log
	@echo ""
	@echo "=== Trampoline (debug.log) ==="
	@cat debug.log 2>/dev/null || echo "(vacío)"
	@echo ""
	@echo "=== Últimas líneas de serial.log ==="
	@tail -60 serial.log 2>/dev/null || echo "(vacío)"

run-smp-kvm-ahci: iso tests/test_disk.img
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_STORAGE_Q35) \
		-smp 4 \
		-serial stdio

run-smp-kvm-ahci-debug: iso tests/test_disk.img
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_FLAGS_DEBUG) \
		$(QEMU_STORAGE_Q35) \
		-smp 4 \
		-serial file:serial.log
	@echo ""
	@echo "=== Trampoline (debug.log) ==="
	@cat debug.log 2>/dev/null || echo "(vacío)"
	@echo ""
	@echo "=== Últimas líneas de serial.log ==="
	@tail -80 serial.log 2>/dev/null || echo "(vacío)"

run-kvm-ahci: iso tests/test_disk.img
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_STORAGE_Q35) \
		-serial stdio

run-smp-kvm-ahci-3disk: iso tests/test_disk.img tests/extra_disk.img
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_STORAGE_Q35) \
		-drive if=none,id=disk2,format=raw,file=tests/extra_disk.img \
		-device ide-hd,drive=disk2,bus=ide.3 \
		-smp 4 \
		-serial stdio

# ---------------------------------------------------------------------------
# ISO
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
	@echo "[Makefile] ISO regenerada: aurora.iso"

run-iso: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		-smp 4 \
		-cdrom aurora.iso \
		-serial stdio

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------
clean:
	$(MAKE) -C bootloader clean
	$(MAKE) -C kernel clean
	$(MAKE) -C user clean
	@if [ -f user/musl/Makefile ]; then $(MAKE) -C user/musl clean; fi
	rm -rf sysroot kernel/initrd.tar aurora.img aurora.iso esp
	rm -f serial.log debug.log qemu_debug.log