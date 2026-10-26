# Makefile global - Aurora OS



.PHONY: all bootloader bootloader-efi bootloader-bios \
        kernel user user-native user-musl image iso iso_root \
        normal run run-debug run-smp run-smp-debug \
        run-smp-kvm run-smp-kvm-debug run-smp-kvm-ahci run-smp-kvm-ahci-debug \
        run-kvm-ahci run-smp-kvm-ahci-3disk run-smp-kvm-ahci-tap run-smp-kvm-xhci run-smp-kvm-xhci-debug \
        run-iso run-iso-tcg run-iso-as-usb run-smp-kvm-xhci-storage run-smp-kvm-xhci-storage-debug \
        run-bios run-bios-debug run-bios-usb run-smp-kvm-xhci-hub run-smp-kvm-xhci-hub-debug \
        tap-setup tap-teardown tap-status run-smp-kvm-xhci-uas \
        python-stdlib env clean

# ---------------------------------------------------------------------------
# Preparacion del entorno (paquetes del host, toolchain musl, pila Wayland,
# iconos, discos de prueba...). scripts/setup-env.sh solo actua si falta algo,
# asi que en un build normal cuesta una comprobacion rapida.
#   make env              preparar a mano
#   SKIP_ENV=1 make ...   saltarse la comprobacion
#   WITH_MESA=1 make ...  ademas, bajar Mesa/LLVM (initrd +500 MB)
# ---------------------------------------------------------------------------
env:
	@bash scripts/setup-env.sh

RESOLV_CONF ?= nameserver 1.1.1.1\nnameserver 8.8.8.8\noptions timeout:2 attempts:2\n

# Drivers en Ring 3 (.aur) por defecto. Con =0 se usa el driver del kernel.
# Cada uno instala/borra el marcador /usr/lib/aur/<drv>.user en el initrd.
AHCI_USER ?= 1
XHCI_USER ?= 1
PS2_USER  ?= 1

# ---------------------------------------------------------------------------
# Rutas del bootloader e ISO.
# ---------------------------------------------------------------------------
BOOTX64_EFI     ?= bootloader/efi/BOOTX64.EFI
BIOS_IMG        ?= bootloader/bios/bios.img
KERNEL_ELF      ?= kernel/kernel.elf
AURORA_CONF     ?= bootloader/aurora.conf

EFI_IMG         ?= build/efi.img
ISO_ROOT        ?= build/iso_root
ISO_OUT         ?= aurora.iso

# ---------------------------------------------------------------------------
# all: por defecto genera la ISO completa.
# ---------------------------------------------------------------------------
all: iso

# Añade esto:
normal: iso

# ===========================================================================
# SYSROOT / USERLAND
# ===========================================================================

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

bootloader: bootloader-efi bootloader-bios

bootloader-efi: | env
	$(MAKE) -C bootloader/efi

bootloader-bios: | env
	$(MAKE) -C bootloader/bios

# [FIX] Reglas explícitas para que 'make' sepa cómo generar los archivos
# si los pide como dependencia (ej: desde $(EFI_IMG) o iso_root)
$(BOOTX64_EFI): | env
	$(MAKE) -C bootloader/efi

$(BIOS_IMG): | env
	$(MAKE) -C bootloader/bios

# Imagenes de disco de prueba: las crea scripts/setup-env.sh.
tests/test_disk.img tests/extra_disk.img: | env

# ---------------------------------------------------------------------------
# Userland: nativo (user/) + musl (user/musl/).
# ---------------------------------------------------------------------------
user: user-native user-musl

user-native: | env
	$(MAKE) -C user all

user-musl: | env
	@if [ -f user/musl/Makefile ]; then \
		echo "[Makefile] Build musl userland..."; \
		$(MAKE) -C user/musl all; \
	else \
		echo "[Makefile] user/musl/Makefile no existe, saltando"; \
	fi

# ---------------------------------------------------------------------------
# ELF malformado para probar p_offset + p_filesz fuera del archivo.
# ---------------------------------------------------------------------------
elf_malformed: user
	@if [ -f sysroot/usr/bin/filetest ]; then \
		cp sysroot/usr/bin/filetest sysroot/usr/bin/elf_malformed; \
		printf '\\377\\377\\377\\377\\377\\377\\377\\377' | \
			dd of=sysroot/usr/bin/elf_malformed bs=1 seek=96 count=8 \
			conv=notrunc status=none; \
	fi

# ---------------------------------------------------------------------------
# Python stdlib.
# ---------------------------------------------------------------------------
python-stdlib:
	@if [ -x scripts/fetch-python-stdlib.sh ]; then \
		echo "[Makefile] Fetch Python stdlib..."; \
		scripts/fetch-python-stdlib.sh; \
	else \
		echo "[Makefile]   (scripts/fetch-python-stdlib.sh no existe, saltando)"; \
	fi

# ===========================================================================
# INITRD
# ===========================================================================

initrd.tar: sysroot user elf_malformed python-stdlib drivers | env
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

	@rm -rf sysroot/tmp sysroot/var sysroot/run sysroot/data/tmp

