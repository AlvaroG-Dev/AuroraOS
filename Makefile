# Makefile global - Aurora OS

.PHONY: all bootloader kernel user user-native user-musl image \
        run run-debug run-smp run-smp-debug \
        run-smp-kvm run-smp-kvm-debug clean sysroot initrd.tar iso run-iso \
        run-smp-kvm-ahci run-smp-kvm-ahci-debug run-kvm-ahci \
        run-smp-kvm-ahci-3disk run-smp-kvm-ahci-tap \
        tap-setup tap-teardown python-stdlib

RESOLV_CONF ?= nameserver 1.1.1.1\nnameserver 8.8.8.8\noptions timeout:2 attempts:2\n

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
# ---------------------------------------------------------------------------
elf_malformed: user
	@if [ -f sysroot/usr/bin/filetest ]; then \
		cp sysroot/usr/bin/filetest sysroot/usr/bin/elf_malformed; \
		printf '\\377\\377\\377\\377\\377\\377\\377\\377' | \
			dd of=sysroot/usr/bin/elf_malformed bs=1 seek=96 count=8 \
			conv=notrunc status=none; \
	fi

# ---------------------------------------------------------------------------
# Python stdlib: copia el stdlib del host a sysroot/usr/lib/python3.X/ y
# precompila .py → .pyc.
# ---------------------------------------------------------------------------
python-stdlib:
	@if [ -x scripts/fetch-python-stdlib.sh ]; then \
		echo "[Makefile] Fetch Python stdlib..."; \
		scripts/fetch-python-stdlib.sh; \
	else \
		echo "[Makefile]   (scripts/fetch-python-stdlib.sh no existe, saltando)"; \
	fi

# ---------------------------------------------------------------------------
# initrd.tar: construye el tarfs desde sysroot/.
# ---------------------------------------------------------------------------
initrd.tar: sysroot user elf_malformed python-stdlib
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
# Bundle de CAs para TLS (openssl/curl/gnutls).
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
	fi

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
# 4b. NSS modules de glibc.
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
# 4c. libgcc_s.so.1: la necesita glibc para pthread_exit (unwinding de la
# pila del thread). Sin ella, cualquier thread que llame a pthread_exit
# aborta con "libgcc_s.so.1 must be installed for pthread_exit to work" y
# el proceso muere con SIGABRT (code 134). git la usa: crea un thread
# sideband-demuxer durante el clone y cuando sale llama a pthread_exit.
#
# Es la misma versión que libc.so.6 (mismo toolchain del host).
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
# ---------------------------------------------------------------------------
	@echo "[Makefile] Instalando /bin/sh (dash)..."
	@if [ -f sysroot/usr/bin/bash ]; then \
		cp -f sysroot/usr/bin/bash sysroot/bin/bash; \
		rm -f sysroot/bin/sh sysroot/usr/bin/sh; \
		ln -sf bash sysroot/bin/sh; \
		ln -sf bash sysroot/usr/bin/sh; \
	fi

# ---------------------------------------------------------------------------
# [bash] /etc/passwd, /etc/group, /etc/profile ahora los crea
# tmpfs_init() en runtime.
# ---------------------------------------------------------------------------
	@echo "[Makefile] Creando .bashrc/.profile de root..."
	@mkdir -p sysroot/root
	@[ -f sysroot/root/.bashrc ] || printf '# ~/.bashrc (empty)\n' > sysroot/root/.bashrc
	@[ -f sysroot/root/.profile ] || printf '# ~/.profile (empty)\n' > sysroot/root/.profile

# ---------------------------------------------------------------------------
# 6. /root/hello.c para tcc -run
# ---------------------------------------------------------------------------
	@if [ ! -f sysroot/root/hello.c ]; then \
		printf '#include <stdio.h>\n\nint main(void) {\n    printf("hola desde codigo JIT\\n");\n    for (int i = 0; i < 5; i++)\n        printf("  iter %%d\\n", i);\n    return 0;\n}\n' > sysroot/root/hello.c; \
	fi

# ---------------------------------------------------------------------------
# /etc/ld.so.cache
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

image: bootloader kernel user
	mkdir -p esp/EFI/BOOT esp/etc
	cp bootloader/BOOTX64.EFI esp/EFI/BOOT/
	cp kernel/kernel.elf esp/
	cp bootloader/aurora.conf esp/etc/aurora.conf

	@if [ ! -f aurora.img ]; then \
		echo "[Makefile] Creando aurora.img (primera vez, 256 MB)..."; \
		dd if=/dev/zero of=aurora.img bs=1M count=256 status=none; \
		mkfs.fat -F 32 -s 4 -S 512 aurora.img >/dev/null; \
	else \
		echo "[Makefile] Reutilizando aurora.img existente (persistente)"; \
	fi

	mcopy -i aurora.img -o -s esp/EFI ::
	mcopy -i aurora.img -o esp/kernel.elf ::
	mcopy -i aurora.img -o -s esp/etc ::

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
# Networking de QEMU.
#
# Por defecto usamos slirp (user-mode): no requiere privilegios, funciona
# out-of-the-box, pero tiene un techo de ~300-500 KB/s por la granularidad
# del timer interno de QEMU (4 ms por paquete).
#
# Con TAP hablamos TCP/IP directo con la red del host y vemos decenas de
# MB/s. Requiere configurar la interfaz tap0 en el host (ver `make tap-setup`).
#
# El driver e1000e de Aurora tiene hardcodeado 10.0.2.15/24 gw=10.0.2.2
# (los defaults de slirp), así que reutilizamos esa misma subred en tap0.
# No colisiona mientras no corras otro QEMU con slirp a la vez en el host.
# ---------------------------------------------------------------------------
QEMU_NET_SLIRP = -nic user,model=e1000e
QEMU_NET_TAP   = -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
                 -device e1000e,netdev=net0

