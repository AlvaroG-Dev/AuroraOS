# Makefile global - Aurora OS

.PHONY: all bootloader kernel user user-native user-musl image \
        run run-debug run-smp run-smp-debug \
        run-smp-kvm run-smp-kvm-debug clean sysroot initrd.tar iso run-iso \
        run-smp-kvm-ahci run-smp-kvm-ahci-debug run-kvm-ahci \
        run-smp-kvm-ahci-3disk

all: image

sysroot:
	@mkdir -p sysroot/system/icons sysroot/system/wallpapers
	@mkdir -p sysroot/etc
	@mkdir -p sysroot/usr/share sysroot/usr/games
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
# ---------------------------------------------------------------------------
user: user-native user-musl

user-native:
	$(MAKE) -C user all

user-musl:
	@if [ -f user/musl/Makefile ]; then \
		echo "[Makefile] Build musl userland..."; \
		$(MAKE) -C user/musl all; \
	else \
		echo "[Makefile] user/musl/Makefile no existe, saltando"; \
	fi

# ---------------------------------------------------------------------------
# ELF malformado para probar p_offset + p_filesz fuera del archivo.
#
# Parte de filetest (que user/Makefile ya deja en sysroot/usr/bin/).
# ---------------------------------------------------------------------------
elf_malformed: user
	@if [ -f sysroot/usr/bin/filetest ]; then \
		cp sysroot/usr/bin/filetest sysroot/usr/bin/elf_malformed; \
		printf '\\377\\377\\377\\377\\377\\377\\377\\377' | \
			dd of=sysroot/usr/bin/elf_malformed bs=1 seek=96 count=8 \
			conv=notrunc status=none; \
	fi

# ---------------------------------------------------------------------------
# initrd.tar: construye el tarfs desde sysroot/.
# ---------------------------------------------------------------------------
initrd.tar: sysroot user elf_malformed
	@echo "[Makefile] Preparando estructura de sysroot/..."
	@mkdir -p sysroot/bin sysroot/sbin \
	          sysroot/usr/bin sysroot/usr/sbin \
	          sysroot/lib sysroot/lib64 \
	          sysroot/usr/lib sysroot/usr/lib64 \
	          sysroot/lib/x86_64-linux-gnu \
	          sysroot/usr/lib/x86_64-linux-gnu \
	          sysroot/usr/include \
	          sysroot/usr/share sysroot/usr/games \
	          sysroot/data sysroot/root

# ---------------------------------------------------------------------------
# 1. (Sin busybox — userspace es 100% glibc/Ubuntu)
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 2. tcc + musl + ncurses + terminfo (source-build → build/tcc_stage/)
# ---------------------------------------------------------------------------
	@echo "[Makefile] Copiando build/tcc_stage/ (tcc + musl + ncurses)..."
	@if [ -d build/tcc_stage ]; then \
		[ -f build/tcc_stage/usr/bin/tcc ] && cp -f build/tcc_stage/usr/bin/tcc sysroot/usr/bin/ || true; \
		[ -f build/tcc_stage/usr/lib/libtcc.so ] && cp -f build/tcc_stage/usr/lib/libtcc.so sysroot/usr/lib/ || true; \
		[ -d build/tcc_stage/usr/lib/tcc ] && cp -r build/tcc_stage/usr/lib/tcc/. sysroot/usr/lib/tcc/ || true; \
		[ -d build/tcc_stage/usr/include ] && cp -r build/tcc_stage/usr/include/. sysroot/usr/include/ || true; \
		[ -d build/tcc_stage/usr/share/terminfo ] && cp -r build/tcc_stage/usr/share/terminfo sysroot/usr/share/ || true; \
		for lib in libncursesw.a libtinfow.a libformw.a libmenuw.a libpanelw.a; do \
			[ -f build/tcc_stage/usr/lib/$$lib ] && cp -f build/tcc_stage/usr/lib/$$lib sysroot/usr/lib/ || true; \
		done; \
	fi
	@echo "[Makefile] Copiando libc.a + ld-musl para tcc..."
	@if [ -f build/tcc_stage/usr/lib/libc.a ]; then \
		cp -f build/tcc_stage/usr/lib/libc.a sysroot/usr/lib/; \
		cp -f build/tcc_stage/usr/lib/libc.a sysroot/lib/; \
		for stub in libm.a libpthread.a libdl.a librt.a libcrypt.a \
		            libresolv.a libutil.a libxnet.a libssp_nonshared.a; do \
			[ -f build/tcc_stage/usr/lib/$$stub ] && cp -f build/tcc_stage/usr/lib/$$stub sysroot/usr/lib/ || true; \
		done; \
	fi
	@if [ -f build/tcc_stage/lib/ld-musl-x86_64.so.1 ]; then \
		cp -f build/tcc_stage/lib/ld-musl-x86_64.so.1 sysroot/lib/; \
		[ -f build/tcc_stage/lib/libc.so ] && cp -f build/tcc_stage/lib/libc.so sysroot/lib/ || true; \
	fi