# ---------------------------------------------------------------------------
# Ficheros estaticos de /etc.
# ---------------------------------------------------------------------------
	@echo "[Makefile] Copiando /etc estaticos..."
	@mkdir -p sysroot/etc
	@mkdir -p sysroot/usr/lib/aur
	@if [ "$(AHCI_USER)" = "1" ]; then \
		echo "[Makefile] AHCI en Ring 3 (ahci.aur): /usr/lib/aur/ahci.user"; \
		echo 1 > sysroot/usr/lib/aur/ahci.user; \
	else \
		rm -f sysroot/usr/lib/aur/ahci.user; \
	fi
	@if [ "$(XHCI_USER)" = "1" ]; then \
		echo "[Makefile] xHCI en Ring 3 (xhci.aur): /usr/lib/aur/xhci.user"; \
		echo 1 > sysroot/usr/lib/aur/xhci.user; \
	else \
		rm -f sysroot/usr/lib/aur/xhci.user; \
	fi
	@if [ "$(PS2_USER)" = "1" ]; then \
		echo "[Makefile] PS/2 en Ring 3 (ps2.aur): /usr/lib/aur/ps2.user"; \
		echo 1 > sysroot/usr/lib/aur/ps2.user; \
	else \
		rm -f sysroot/usr/lib/aur/ps2.user; \
	fi
	@if [ -f /etc/inputrc ]; then \
		cp -f /etc/inputrc sysroot/etc/inputrc; \
	else \
		printf '# /etc/inputrc (default)\nset bell-style none\nset meta-flag on\nset input-meta on\nset output-meta on\nset convert-meta off\n' > sysroot/etc/inputrc; \
	fi
	@if [ -f /etc/nsswitch.conf ]; then \
		cp -f /etc/nsswitch.conf sysroot/etc/nsswitch.conf; \
	else \
		printf 'passwd:     files\ngroup:      files\nshadow:     files\nhosts:      files dns\nservices:   files\nnetworks:   files\n' > sysroot/etc/nsswitch.conf; \
	fi
	@if [ -f /etc/bash.bashrc ]; then \
		cp -f /etc/bash.bashrc sysroot/etc/bash.bashrc; \
	else \
		printf '# /etc/bash.bashrc (default)\n' > sysroot/etc/bash.bashrc; \
	fi
	@if [ -f /usr/share/locale/locale.alias ]; then \
		mkdir -p sysroot/usr/share/locale; \
		cp -f /usr/share/locale/locale.alias sysroot/usr/share/locale/locale.alias; \
	fi
	@if [ -f /usr/share/zoneinfo/UTC ]; then \
		cp -f /usr/share/zoneinfo/UTC sysroot/etc/localtime; \
	fi

	@echo "[Makefile] Generando /etc/resolv.conf..."
	@printf '%b' "$(RESOLV_CONF)" > sysroot/etc/resolv.conf

# ---------------------------------------------------------------------------
# Bundle de CAs para TLS.
# ---------------------------------------------------------------------------
	@echo "[Makefile] Copiando bundle de CAs para TLS..."
	@mkdir -p sysroot/etc/ssl/certs
	@found_ca=0; \
	for ca in /etc/ssl/certs/ca-certificates.crt \
	          /etc/pki/tls/certs/ca-bundle.crt \
	          /etc/ssl/ca-bundle.pem; do \
		if [ -f "$$ca" ]; then \
			cp -Lf "$$ca" sysroot/etc/ssl/certs/ca-certificates.crt; \
			echo "[Makefile]   CA bundle: $$ca"; \
			found_ca=1; \
			break; \
		fi; \
	done; \
	if [ "$$found_ca" = "0" ]; then \
		echo "[Makefile]   (WARN) no se encontró CA bundle en el host."; \
		echo "[Makefile]          curl https:// fallará con error 77."; \
	else \
		mkdir -p sysroot/usr/lib/ssl; \
		ln -sfn /etc/ssl/certs/ca-certificates.crt sysroot/usr/lib/ssl/cert.pem; \
		ln -sfn /etc/ssl/certs sysroot/usr/lib/ssl/certs; \
	fi

# ---------------------------------------------------------------------------
# tcc + musl + ncurses + terminfo.
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
# frotz + Zork I.
# ---------------------------------------------------------------------------
	@echo "[Makefile] Copiando frotz + Zork I..."
	@[ -f third_party/frotz/frotz ] && cp -f third_party/frotz/frotz sysroot/usr/bin/ || true
	@[ -f third_party/zork1.z3 ] && cp -f third_party/zork1.z3 sysroot/usr/games/ || true

# ---------------------------------------------------------------------------
# libs/ — runtime de otros OS.
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
# NSS modules de glibc.
# ---------------------------------------------------------------------------
	@echo "[Makefile] Copiando NSS modules de glibc..."
	@mkdir -p sysroot/lib/x86_64-linux-gnu sysroot/usr/lib/x86_64-linux-gnu
	@for lib in libnss_dns.so.2 libnss_files.so.2 libnss_compat.so.2; do \
		for d in /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu; do \
			if [ -e "$$d/$$lib" ]; then \
				cp -Lf "$$d/$$lib" sysroot/lib/x86_64-linux-gnu/; \
				cp -Lf "$$d/$$lib" sysroot/usr/lib/x86_64-linux-gnu/; \
				echo "[Makefile]   nss: $$lib"; \
				break; \
			fi; \
		done; \
	done

# ---------------------------------------------------------------------------
# libgcc_s.so.1.
# ---------------------------------------------------------------------------
	@echo "[Makefile] Copiando libgcc_s.so.1..."
	@for d in /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu /lib64 /usr/lib64 /lib /usr/lib; do \
		if [ -e "$$d/libgcc_s.so.1" ]; then \
			cp -Lf "$$d/libgcc_s.so.1" sysroot/lib/x86_64-linux-gnu/; \
			mkdir -p sysroot/lib64 sysroot/usr/lib/x86_64-linux-gnu; \
			cp -Lf "$$d/libgcc_s.so.1" sysroot/lib64/ 2>/dev/null || true; \
			cp -Lf "$$d/libgcc_s.so.1" sysroot/usr/lib/x86_64-linux-gnu/ 2>/dev/null || true; \
			echo "[Makefile]   libgcc_s: $$d"; \
			break; \
		fi; \
	done

# ---------------------------------------------------------------------------
# precompiled/.
# ---------------------------------------------------------------------------
	@echo "[Makefile] Copiando precompiled/ a sysroot/..."
	@if [ -d precompiled ]; then \
		cp -a precompiled/. sysroot/; \
	else \
		echo "[Makefile]   (precompiled/ no existe)"; \
	fi

# ---------------------------------------------------------------------------
# Git HTTP(S) transport helper.
# ---------------------------------------------------------------------------
	@echo "[Makefile] Instalando helper HTTP(S) de Git..."
	@helper=/usr/lib/git-core/git-remote-http; \
	if [ ! -x "$$helper" ]; then \
		echo "[Makefile]   ERROR: no se encontró $$helper"; \
		exit 1; \
	fi; \
	mkdir -p sysroot/usr/lib/git-core \
	         sysroot/lib/x86_64-linux-gnu \
	         sysroot/usr/lib/x86_64-linux-gnu; \
	cp -Lf "$$helper" sysroot/usr/lib/git-core/git-remote-http; \
	ln -sf git-remote-http sysroot/usr/lib/git-core/git-remote-https; \
	ldd "$$helper" | awk '{ for (i = 1; i <= NF; i++) if ($$i ~ /^\//) { print $$i; break } }' | \
	while IFS= read -r dep; do \
		if [ -f "$$dep" ]; then \
			cp -Lf "$$dep" sysroot/lib/x86_64-linux-gnu/; \
			cp -Lf "$$dep" sysroot/usr/lib/x86_64-linux-gnu/; \
		fi; \
	done

