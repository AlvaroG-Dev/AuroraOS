# Makefile global - Aurora OS

.PHONY: all bootloader kernel user image run run-debug run-smp run-smp-debug \
        run-smp-kvm run-smp-kvm-debug clean sysroot initrd.tar iso run-iso \
        run-smp-kvm-ahci run-smp-kvm-ahci-debug run-kvm-ahci run-smp-kvm-ahci-2disk

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


# ---------------------------------------------------------------------------
# ELF malformado para probar p_offset + p_filesz fuera del archivo.
# Se parte de un ELF real y se corrompe p_filesz del primer PT_LOAD.
# ---------------------------------------------------------------------------
elf_malformed: user
	@mkdir -p sysroot/apps
	@cp sysroot/apps/filetest sysroot/apps/elf_malformed
	@printf '\\377\\377\\377\\377\\377\\377\\377\\377' | dd of=sysroot/apps/elf_malformed bs=1 seek=96 count=8 conv=notrunc status=none

initrd.tar: sysroot user elf_malformed
	@echo "[Makefile] Generando initrd.tar desde sysroot/"
	tar --format=ustar -cf kernel/initrd.tar -C sysroot .

kernel: initrd.tar
	$(MAKE) -C kernel

image: bootloader kernel user
	mkdir -p esp/EFI/BOOT
	cp bootloader/BOOTX64.EFI esp/EFI/BOOT/
	cp kernel/kernel.elf esp/
	dd if=/dev/zero of=aurora.img bs=1M count=64
	mkfs.fat -F 32 aurora.img
	mcopy -i aurora.img -s esp/EFI ::
	mcopy -i aurora.img -s esp/kernel.elf ::
	# [TEST] Copiar binarios userland al FS persistente para validar
	# elf_load_streaming sobre FAT32. Nombres 8.3 hasta que haya LFN.
	@if [ -f sysroot/apps/ls ]; then \
		mcopy -i aurora.img sysroot/apps/ls ::/LS.ELF; \
	fi
	@if [ -f sysroot/apps/cat ]; then \
		mcopy -i aurora.img sysroot/apps/cat ::/CAT.ELF; \
	fi

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
	-m 5G \
	-no-reboot -no-shutdown

QEMU_FLAGS_DEBUG = \
	-debugcon file:debug.log \
	-d int,cpu_reset \
	-D qemu_debug.log \
	-s -S

# Flags para KVM: -enable-kvm usa la aceleración, -cpu host expone la CPU
# real del host (con SMEP, SMAP, VT-x, etc.). Con esto, INIT-SIPI-SIPI
# funciona igual que en hardware real.
QEMU_FLAGS_KVM = \
	-enable-kvm \
	-cpu host

# ---------------------------------------------------------------------------
# Almacenamiento
#
# aurora.img SOLO debe aparecer UNA VEZ por target. El patrón correcto es:
#   -drive if=none,id=DISK,format=raw,file=IMAGE
#   -device ide-hd,drive=DISK,bus=BUS[,unit=N]
#
# En `-machine pc` (default, PIIX3): los buses ide.0 (primario) e ide.1
# (secundario) admiten DOS unidades cada uno (master=unit 0, slave=unit 1).
#
# En `-machine q35` (ICH9 AHCI): los buses ide.0 .. ide.5 son los 6 puertos
# SATA del controlador integrado, y cada uno admite UN SOLO dispositivo
# (unit 0). No hay un id= asignable para el controlador integrado; solo
# existe si añades `-device ich9-ahci,id=ahciN`, y en ese caso tu driver
# NO lo ve (pci_find_device devuelve el built-in primero).
# ---------------------------------------------------------------------------

# --- PIIX3 (default `pc`) ---
#   ide.0 unit 0 → aurora.img    (hda)
#   ide.1 unit 0 → aurora.iso    (sr0)
#   ide.1 unit 1 → test_disk.img (hdb)
QEMU_STORAGE_PC = \
	-drive if=none,id=disk0,format=raw,file=aurora.img \
	-device ide-hd,drive=disk0,bus=ide.0,unit=0 \
	-drive if=none,id=cdrom0,format=raw,file=aurora.iso \
	-device ide-cd,drive=cdrom0,bus=ide.1,unit=0 \
	-drive if=none,id=testdisk,format=raw,file=tests/test_disk.img \
	-device ide-hd,drive=testdisk,bus=ide.1,unit=1