HOST_IFACE ?= eth0

# ---------------------------------------------------------------------------
# TCG
# ---------------------------------------------------------------------------
run: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_STORAGE_PC) \
		$(QEMU_NET_SLIRP) \
		-cpu max \
		-serial stdio

run-debug: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_STORAGE_PC) \
		$(QEMU_NET_SLIRP) \
		$(QEMU_FLAGS_DEBUG) \
		-cpu max \
		-serial file:serial.log

run-smp: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_STORAGE_PC) \
		$(QEMU_NET_SLIRP) \
		-cpu max \
		-smp 4 \
		-serial stdio

run-smp-debug: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_STORAGE_PC) \
		$(QEMU_NET_SLIRP) \
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
		$(QEMU_NET_SLIRP) \
		$(QEMU_FLAGS_KVM) \
		-smp 8 \
		-serial stdio

run-smp-kvm-debug: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_STORAGE_PC) \
		$(QEMU_NET_SLIRP) \
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
		$(QEMU_NET_SLIRP) \
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
		$(QEMU_NET_SLIRP) \
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
		$(QEMU_NET_SLIRP) \
		-serial stdio

run-smp-kvm-ahci-3disk: iso tests/test_disk.img tests/extra_disk.img
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_STORAGE_Q35) \
		$(QEMU_NET_SLIRP) \
		-drive if=none,id=disk2,format=raw,file=tests/extra_disk.img \
		-device ide-hd,drive=disk2,bus=ide.3 \
		-smp 4 \
		-serial stdio

# ---------------------------------------------------------------------------
# run-smp-kvm-ahci-tap: igual que run-smp-kvm-ahci pero con red TAP en vez
# de slirp. Requiere `make tap-setup` ejecutado antes (una vez por sesión).
# ---------------------------------------------------------------------------
run-smp-kvm-ahci-tap: iso tests/test_disk.img
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_STORAGE_Q35) \
		$(QEMU_NET_TAP) \
		-smp 4 \
		-serial stdio

# ---------------------------------------------------------------------------
# tap-setup / tap-teardown.
#
# Configura (o borra) la interfaz tap0 del host. Requiere sudo. Sobrevive
# a reinicios de QEMU, hay que rehacerlo tras reboot del host.
#
# HOST_IFACE debe ser tu interfaz de red real (la que tiene el default
# route). Descúbrela con: ip route | grep default
#
#   make tap-setup HOST_IFACE=wlan0
#   make run-smp-kvm-ahci-tap
#
# Cuando termines:
#   make tap-teardown HOST_IFACE=wlan0
# ---------------------------------------------------------------------------
tap-setup:
	@echo "[tap] creando tap0..."
	-sudo ip tuntap add dev tap0 mode tap user $$(id -u -n)
	sudo ip link set tap0 up
	sudo ip addr add 10.0.2.2/24 dev tap0 || true
	sudo sysctl -w net.ipv4.ip_forward=1
	sudo iptables -t nat -C POSTROUTING -s 10.0.2.0/24 -o $(HOST_IFACE) -j MASQUERADE 2>/dev/null || \
		sudo iptables -t nat -A POSTROUTING -s 10.0.2.0/24 -o $(HOST_IFACE) -j MASQUERADE
	sudo iptables -C FORWARD -i tap0 -o $(HOST_IFACE) -j ACCEPT 2>/dev/null || \
		sudo iptables -A FORWARD -i tap0 -o $(HOST_IFACE) -j ACCEPT
	sudo iptables -C FORWARD -i $(HOST_IFACE) -o tap0 -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || \
		sudo iptables -A FORWARD -i $(HOST_IFACE) -o tap0 -m state --state RELATED,ESTABLISHED -j ACCEPT
	@echo "[tap] listo. Ahora: make run-smp-kvm-ahci-tap"

tap-teardown:
	-sudo iptables -t nat -D POSTROUTING -s 10.0.2.0/24 -o $(HOST_IFACE) -j MASQUERADE
	-sudo iptables -D FORWARD -i tap0 -o $(HOST_IFACE) -j ACCEPT
	-sudo iptables -D FORWARD -i $(HOST_IFACE) -o tap0 -m state --state RELATED,ESTABLISHED -j ACCEPT
	-sudo ip link set tap0 down
	-sudo ip tuntap del dev tap0 mode tap
	@echo "[tap] tap0 eliminado."

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
		$(QEMU_NET_SLIRP) \
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