# ---------------------------------------------------------------------------
# /bin/sh.
# ---------------------------------------------------------------------------
	@echo "[Makefile] Instalando /bin/sh (dash)..."
	@if [ -f sysroot/usr/bin/bash ]; then \
		cp -f sysroot/usr/bin/bash sysroot/bin/bash; \
		rm -f sysroot/bin/sh sysroot/usr/bin/sh; \
		ln -sf bash sysroot/bin/sh; \
		ln -sf bash sysroot/usr/bin/sh; \
	fi

# ---------------------------------------------------------------------------
# .bashrc / .profile.
# ---------------------------------------------------------------------------
	@echo "[Makefile] Creando .bashrc/.profile de root..."
	@mkdir -p sysroot/root
	@[ -f sysroot/root/.bashrc ] || printf '# ~/.bashrc (empty)\n' > sysroot/root/.bashrc
	@[ -f sysroot/root/.profile ] || printf '# ~/.profile (empty)\n' > sysroot/root/.profile

# ---------------------------------------------------------------------------
# /root/hello.c.
# ---------------------------------------------------------------------------
	@if [ ! -f sysroot/root/hello.c ]; then \
		printf '#include <stdio.h>\n\nint main(void) {\n    printf("hola desde codigo JIT\\n");\n    for (int i = 0; i < 5; i++)\n        printf("  iter %%d\\n", i);\n    return 0;\n}\n' > sysroot/root/hello.c; \
	fi

# ---------------------------------------------------------------------------
# /etc/ld.so.cache.
# ---------------------------------------------------------------------------
	@echo "[Makefile] Generando /etc/ld.so.cache..."
	@mkdir -p sysroot/etc/ld.so.conf.d
	@printf '/lib/x86_64-linux-gnu\n/usr/lib/x86_64-linux-gnu\n/lib64\n/usr/lib64\n/lib\n/usr/lib\n' > sysroot/etc/ld.so.conf
	@if command -v ldconfig >/dev/null 2>&1; then \
		ldconfig -r "$(CURDIR)/sysroot" 2>/dev/null \
			&& echo "[Makefile]   ld.so.cache OK" \
			|| echo "[Makefile]   ldconfig falló (no crítico)"; \
	elif [ -x /sbin/ldconfig ]; then \
		/sbin/ldconfig -r "$(CURDIR)/sysroot" 2>/dev/null \
			&& echo "[Makefile]   ld.so.cache OK" \
			|| echo "[Makefile]   ldconfig falló (no crítico)"; \
	else \
		echo "[Makefile]   ldconfig no encontrado (glibc hará probing)"; \
	fi

	tar --format=ustar --owner=0 --group=0 -cf kernel/initrd.tar -C sysroot .

kernel: initrd.tar
	$(MAKE) -C kernel

# ===========================================================================
# ISO — estilo Windows/Linux:
#   - efi.img contiene BOOTX64.EFI + kernel.elf + aurora.conf dentro.
#   - La ISO9660 solo contiene efi.img. NO dependemos de que el firmware
#     sepa leer ISO9660: solo FAT, que es lo único garantizado por UEFI.
# ===========================================================================

$(EFI_IMG): kernel initrd.tar $(BOOTX64_EFI) $(BIOS_IMG) $(AURORA_CONF)
	@mkdir -p build
	@bs=$$(stat -c%s $(KERNEL_ELF)); \
	 is=$$(stat -c%s kernel/initrd.tar); \
	 total=$$((bs + is)); \
	 size_mb=$$(awk "BEGIN { print int($$total/1048576) + 32 }"); \
	 if [ $$size_mb -lt 64 ]; then size_mb=64; fi; \
	 echo "[ISO] Creando $(EFI_IMG) (FAT16, $${size_mb} MB)..."; \
	 rm -f $(EFI_IMG); \
	 dd if=/dev/zero of=$(EFI_IMG) bs=1M count=$$size_mb status=none; \
	 mkfs.fat -F 16 -n EFIBOOT $(EFI_IMG) >/dev/null; \
	 mmd -i $(EFI_IMG) ::/EFI; \
	 mmd -i $(EFI_IMG) ::/EFI/BOOT; \
	 mmd -i $(EFI_IMG) ::/aurora; \
	 mcopy -i $(EFI_IMG) $(BOOTX64_EFI)      ::/EFI/BOOT/BOOTX64.EFI; \
	 mcopy -i $(EFI_IMG) $(KERNEL_ELF)       ::/aurora/kernel.elf; \
	 mcopy -i $(EFI_IMG) kernel/initrd.tar   ::/aurora/initrd.tar; \
	 mcopy -i $(EFI_IMG) $(AURORA_CONF)      ::/aurora/aurora.conf; \
	 echo "[ISO] $(EFI_IMG) listo:"; \
	 ls -lh $(EFI_IMG)

iso_root: $(EFI_IMG) $(BOOTX64_EFI) $(BIOS_IMG)
	@rm -rf $(ISO_ROOT)
	@mkdir -p $(ISO_ROOT)/EFI/BOOT $(ISO_ROOT)/boot
	@cp $(EFI_IMG)     $(ISO_ROOT)/efi.img
	@cp $(BOOTX64_EFI) $(ISO_ROOT)/EFI/BOOT/BOOTX64.EFI
	@cp $(BIOS_IMG)    $(ISO_ROOT)/boot/bios.img
	@echo "[ISO] $(ISO_ROOT) preparado:"
	@ls -lh $(ISO_ROOT)/efi.img $(ISO_ROOT)/EFI/BOOT/BOOTX64.EFI $(ISO_ROOT)/boot/bios.img