# --- Q35 (ICH9 AHCI) ---
#   ide.0 → aurora.img         (sda)
#   ide.1 → tests/test_disk.img (sdb)
#   ide.2 → aurora.iso         (ignorado: el driver AHCI no soporta ATAPI)
QEMU_STORAGE_Q35 = \
	-drive if=none,id=disk0,format=raw,file=aurora.img \
	-device ide-hd,drive=disk0,bus=ide.0 \
	-drive if=none,id=testdisk,format=raw,file=tests/test_disk.img \
	-device ide-hd,drive=testdisk,bus=ide.1 \
	-drive if=none,id=cdrom0,format=raw,file=aurora.iso \
	-device ide-cd,drive=cdrom0,bus=ide.2

# ---------------------------------------------------------------------------
# AHCI (Fase 3)
#
# El controlador AHCI en QEMU lo expone la máquina `q35` (ICH9). En
# `-machine pc` (default) solo hay PIIX3 + IDE, y el driver AHCI se
# queda sin HBA. Por eso estos targets fuerzan `-machine q35`.
#
# En Q35, el CD-ROM se conecta a un puerto SATA distinto del disco duro.
# Usamos `-drive if=none,id=...` + `-device ide-hd/ide-cd,bus=...` para
# controlar exactamente qué puerto ocupa cada uno.
#
# Layout por defecto:
#   ahci.0 (puerto 0) → disco duro   → sda
#   ahci.1 (puerto 1) → CD-ROM       → sr0 (por ahora el driver AHCI no
#                                       detecta ATAPI, así que sr0 sigue
#                                       viniendo del PIIX3 si estuviera)
#
# Nota: el driver AHCI actual NO soporta ATAPI todavía. El CD-ROM en Q35
# no aparecerá como sr0. Si quieres CD, usa los targets sin `-ahci`.
# ---------------------------------------------------------------------------

QEMU_FLAGS_Q35_AHCI = \
	-machine q35 \
	-device ahci,id=ahci \
	-drive if=none,id=disk0,format=raw,file=aurora.img \
	-device ide-hd,drive=disk0,bus=ahci.0


# ---------------------------------------------------------------------------
# TCG (emulación pura por software, sin KVM)
# ---------------------------------------------------------------------------

# 1 CPU, sin debug.
run: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_STORAGE_PC) \
		-cpu max \
		-serial stdio

# 1 CPU con debug.
run-debug: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_STORAGE_PC) \
		$(QEMU_FLAGS_DEBUG) \
		-cpu max \
		-serial file:serial.log

# 4 CPUs sin debug (TCG, no arrancará APs).
run-smp: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_STORAGE_PC) \
		-cpu max \
		-smp 4 \
		-serial stdio

# 4 CPUs con debug (TCG, no arrancará APs, solo para diagnóstico).
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
# KVM (virtualización por hardware). ESTE es el que quieres para SMP.
# ---------------------------------------------------------------------------

# 8 CPUs con KVM. El que quieres para probar SMP de verdad.
run-smp-kvm: iso
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_STORAGE_PC) \
		$(QEMU_FLAGS_KVM) \
		-smp 8 \
		-serial stdio

# 8 CPUs con KVM y debug. El que quieres para depurar SMP con logs.
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

# 4 CPUs + AHCI + KVM. El target para probar AHCI en SMP con 2 discos.
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

# 1 CPU + AHCI + KVM. Para aislar problemas de SMP.
run-kvm-ahci: iso tests/test_disk.img
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
		-machine q35 \
		$(QEMU_FLAGS_COMMON) \
		$(QEMU_FLAGS_KVM) \
		$(QEMU_STORAGE_Q35) \
		-serial stdio

# 4 CPUs + AHCI + 3 discos (aurora.img, test_disk.img, extra_disk.img).
# El tercer disco ocupa ide.3. Si no existe extra_disk.img, se crea de
# 32 MB vacío (sin GPT) para probar el caso "disco sin tabla".
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

# Arrancar solo desde el CD (sin disco duro).
# Útil para probar que el sistema arranca desde un Live CD sin necesidad
# de un disco. NO tiene hda, así que los tests de ATA se saltan.
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
	rm -rf sysroot kernel/initrd.tar aurora.img aurora.iso esp
	rm -f serial.log debug.log qemu_debug.log