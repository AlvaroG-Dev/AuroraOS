# Ejecuciones de LinuxKPI en QEMU (make run-lkpi-<config>) y bateria de pruebas
# (make lkpi-regress). Lo incluye linuxkpi.mk. Cada config escribe su propio
# fichero de arranque con `cmdline=` (bootloader/aurora.conf no se toca) y
# reconstruye la ISO con AURORA_CONF=... Un solo QEMU a la vez.
#
#   make help-lkpi                 lista las configs
#   make run-lkpi-bochs            lo que cada una carga y que dispositivo anade
#   make run-lkpi-i915-vfio VFIO_DEV=0000:00:02.0   (necesita KVM, root y la GPU en vfio-pci)
#
# Variables: LKPI_MEM (6G: el initrd de ahora lo exige), LKPI_SMP (4),
# LKPI_DISPLAY (vacio = ventana de QEMU; `-display none` para sin pantalla).
LKPI_MEM     ?= 6G
LKPI_SMP     ?= 4
LKPI_DISPLAY ?=
LKPI_RUNDIR  := build/lkpi-run
LKPI_BASE    := linuxkpi
LKPI_DRMSET  := linuxkpi,lkpi_drm
LKPI_QEMU    := $(QEMU_ACCEL) -m $(LKPI_MEM) -smp $(LKPI_SMP) $(LKPI_DISPLAY) -serial stdio \
                -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
                -drive if=pflash,format=raw,file=OVMF_VARS.fd -no-reboot -no-shutdown $(QEMU_NET_SLIRP)

LKPI_RUN_CONFIGS := base native bochs simpledrm virtio-gpu virtio-gpu2 ttm i915 i915-vfio edu

# Linea del kernel de cada config
LKPI_CMD_base        := lkpi.load=linuxkpi
LKPI_CMD_native      := drm.backend=native lkpi.load=linuxkpi,lkpi_drm,lkpi_bochs
LKPI_CMD_bochs       := aur.skip=1234:1111 lkpi.load=linuxkpi,lkpi_drm,lkpi_bochs
LKPI_CMD_simpledrm   := lkpi.load=linuxkpi,lkpi_drm,lkpi_simpledrm
LKPI_CMD_virtio-gpu  := aur.skip=1af4:1050 lkpi.load=linuxkpi,lkpi_virtio,lkpi_drm,lkpi_virtio_gpu
LKPI_CMD_virtio-gpu2 := $(LKPI_CMD_virtio-gpu)
LKPI_CMD_ttm         := aur.skip=1234:1111 lkpi.load=linuxkpi,lkpi_drm,lkpi_ttm,lkpi_bochs
LKPI_CMD_i915        := lkpi.load=linuxkpi,lkpi_drm,lkpi_ttm,lkpi_drm_display,lkpi_i915
LKPI_CMD_i915-vfio   := $(LKPI_CMD_i915)
LKPI_CMD_edu         := lkpi.load=linuxkpi

# Dispositivos de QEMU de cada config
LKPI_DEV_base        := -vga std
LKPI_DEV_native      := -vga std
LKPI_DEV_bochs       := -vga std
LKPI_DEV_simpledrm   := -vga std
LKPI_DEV_virtio-gpu  := -vga none -device virtio-gpu-pci
LKPI_DEV_virtio-gpu2 := -vga none -device virtio-gpu-pci,max_outputs=2
LKPI_DEV_ttm         := -vga std
LKPI_DEV_i915        := -vga std
LKPI_DEV_i915-vfio   := -vga none -device vfio-pci,host=$(VFIO_DEV)
LKPI_DEV_edu         := -vga std -device edu -device virtio-rng-pci

$(LKPI_RUNDIR)/%.conf: FORCE
	@mkdir -p $(LKPI_RUNDIR)
	@{ grep -v '^cmdline=' bootloader/aurora.conf; echo 'cmdline=$(LKPI_CMD_$*)'; } > $@.new
	@cmp -s $@.new $@ && rm -f $@.new || mv -f $@.new $@
FORCE:

define LKPI_RUN_RULE
.PHONY: run-lkpi-$(1)
run-lkpi-$(1): $(LKPI_RUNDIR)/$(1).conf
	@$(if $(filter i915-vfio,$(1)),test -n "$(VFIO_DEV)" || { echo "uso: make run-lkpi-i915-vfio VFIO_DEV=0000:00:02.0"; exit 2; },true)
	@$(MAKE) --no-print-directory iso AURORA_CONF=$(LKPI_RUNDIR)/$(1).conf
	cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 $$(LKPI_QEMU) $$(LKPI_DEV_$(1)) -cdrom $$(ISO_OUT)
endef
$(foreach c,$(LKPI_RUN_CONFIGS),$(eval $(call LKPI_RUN_RULE,$(c))))

# --- Baterias headless (scripts/qemu-drive.py): salen con != 0 si algo falla ----
.PHONY: lkpi-suite lkpi-test lkpi-drm-test lkpi-userland lkpi-regress help-lkpi
lkpi-suite:
	scripts/lkpi-regress.sh suite
lkpi-test:
	scripts/lkpi-regress.sh lkpi_test
lkpi-drm-test:
	scripts/lkpi-regress.sh drm_test
lkpi-userland:
	scripts/lkpi-regress.sh userland
lkpi-regress:
	scripts/lkpi-regress.sh all

help-lkpi:
	@echo "Ejecucion interactiva (make run-lkpi-<config>):"
	@$(foreach c,$(LKPI_RUN_CONFIGS),echo "  $(c): $(LKPI_CMD_$(c))  [$(LKPI_DEV_$(c))]";)
	@echo "Pruebas headless: lkpi-suite (kernel, smp 2 y 4), lkpi-test, lkpi-drm-test (4 configs),"
	@echo "                  lkpi-userland (sig/rseq/timer/thread_sig/clipboard/primary-icons), lkpi-regress (todo)"
	@echo "Entorno: scripts/setup-env.sh (WITH_LKPI=1 baja Linux 6.18.59 y linux-firmware)"