# ---------------------------------------------------------------------------
# aurora.iso: ISO híbrida (BIOS + UEFI) para CD y USB.
# ---------------------------------------------------------------------------
$(ISO_OUT): iso_root
	@echo "[ISO] Generando $(ISO_OUT) con xorriso (BIOS + UEFI)..."
	@xorriso -as mkisofs \
		-V "AURORA_OS" \
		-R -J -joliet-long \
		-b boot/bios.img -no-emul-boot -boot-load-size 128 \
		-eltorito-alt-boot \
		-e efi.img -no-emul-boot \
		-o $(ISO_OUT) \
		$(ISO_ROOT)

	@echo "[ISO] Convirtiendo en ISO híbrida (BIOS desde USB)..."
	@mkdir -p build
	@# 1. Obtener LBA del efi.img a partir del catálogo El Torito
	@EFI_LBA_2048=$$(xorriso -indev $(ISO_OUT) -report_el_torito plain 2>/dev/null | awk '/UEFI/ {print $$NF}'); \
	 EFI_LBA=$$((EFI_LBA_2048 * 4)); \
	 EFI_SIZE=$$(stat -c%s build/efi.img); \
	 EFI_SECTORS=$$((EFI_SIZE / 512)); \
	 echo "  [ISO] efi.img en LBA $$EFI_LBA_2048 (2048B) -> LBA $$EFI_LBA (512B, $$EFI_SECTORS sectores)"; \
	 \
	 echo "  [ISO] Calculando CHS para LBA $$EFI_LBA (para contentar a Rufus)"; \
	 S=$$(( (EFI_LBA % 63) + 1 )); \
	 H=$$(( (EFI_LBA / 63) % 255 )); \
	 C=$$(( EFI_LBA / 16065 )); \
	 C_H=$$(( C >> 8 )); \
	 C_L=$$(( C & 0xFF )); \
	 S_H=$$(( S + (C_H << 6) )); \
	 printf "  [ISO] CHS generado: C=$$C H=$$H S=$$S (bytes: %02x %02x %02x)\n" $$H $$S_H $$C_L; \
	 \
	 echo "  [ISO] Inyectando stage1_mbr.bin en LBA 0"; \
	 dd if=bootloader/bios/stage1_mbr.bin of=$(ISO_OUT) bs=512 count=1 conv=notrunc status=none; \
	 \
	 echo "  [ISO] Generando y inyectando tabla de particiones MBR"; \
	 printf "80 %02x %02x %02x 0c fe ff ff" $$H $$S_H $$C_L > build/mbr_part.hex; \
	 printf "%02x%02x%02x%02x " \
	     $$((EFI_LBA & 0xFF)) $$(((EFI_LBA >> 8) & 0xFF)) \
	     $$(((EFI_LBA >> 16) & 0xFF)) $$(((EFI_LBA >> 24) & 0xFF)) >> build/mbr_part.hex; \
	 printf "%02x%02x%02x%02x\n" \
	     $$((EFI_SECTORS & 0xFF)) $$(((EFI_SECTORS >> 8) & 0xFF)) \
	     $$(((EFI_SECTORS >> 16) & 0xFF)) $$(((EFI_SECTORS >> 24) & 0xFF)) >> build/mbr_part.hex; \
	 xxd -r -p build/mbr_part.hex build/mbr_part.bin; \
	 dd if=build/mbr_part.bin of=$(ISO_OUT) bs=1 seek=446 conv=notrunc status=none; \
	 \
	 echo "  [ISO] Inyectando stage2.bin en LBA 1"; \
	 dd if=bootloader/bios/stage2.bin of=$(ISO_OUT) bs=512 seek=1 conv=notrunc status=none

	@echo "[ISO] $(ISO_OUT) listo para CD/USB:"
	@ls -lh $(ISO_OUT)

# Alias.
iso: $(ISO_OUT)
image: $(ISO_OUT)

# ===========================================================================
# QEMU
# ===========================================================================

# RAM del guest: el initrd (~500 MB) se carga entero en memoria; con 512M el
# firmware devuelve EFI_OUT_OF_RESOURCES ("initrd no cargado"), no hay init y
# no arranca ni el dock ni las apps. Con Mesa (WITH_MESA=1) subir a 3G.
QEMU_MEM ?= 2G

# Aceleracion para 'run' / 'run-smp': KVM si /dev/kvm es accesible (el
# compositor va fluido), y TCG puro si no (muy lento: el raton se atasca).
# Forzar uno u otro:  make run QEMU_ACCEL="-cpu max"
QEMU_ACCEL ?= $(shell [ -w /dev/kvm ] && echo "-enable-kvm -cpu host" || echo "-cpu max")

QEMU_FLAGS_COMMON = \
	-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
	-drive if=pflash,format=raw,file=OVMF_VARS.fd \
	-m $(QEMU_MEM) \
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
	-drive if=none,id=disk0,format=raw,file=aurora.iso \
	-device ide-hd,drive=disk0,bus=ide.0,unit=0 \
	-drive if=none,id=cdrom0,format=raw,file=aurora.iso \
	-device ide-cd,drive=cdrom0,bus=ide.1,unit=0 \
	-drive if=none,id=testdisk,format=raw,file=tests/test_disk.img \
	-device ide-hd,drive=testdisk,bus=ide.1,unit=1

QEMU_STORAGE_Q35 = \
	-drive if=none,id=disk0,format=raw,file=aurora.iso \
	-device ide-hd,drive=disk0,bus=ide.0 \
	-drive if=none,id=testdisk,format=raw,file=tests/test_disk.img \
	-device ide-hd,drive=testdisk,bus=ide.1 \
	-drive if=none,id=cdrom0,format=raw,file=aurora.iso \
	-device ide-cd,drive=cdrom0,bus=ide.2

QEMU_FLAGS_Q35_AHCI = \
	-machine q35 \
	-device ahci,id=ahci \
	-drive if=none,id=disk0,format=raw,file=aurora.iso \
	-device ide-hd,drive=disk0,bus=ahci.0

# ---------------------------------------------------------------------------
# Networking de QEMU.
# ---------------------------------------------------------------------------
QEMU_NET_SLIRP = -nic user,model=e1000e
QEMU_NET_TAP   = -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
                 -device e1000e,netdev=net0

HOST_IFACE ?= eth0

TAP_IFACE      ?= tap0
TAP_HOST_IP    ?= 10.0.2.1
TAP_CIDR       ?= 24
TAP_DHCP_LOW   ?= 10.0.2.100
TAP_DHCP_HIGH  ?= 10.0.2.200
TAP_DHCP_LEASE ?= 12h
TAP_DNS        ?= 1.1.1.1