# ---------------------------------------------------------------------------
# 3. frotz + Zork I (source-build en third_party/)
# ---------------------------------------------------------------------------
	@echo "[Makefile] Copiando frotz + Zork I..."
	@[ -f third_party/frotz/frotz ] && cp -f third_party/frotz/frotz sysroot/usr/bin/ || true
	@[ -f third_party/zork1.z3 ] && cp -f third_party/zork1.z3 sysroot/usr/games/ || true

# ---------------------------------------------------------------------------
# 4. libs/ — runtime de otros OS (glibc del host, etc.)
# ---------------------------------------------------------------------------
	@echo "[Makefile] Copiando libs/ a sysroot/..."
	@if [ -d libs ]; then \
		find libs -mindepth 1 -maxdepth 1 -type d \
			! -name 'glibc' -exec cp -a {} sysroot/ \; ; \
		if [ -d libs/glibc ]; then \
			for d in lib lib64; do \
				[ -d libs/glibc/$$d ] || continue; \
				cp -a libs/glibc/$$d/. sysroot/$$d/; \
				cp -a libs/glibc/$$d/. sysroot/usr/$$d/; \
			done; \
			echo "[Makefile]   libs glibc -> sysroot/{lib,lib64} y sysroot/usr/{lib,lib64}"; \
		fi; \
	else \
		echo "[Makefile]   (libs/ no existe)"; \
	fi

# ---------------------------------------------------------------------------
# 5. precompiled/ — binarios de otros OS
# ---------------------------------------------------------------------------
	@echo "[Makefile] Copiando precompiled/ a sysroot/..."
	@if [ -d precompiled ]; then \
		cp -a precompiled/. sysroot/; \
	else \
		echo "[Makefile]   (precompiled/ no existe)"; \
	fi

# ---------------------------------------------------------------------------
# /bin/sh: los programas de Ubuntu hacen execve("/bin/sh") en muchos sitios.
# Sin usrmerge, /bin es un directorio real. Ponemos dash ahí.
# ---------------------------------------------------------------------------
	@echo "[Makefile] Instalando /bin/sh (dash)..."
	@if [ -f sysroot/usr/bin/bash ]; then \
		cp -f sysroot/usr/bin/bash sysroot/bin/bash; \
		rm -f sysroot/bin/sh sysroot/usr/bin/sh; \
		ln -sf bash sysroot/bin/sh; \
		ln -sf bash sysroot/usr/bin/sh; \
	fi

# ---------------------------------------------------------------------------
# [bash] Ficheros de config que bash espera. Sin ellos, bash sale con
# exit(1) al no poder leer /etc/profile ni /root/.bashrc.
# ---------------------------------------------------------------------------
	@echo "[Makefile] Creando config de bash..."
	@mkdir -p sysroot/etc sysroot/root
	@[ -f sysroot/etc/passwd ] || printf 'root:x:0:0:root:/root:/bin/sh\n' > sysroot/etc/passwd
	@[ -f sysroot/etc/group ]  || printf 'root:x:0:\n' > sysroot/etc/group
	@[ -f sysroot/etc/profile ] || printf '# /etc/profile (empty)\n' > sysroot/etc/profile
	@[ -f sysroot/root/.bashrc ] || printf '# ~/.bashrc (empty)\n' > sysroot/root/.bashrc
	@[ -f sysroot/root/.profile ] || printf '# ~/.profile (empty)\n' > sysroot/root/.profile
	
# ---------------------------------------------------------------------------
# 6. /root/hello.c para tcc -run
# ---------------------------------------------------------------------------
	@if [ ! -f sysroot/root/hello.c ]; then \
		printf '#include <stdio.h>\n\nint main(void) {\n    printf("hola desde codigo JIT\\n");\n    for (int i = 0; i < 5; i++)\n        printf("  iter %%d\\n", i);\n    return 0;\n}\n' > sysroot/root/hello.c; \
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