TAP_DNSMASQ_CONF   ?= /tmp/aurora-dnsmasq.conf
TAP_DNSMASQ_PID    ?= /tmp/aurora-dnsmasq.pid
TAP_DNSMASQ_LOG    ?= /tmp/aurora-dnsmasq.log
TAP_DNSMASQ_LEASES ?= /tmp/aurora-dnsmasq.leases
TAP_DNSMASQ_UNIT   ?= aurora-dnsmasq

TAP_SUBNET = $(shell echo $(TAP_HOST_IP) | awk -F. '{print $$1"."$$2"."$$3".0"}')/$(TAP_CIDR)

# ---------------------------------------------------------------------------
# TCG
# ---------------------------------------------------------------------------
run: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_NET_SLIRP) \
		$(QEMU_ACCEL) \
		-smp 4 \
		-cdrom $(ISO_OUT) \
		-serial stdio

run-debug: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_NET_SLIRP) \
		$(QEMU_FLAGS_DEBUG) \
		-cpu max \
		-smp 4 \
		-cdrom $(ISO_OUT) \
		-serial file:serial.log

run-smp: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_NET_SLIRP) \
		$(QEMU_ACCEL) \
		-smp 4 \
		-cdrom $(ISO_OUT) \
		-serial stdio

run-smp-debug: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_NET_SLIRP) \
		$(QEMU_FLAGS_DEBUG) \
		-cpu max \
		-smp 4 \
		-cdrom $(ISO_OUT) \
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
		$(QEMU_NET_SLIRP) \
		$(QEMU_FLAGS_KVM) \
		-smp 8 \
		-cdrom $(ISO_OUT) \
		-serial stdio

run-smp-kvm-debug: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_NET_SLIRP) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_FLAGS_DEBUG) \
		-smp 8 \
		-cdrom $(ISO_OUT) \
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
		$(QEMU_NET_SLIRP) \
		-smp 4 \
		-cdrom $(ISO_OUT) \
		-drive if=none,id=testdisk,format=raw,file=tests/test_disk.img \
		-device ahci,id=ahci \
		-device ide-hd,drive=testdisk,bus=ahci.0 \
		-serial stdio

run-smp-kvm-ahci-debug: iso tests/test_disk.img
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_FLAGS_DEBUG) \
		$(QEMU_NET_SLIRP) \
		-smp 4 \
		-cdrom $(ISO_OUT) \
		-drive if=none,id=testdisk,format=raw,file=tests/test_disk.img \
		-device ahci,id=ahci \
		-device ide-hd,drive=testdisk,bus=ahci.0 \
		-serial file:serial.log
	@echo ""
	@echo "=== Trampoline (debug.log) ==="
	@cat debug.log 2>/dev/null || echo "(vacío)"
	@echo ""
	@echo "=== Últimas líneas de serial.log ==="
	@tail -80 serial.log 2>/dev/null || echo "(vacío)"

# ---------------------------------------------------------------------------
# xHCI (USB 3.x) + teclado + ratón.
#
# qemu-xhci expone un controlador xHCI real en un slot PCI propio. Los dos
# USB devices van a puertos fijos (1 y 2) para que el log sea determinista:
#
#   port 1 -> usb-kbd   (HID class 0x03, subclass 0x01, proto 0x01)
#   port 2 -> usb-mouse (HID class 0x03, subclass 0x01, proto 0x02)
#
# Fase 1 (bring-up): busca en el log
#   [xHCI] detectado en ...
#   [xHCI] ver=0110 CAPLEN=0x40 slots=... ports=...
#   [xHCI] inicializado
#   [TEST] PASS xhci: no-op command
#   [xHCI] port 1: PORTSC=0x... CCS=1 ...
#   [xHCI] port 2: PORTSC=0x... CCS=1 ...
#
# Fase 2 (HID): los reports de teclado y ratón llegarán al compositor.
#
# Fase 3 (hubs): añade `-device usb-hub,...` (ver comentario más abajo).
# Fase 4 (storage): añade `-device usb-storage,...` (idem).
# ---------------------------------------------------------------------------
run-smp-kvm-xhci: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_NET_SLIRP) \
		-smp 4 \
		-cdrom $(ISO_OUT) \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0,port=1 \
		-device usb-mouse,bus=xhci.0,port=2 \
		-serial stdio

run-smp-kvm-xhci-debug: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_FLAGS_DEBUG) \
		$(QEMU_NET_SLIRP) \
		-smp 4 \
		-cdrom $(ISO_OUT) \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0,port=1 \
		-device usb-mouse,bus=xhci.0,port=2 \
		-serial file:serial.log
	@echo ""
	@echo "=== Trampoline (debug.log) ==="
	@cat debug.log 2>/dev/null || echo "(vacío)"
	@echo ""
	@echo "=== Últimas líneas de serial.log (xHCI) ==="
	@grep -E '\[xHCI|\[USB|usb:' serial.log 2>/dev/null | tail -80 || tail -80 serial.log 2>/dev/null || echo "(vacío)"
	
run-smp-kvm-xhci-storage: iso tests/test_disk.img
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_NET_SLIRP) \
		-smp 4 \
		-cdrom $(ISO_OUT) \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-device usb-mouse,bus=xhci.0 \
		-drive if=none,id=usbstick,format=raw,file=tests/test_disk.img \
		-device usb-storage,drive=usbstick,bus=xhci.0 \
		-serial stdio

run-smp-kvm-xhci-storage-debug: iso tests/test_disk.img
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_FLAGS_DEBUG) \
		$(QEMU_NET_SLIRP) \
		-smp 4 \
		-cdrom $(ISO_OUT) \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-device usb-mouse,bus=xhci.0 \
		-drive if=none,id=usbstick,format=raw,file=tests/test_disk.img \
		-device usb-storage,drive=usbstick,bus=xhci.0 \
		-serial file:serial.log
	@echo ""
	@echo "=== Últimas líneas de serial.log (USB) ==="
	@grep -E '\[xHCI|\[USB|\[BLK|\[FAT32' serial.log 2>/dev/null | tail -100 || tail -100 serial.log 2>/dev/null || echo "(vacío)"

run-smp-kvm-xhci-cd: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_NET_SLIRP) \
		-smp 4 \
		-cdrom $(ISO_OUT) \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-device usb-mouse,bus=xhci.0 \
		-drive if=none,id=usbcd,format=raw,media=cdrom,file=$(ISO_OUT),readonly=on \
		-device usb-storage,drive=usbcd,bus=xhci.0 \
		-serial stdio

run-smp-kvm-xhci-hub: iso tests/test_disk.img
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_NET_SLIRP) \
		-smp 4 \
		-cdrom $(ISO_OUT) \
		-device qemu-xhci,id=xhci \
		-device usb-hub,bus=xhci.0,port=1 \
		-device usb-kbd,bus=xhci.0,port=1.1 \
		-device usb-mouse,bus=xhci.0,port=1.2 \
		-drive if=none,id=usbstick,format=raw,file=tests/test_disk.img \
		-device usb-storage,drive=usbstick,bus=xhci.0,port=2 \
		-serial stdio

run-smp-kvm-xhci-hub-debug: iso tests/test_disk.img
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_FLAGS_DEBUG) \
		$(QEMU_NET_SLIRP) \
		-smp 4 \
		-cdrom $(ISO_OUT) \
		-device qemu-xhci,id=xhci \
		-device usb-hub,bus=xhci.0,port=1 \
		-device usb-kbd,bus=xhci.0,port=1.1 \
		-device usb-mouse,bus=xhci.0,port=1.2 \
		-drive if=none,id=usbstick,format=raw,file=tests/test_disk.img \
		-device usb-storage,drive=usbstick,bus=xhci.0,port=2 \
		-serial file:serial.log
	@echo ""
	@grep -E '\[xHCI|\[USB|\[HUB|\[BLK' serial.log | tail -80
	
run-smp-kvm-xhci-uas: iso tests/test_disk.img
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_NET_SLIRP) \
		-smp 4 \
		-cdrom $(ISO_OUT) \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-device usb-mouse,bus=xhci.0 \
		-drive if=none,id=usbstick,format=raw,file=tests/test_disk.img \
		-device usb-uas,id=uas,bus=xhci.0 \
		-device scsi-hd,bus=uas.0,scsi-id=0,lun=0,drive=usbstick \
		-serial stdio

run-kvm-ahci: iso tests/test_disk.img
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_NET_SLIRP) \
		-cdrom $(ISO_OUT) \
		-drive if=none,id=testdisk,format=raw,file=tests/test_disk.img \
		-device ahci,id=ahci \
		-device ide-hd,drive=testdisk,bus=ahci.0 \
		-serial stdio

run-smp-kvm-ahci-3disk: iso tests/test_disk.img tests/extra_disk.img
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_NET_SLIRP) \
		-smp 4 \
		-cdrom $(ISO_OUT) \
		-drive if=none,id=testdisk,format=raw,file=tests/test_disk.img \
		-drive if=none,id=extradisk,format=raw,file=tests/extra_disk.img \
		-device ahci,id=ahci \
		-device ide-hd,drive=testdisk,bus=ahci.0 \
		-device ide-hd,drive=extradisk,bus=ahci.1 \
		-serial stdio

run-smp-kvm-ahci-tap: iso tests/test_disk.img
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_NET_TAP) \
		-smp 4 \
		-cdrom $(ISO_OUT) \
		-drive if=none,id=testdisk,format=raw,file=tests/test_disk.img \
		-device ahci,id=ahci \
		-device ide-hd,drive=testdisk,bus=ahci.0 \
		-serial stdio

# ---------------------------------------------------------------------------
# ISO como USB (isohybrid check).
# ---------------------------------------------------------------------------
run-iso-as-usb: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_NET_SLIRP) \
		-smp 4 \
		-device qemu-xhci,id=xhci \
		-drive if=none,id=usbstick,format=raw,file=$(ISO_OUT) \
		-device usb-storage,drive=usbstick,bus=xhci.0 \
		-serial stdio

run-iso-tcg: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_NET_SLIRP) \
		-cpu max \
		-smp 4 \
		-cdrom $(ISO_OUT) \
		-serial stdio

# Alias: run-iso == run-smp-kvm.
run-iso: run-smp-kvm

# ===========================================================================
# BIOS (SeaBIOS por defecto, sin OVMF)
# ===========================================================================
QEMU_FLAGS_BIOS = -m $(QEMU_MEM) -no-reboot -no-shutdown

run-bios: iso
	qemu-system-x86_64 \
		$(QEMU_FLAGS_BIOS) \
		$(QEMU_NET_SLIRP) \
		-cpu host \
		-smp 2 \
		-cdrom $(ISO_OUT) \
		-serial stdio \
		-enable-kvm

run-bios-debug: iso
	qemu-system-x86_64 \
		$(QEMU_FLAGS_BIOS) \
		$(QEMU_NET_SLIRP) \
		-cpu host \
		-smp 2 \
		-cdrom $(ISO_OUT) \
		-serial file:serial.log \
		-s -S \
		-enable-kvm

# ---------------------------------------------------------------------------
# Prueba del ISO como si fuera un USB conectado (para testear el MBR/BIOS)
# ---------------------------------------------------------------------------
run-bios-usb: iso
	qemu-system-x86_64 -m $(QEMU_MEM) -no-reboot -no-shutdown \
	    -nic user,model=e1000e -cpu max -smp 2 \
	    -drive if=none,id=usbstick,format=raw,file=$(ISO_OUT) \
	    -device qemu-xhci,id=xhci \
	    -device usb-storage,drive=usbstick,bus=xhci.0 \
	    -serial stdio \
		-enable-kvm

# ===========================================================================
# TAP setup / teardown / status
# ===========================================================================

tap-setup:
	@echo "[tap] === Configurando red privada $(TAP_SUBNET) en $(TAP_IFACE) ==="
	@if ! command -v dnsmasq >/dev/null 2>&1; then \
		echo "[tap] ERROR: dnsmasq no está instalado."; \
		echo "[tap]        sudo apt install dnsmasq"; \
		exit 1; \
	fi
	@if ! command -v systemd-run >/dev/null 2>&1; then \
		echo "[tap] ERROR: systemd-run no está disponible."; \
		exit 1; \
	fi

	-@ip link show $(TAP_IFACE) >/dev/null 2>&1 || \
		sudo ip tuntap add dev $(TAP_IFACE) mode tap user $$(id -u -n)
	sudo ip link set $(TAP_IFACE) up
	sudo ip addr flush dev $(TAP_IFACE) 2>/dev/null || true
	sudo ip addr add $(TAP_HOST_IP)/$(TAP_CIDR) dev $(TAP_IFACE)
	@echo "[tap] $(TAP_IFACE) arriba: $(TAP_HOST_IP)/$(TAP_CIDR)"

	sudo sysctl -qw net.ipv4.ip_forward=1

	sudo iptables -t nat -C POSTROUTING -s $(TAP_SUBNET) -o $(HOST_IFACE) -j MASQUERADE 2>/dev/null || \
		sudo iptables -t nat -A POSTROUTING -s $(TAP_SUBNET) -o $(HOST_IFACE) -j MASQUERADE
	sudo iptables -C FORWARD -i $(TAP_IFACE) -o $(HOST_IFACE) -j ACCEPT 2>/dev/null || \
		sudo iptables -A FORWARD -i $(TAP_IFACE) -o $(HOST_IFACE) -j ACCEPT
	sudo iptables -C FORWARD -i $(HOST_IFACE) -o $(TAP_IFACE) -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || \
		sudo iptables -A FORWARD -i $(HOST_IFACE) -o $(TAP_IFACE) -m state --state RELATED,ESTABLISHED -j ACCEPT
	@echo "[tap] NAT: $(TAP_SUBNET) → $(HOST_IFACE) (MASQUERADE)"

	@echo "[tap] generando $(TAP_DNSMASQ_CONF)"
	@printf '%s\n' \
		"# Generado por make tap-setup. No editar a mano." \
		"interface=$(TAP_IFACE)" \
		"bind-interfaces" \
		"except-interface=lo" \
		"port=0" \
		"no-resolv" \
		"dhcp-range=$(TAP_DHCP_LOW),$(TAP_DHCP_HIGH),$(TAP_DHCP_LEASE)" \
		"dhcp-option=3,$(TAP_HOST_IP)" \
		"dhcp-option=6,$(TAP_DNS)" \
		"dhcp-leasefile=$(TAP_DNSMASQ_LEASES)" \
		"log-dhcp" \
		> $(TAP_DNSMASQ_CONF)

	-sudo systemctl stop $(TAP_DNSMASQ_UNIT) 2>/dev/null || true
	-sudo systemctl reset-failed $(TAP_DNSMASQ_UNIT) 2>/dev/null || true
	-sudo rm -f $(TAP_DNSMASQ_PID) $(TAP_DNSMASQ_LOG) $(TAP_DNSMASQ_LEASES) 2>/dev/null || true

	@echo "[tap] arrancando dnsmasq (unit systemd: $(TAP_DNSMASQ_UNIT))"
	sudo systemd-run --unit=$(TAP_DNSMASQ_UNIT) \
		--description="Aurora DHCP/DNS on $(TAP_IFACE)" \
		--property=Type=forking \
		--property=PIDFile=$(TAP_DNSMASQ_PID) \
		--property=Restart=on-failure \
		/usr/sbin/dnsmasq \
			--conf-file=$(TAP_DNSMASQ_CONF) \
			--pid-file=$(TAP_DNSMASQ_PID) \
			--log-facility=$(TAP_DNSMASQ_LOG)
	@sleep 1.0

	@if ! systemctl is-active --quiet $(TAP_DNSMASQ_UNIT); then \
		echo "[tap] ERROR: $(TAP_DNSMASQ_UNIT) no está activo"; \
		sudo journalctl -u $(TAP_DNSMASQ_UNIT) -n 30 --no-pager; \
		exit 1; \
	fi
	@PID=$$(systemctl show -p MainPID --value $(TAP_DNSMASQ_UNIT)); \
	echo "[tap] dnsmasq arriba: unit=$(TAP_DNSMASQ_UNIT) pid=$$PID"

	@echo "[tap] DHCP: $(TAP_DHCP_LOW) - $(TAP_DHCP_HIGH) (lease $(TAP_DHCP_LEASE))"
	@echo "[tap] GW:   $(TAP_HOST_IP)"
	@echo "[tap] DNS:  $(TAP_DNS)"
	@echo "[tap] listo. Siguiente: make run-smp-kvm-ahci-tap"

tap-teardown:
	@echo "[tap] === Parando red privada ==="
	@echo "[tap] parando $(TAP_DNSMASQ_UNIT)"
	-sudo systemctl stop $(TAP_DNSMASQ_UNIT) 2>/dev/null || true
	-sudo systemctl reset-failed $(TAP_DNSMASQ_UNIT) 2>/dev/null || true
	-sudo rm -f $(TAP_DNSMASQ_PID) $(TAP_DNSMASQ_LOG) $(TAP_DNSMASQ_CONF) \
	             $(TAP_DNSMASQ_LEASES) 2>/dev/null || true

	-sudo iptables -t nat -D POSTROUTING -s $(TAP_SUBNET) -o $(HOST_IFACE) -j MASQUERADE 2>/dev/null || true
	-sudo iptables -D FORWARD -i $(TAP_IFACE) -o $(HOST_IFACE) -j ACCEPT 2>/dev/null || true
	-sudo iptables -D FORWARD -i $(HOST_IFACE) -o $(TAP_IFACE) -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || true

	-sudo ip link set $(TAP_IFACE) down 2>/dev/null || true
	-sudo ip tuntap del dev $(TAP_IFACE) mode tap 2>/dev/null || true
	@echo "[tap] $(TAP_IFACE) eliminado."

tap-status:
	@echo "=== $(TAP_IFACE) ==="
	@ip addr show $(TAP_IFACE) 2>/dev/null || echo "  (no existe. Ejecuta: make tap-setup)"
	@echo ""
	@echo "=== dnsmasq (unit systemd $(TAP_DNSMASQ_UNIT)) ==="
	@if systemctl is-active --quiet $(TAP_DNSMASQ_UNIT) 2>/dev/null; then \
		PID=$$(systemctl show -p MainPID --value $(TAP_DNSMASQ_UNIT)); \
		echo "  activo: MainPID=$$PID"; \
		systemctl status $(TAP_DNSMASQ_UNIT) --no-pager -n 3 2>/dev/null | sed 's/^/    /'; \
	else \
		echo "  inactivo"; \
		echo "  últimas líneas del journal:"; \
		sudo journalctl -u $(TAP_DNSMASQ_UNIT) -n 10 --no-pager 2>/dev/null | sed 's/^/    /' || true; \
	fi
	@echo ""
	@echo "=== iptables NAT ==="
	@sudo iptables -t nat -L POSTROUTING -n -v 2>/dev/null | grep -E "$(TAP_SUBNET)|Chain" || echo "  (sin reglas para $(TAP_SUBNET))"
	@echo ""
	@echo "=== iptables FORWARD ==="
	@sudo iptables -L FORWARD -n -v 2>/dev/null | grep "$(TAP_IFACE)" || echo "  (sin reglas para $(TAP_IFACE))"
	@echo ""
	@echo "=== leases activos ==="
	@sudo cat $(TAP_DNSMASQ_LEASES) 2>/dev/null || echo "  (ninguno)"
	@echo ""
	@echo "=== últimas líneas del log dnsmasq ==="
	@sudo tail -20 $(TAP_DNSMASQ_LOG) 2>/dev/null || echo "  (sin log)"


# ===========================================================================
# [BUILD DINÁMICO] Descubrimiento de drivers (.aur y .kur)
# ===========================================================================

HOST_CC    ?= gcc
# Sin cross-compiler x86_64-elf-gcc se usa el gcc del host (x86_64 ELF): vale
# para codigo freestanding; solo hay que desactivar CET.
KUR_CC     ?= $(shell command -v x86_64-elf-gcc >/dev/null 2>&1 && echo x86_64-elf-gcc || echo gcc)
AUR_CC     ?= gcc
ifeq ($(KUR_CC),gcc)
KUR_HOSTFLAGS = -fcf-protection=none
endif

KUR_CFLAGS  = $(KUR_HOSTFLAGS) -ffreestanding -O2 -mcmodel=kernel -mno-red-zone \
              -fno-stack-protector -fno-pic -fno-pie -fno-common \
              -fno-asynchronous-unwind-tables -I. -Ikernel

AUR_DIRS := $(patsubst %/config.mk,%,$(wildcard drivers/aur/*/config.mk))
KUR_DIRS := $(patsubst %/config.mk,%,$(wildcard drivers/kur/*/config.mk))

AUR_TARGETS :=
KUR_TARGETS :=

define AUR_TEMPLATE
_AUR_CAPS_$(notdir $(1)) := $(strip $(shell grep -m1 '^AUR_CAPS' $(1)/config.mk | cut -d= -f2-))
_AUR_ARGS_$(notdir $(1)) := $(strip $(shell grep -m1 '^AUR_ARGS' $(1)/config.mk | cut -d= -f2-))

$(1)/$(notdir $(1)).elf: $(wildcard $(1)/*.c)
	@echo "[AUR-CC] $(notdir $(1))"
	@$(AUR_CC) -O2 -fno-stack-protector -o $$@ $$^

$(1)/$(notdir $(1)).aur: $(1)/$(notdir $(1)).elf $(1)/config.mk tools/aurbuild
	@echo "[AURBUILD] $(notdir $(1))"
	@./tools/aurbuild --name=$(notdir $(1)) --caps=$$(_AUR_CAPS_$(notdir $(1))) $$(_AUR_ARGS_$(notdir $(1))) $$< $$@

install-aur-$(notdir $(1)): $(1)/$(notdir $(1)).aur
	@mkdir -p sysroot/usr/lib/aur
	@cp -f $$< sysroot/usr/lib/aur/$(notdir $(1)).aur
	@chmod +x sysroot/usr/lib/aur/$(notdir $(1)).aur

AUR_TARGETS += install-aur-$(notdir $(1))
endef

define KUR_TEMPLATE
_KUR_HASH_$(notdir $(1)) := $(strip $(shell grep -m1 '^KUR_HASH' $(1)/config.mk | cut -d= -f2-))

$(1)/$(notdir $(1)).o: $(wildcard $(1)/*.c)
	@echo "[KUR-CC] $(notdir $(1))"
	@$(KUR_CC) $(KUR_CFLAGS) -c $$^ -o $$@

$(1)/$(notdir $(1)).kur: $(1)/$(notdir $(1)).o $(1)/config.mk tools/kurbuild
	@echo "[KURBUILD] $(notdir $(1))"
	@./tools/kurbuild $$(_KUR_HASH_$(notdir $(1))) $$< $$@

install-kur-$(notdir $(1)): $(1)/$(notdir $(1)).kur
	@mkdir -p sysroot/lib/modules
	@cp -f $$< sysroot/lib/modules/$(notdir $(1)).kur

KUR_TARGETS += install-kur-$(notdir $(1))
endef

$(foreach dir,$(AUR_DIRS),$(eval $(call AUR_TEMPLATE,$(dir))))
$(foreach dir,$(KUR_DIRS),$(eval $(call KUR_TEMPLATE,$(dir))))

# ---------------------------------------------------------------------------
# Herramientas Host
# ---------------------------------------------------------------------------
tools/kurbuild: tools/kurbuild.c kernel/sha256.c kernel/sha256.h
	@$(HOST_CC) -O2 -Wall -Wextra -iquote kernel -o $@ tools/kurbuild.c kernel/sha256.c

tools/aurbuild: tools/aurbuild.c
	@$(HOST_CC) -O2 -Wall -Wextra -o $@ $<

# Árbol /lib/modules/<release>/ para kmod (modprobe/rmmod/lsmod), con los
# .kur como <modulo>.ko y los índices generados por depmod.
KERNEL_RELEASE ?= 0.1.0

.PHONY: kmod-tree
kmod-tree: $(KUR_TARGETS)
	@bash scripts/mk-kmod-tree.sh sysroot $(KERNEL_RELEASE) drivers/kur

.PHONY: drivers
# LinuxKPI: capa de compatibilidad con drivers de Linux y sus .kur de prueba
# (/lib/modules/linuxkpi/). Ver Plan_LinuxKPI.md.
include kernel/linuxkpi/linuxkpi.mk

drivers: | env
drivers: tools/kurbuild tools/aurbuild $(KUR_TARGETS) $(AUR_TARGETS) kmod-tree linuxkpi

# ===========================================================================
# Clean
# ===========================================================================
clean:
	$(MAKE) -C bootloader/efi clean
	$(MAKE) -C bootloader/bios clean
	$(MAKE) -C kernel clean
	$(MAKE) -C user clean
	@if [ -f user/musl/Makefile ]; then $(MAKE) -C user/musl clean; fi
	rm -rf sysroot kernel/initrd.tar esp
	rm -rf build/efi.img build/iso_root build/mbr_part.bin
	rm -f aurora.iso
	rm -f serial.log debug.log qemu_debug.log 
	rm -f tools/aurbuild
