# LinuxKPI: la capa (build/linuxkpi/linuxkpi.kur) y sus .kur de prueba.
# Lo incluye el Makefile principal. Plan_LinuxKPI.md (L0.4, L0.5, L1).
include kernel/linuxkpi/flags.mk
include kernel/linuxkpi/linux-version.mk

LKPI_BUILD   := build/linuxkpi
LKPI_SRCS    := $(wildcard kernel/linuxkpi/*.c kernel/linuxkpi/kernel/*.c kernel/linuxkpi/lib/*.c \
                           kernel/linuxkpi/mm/*.c kernel/linuxkpi/base/*.c kernel/linuxkpi/pci/*.c \
                           kernel/linuxkpi/fs/*.c kernel/linuxkpi/dma/*.c kernel/linuxkpi/misc/*.c)
LKPI_SRCS    := $(filter-out kernel/linuxkpi/kur_manifest.c,$(LKPI_SRCS))
LKPI_OBJS    := $(patsubst kernel/linuxkpi/%.c,$(LKPI_BUILD)/obj/%.o,$(LKPI_SRCS))

# Codigo de Linux que la capa compila tal cual (fences y dma_resv, K7), desde
# el arbol fijado (scripts/fetch-linux-drivers.sh; no se versiona). Los
# simbolos se renombran con prefijo lkpi_ en las cabeceras. Un parche minimo
# versionado en kernel/linuxkpi/patches/<fichero>.patch se aplica a una copia
# en build/ (el arbol descargado no se toca).
LKPI_LINUX      ?= build/linuxkpi_src/linux
LKPI_EXT_SRCS   := drivers/dma-buf/dma-fence.c drivers/dma-buf/dma-fence-array.c \
                   drivers/dma-buf/dma-fence-chain.c drivers/dma-buf/dma-fence-unwrap.c \
                   drivers/dma-buf/dma-resv.c drivers/dma-buf/sync_file.c
LKPI_EXT_OBJS   := $(patsubst %.c,$(LKPI_BUILD)/obj/ext/%.o,$(LKPI_EXT_SRCS))
LKPI_OBJS       += $(LKPI_EXT_OBJS)
.SECONDARY: $(patsubst %.c,$(LKPI_BUILD)/ext-src/%.c,$(LKPI_EXT_SRCS))

# Con parche: copia + patch; sin parche: el fichero del arbol.
$(LKPI_BUILD)/ext-src/%.c: $(LKPI_LINUX)/%.c
	@test -f $< || { echo "falta $< : ejecuta scripts/fetch-linux-drivers.sh"; exit 1; }
	@mkdir -p $(dir $@)
	@cp $< $@
	@if [ -f kernel/linuxkpi/patches/$(notdir $<).patch ]; then \
		echo "[LKPI-PATCH] $(notdir $<)"; \
		patch -s $@ kernel/linuxkpi/patches/$(notdir $<).patch || exit 1; fi

$(LKPI_BUILD)/obj/ext/%.o: $(LKPI_BUILD)/ext-src/%.c $(LKPI_HDRS) kernel/linuxkpi/flags.mk $(wildcard kernel/linuxkpi/patches/*.patch)
	@mkdir -p $(dir $@)
	@echo "[LKPI-CC] $(patsubst $(LKPI_BUILD)/ext-src/%,%,$<) (Linux)"
	@$(LKPI_CC) $(LKPI_CFLAGS) -DKBUILD_MODNAME='"linuxkpi"' -I$(dir $(LKPI_LINUX)/$*.c) -c $< -o $@
LKPI_HDRS    := $(shell find include/linuxkpi kernel/linuxkpi -name '*.h' 2>/dev/null)
LKPI_TESTS   := $(patsubst tests/linuxkpi/%/drv.mk,%,$(wildcard tests/linuxkpi/*/drv.mk))
LKPI_MODDIR  := sysroot/lib/modules/linuxkpi

$(LKPI_BUILD)/obj/%.o: kernel/linuxkpi/%.c $(LKPI_HDRS) kernel/linuxkpi/flags.mk
	@mkdir -p $(dir $@)
	@echo "[LKPI-CC] $<"
	@$(LKPI_CC) $(LKPI_CFLAGS) -DKBUILD_MODNAME='"linuxkpi"' -c $< -o $@

$(LKPI_BUILD)/linuxkpi.o: $(LKPI_OBJS)
	@$(LKPI_LD) -r -o $@ $^

$(LKPI_BUILD)/linuxkpi.kur: $(LKPI_BUILD)/linuxkpi.o tools/kurbuild
	@echo "[KURBUILD] linuxkpi"
	@./tools/kurbuild -H $< $@

.PHONY: linuxkpi linuxkpi-tests linuxkpi-drv linuxkpi-clean linuxkpi-firmware
linuxkpi: $(LKPI_BUILD)/linuxkpi.kur linuxkpi-tests linuxkpi-modules linuxkpi-firmware
	@mkdir -p $(LKPI_MODDIR)
	@cp -f $(LKPI_BUILD)/linuxkpi.kur $(LKPI_MODDIR)/linuxkpi.kur

# Modulos de infraestructura de Linux sobre la capa (kernel/linuxkpi/drivers/<n>.mk):
# el nucleo virtio (L5) y, mas adelante, el de DRM. Se cargan bajo demanda.
LKPI_MODULES := lkpi_virtio lkpi_drm lkpi_ttm lkpi_drm_display lkpi_i915 lkpi_bochs lkpi_simpledrm lkpi_virtio_gpu
.PHONY: linuxkpi-modules
linuxkpi-modules: tools/kurbuild
	@for m in $(LKPI_MODULES); do \
		$(MAKE) --no-print-directory -f kernel/linuxkpi/drv.mk DRV=$$m \
			KUR_CC="$(KUR_CC)" install || exit 1; \
	done

# Ficheros de linux-firmware para el initrd (kernel/linuxkpi/firmware.list).
linuxkpi-firmware:
	@scripts/install-linux-firmware.sh sysroot

linuxkpi-tests: tools/kurbuild
	@for t in $(LKPI_TESTS); do \
		$(MAKE) --no-print-directory -f kernel/linuxkpi/drv.mk DRV=$$t \
			KUR_CC="$(KUR_CC)" install || exit 1; \
	done

# make linuxkpi-drv DRV=<nombre>: compila el driver descrito en
# kernel/linuxkpi/drivers/<nombre>.mk (o tests/linuxkpi/<nombre>/drv.mk).
linuxkpi-drv: tools/kurbuild
	@test -n "$(DRV)" || { echo "uso: make linuxkpi-drv DRV=<nombre>"; exit 2; }
	@$(MAKE) --no-print-directory -f kernel/linuxkpi/drv.mk DRV=$(DRV) KUR_CC="$(KUR_CC)" install

# make nvidia: driver NVIDIA open (Plan_NVIDIA.md N2) -> sysroot/lib/modules/linuxkpi/nvidia.kur.
# Descarga las fuentes y compila nv-kernel.o si faltan; no va en
# `linuxkpi` (la primera vez tarda). Se carga con lkpi.load=linuxkpi,nvidia.
.PHONY: nvidia nvidia-test nvidia-test-off
nvidia: tools/kurbuild $(LKPI_BUILD)/linuxkpi.kur
	@[ -d build/linuxkpi_src/nvidia-open/src ] || scripts/fetch-linux-drivers.sh
	@scripts/nvidia-src-patch.sh
	@[ -f build/linuxkpi_src/nvidia-open/src/nvidia/_out/Linux_x86_64/nv-kernel.o ] || \
		$(MAKE) --no-print-directory -C build/linuxkpi_src/nvidia-open/src/nvidia -j$$(nproc)
	@[ build/nvidia-kpi/out/conftest/functions.h -nt scripts/nvidia-conftest.sh ] || scripts/nvidia-conftest.sh
	@scripts/nvidia-kpi-cc.sh | grep -E "^compilan" 
	@scripts/nvidia-link.sh | grep "^OK"
	@mkdir -p $(LKPI_MODDIR)
	@cp -f $(LKPI_BUILD)/linuxkpi.kur $(LKPI_MODDIR)/linuxkpi.kur
	@cp -f build/nvidia-kpi/nvidia.kur $(LKPI_MODDIR)/nvidia.kur
	@[ -f build/linuxkpi_src/nvidia-open/src/nvidia-modeset/_out/Linux_x86_64/nv-modeset-kernel.o ] || \
		$(MAKE) --no-print-directory -C build/linuxkpi_src/nvidia-open/src/nvidia-modeset -j$$(nproc) >/dev/null
	@scripts/nvidia-modeset-cc.sh | grep -E "^compilan"
	@scripts/nvidia-modeset-link.sh | grep "^OK"
	@cp -f build/nvidia-kpi/ms/nvidia-modeset.kur $(LKPI_MODDIR)/nvidia-modeset.kur
	@$(MAKE) --no-print-directory -f kernel/linuxkpi/drv.mk DRV=lkpi_drm KUR_CC="$(KUR_CC)" install
	@scripts/nvidia-drm-cc.sh > build/nvidia-kpi/drm/cc.log || { head -20 build/nvidia-kpi/drm/obj/errors.txt; exit 1; }; grep -E "^compilan" build/nvidia-kpi/drm/cc.log
	@scripts/nvidia-drm-link.sh | grep "^OK"
	@cp -f build/nvidia-kpi/drm/nvidia-drm.kur $(LKPI_MODDIR)/nvidia-drm.kur
	@[ -f build/nvidia/firmware/gsp_ga10x.bin ] || scripts/fetch-nvidia.sh
	@mkdir -p sysroot/lib/firmware/nvidia/$(NVIDIA_OPEN_VERSION)
	@cp -f build/nvidia/firmware/gsp_ga10x.bin build/nvidia/firmware/ucodes_ga10x.bin sysroot/lib/firmware/nvidia/$(NVIDIA_OPEN_VERSION)/
	@mkdir -p sysroot/usr/bin
	@$(or $(MUSL_CC),musl-gcc) -static -O1 -Wall -o sysroot/usr/bin/nvctl_test tests/nvidia/nvctl_test.c -Ibuild/linuxkpi_src/nvidia-open/kernel-open/common/inc -Ibuild/linuxkpi_src/nvidia-open/src/common/sdk/nvidia/inc
	@rm -f kernel/initrd.tar
	@echo "[NVIDIA] instalado en $(LKPI_MODDIR)/nvidia.kur (arranca con lkpi.load=linuxkpi,nvidia)"

# Prueba de /dev/nvidiactl al arrancar (sin Wayland; la salida va a aurora.log
# por /dev/kmsg). nvidia-test-off la quita para volver al arranque normal.
nvidia-test: nvidia
	@mkdir -p sysroot/system
	@echo /usr/bin/nvctl_test > sysroot/system/boot-tests
	@echo "[NVIDIA] sysroot/system/boot-tests escrito: make iso arrancara nvctl_test sin escritorio"

nvidia-test-off:
	@rm -f sysroot/system/boot-tests kernel/initrd.tar
	@echo "[NVIDIA] boot-tests quitado"

linuxkpi-clean:
	rm -rf $(LKPI_BUILD) $(LKPI_MODDIR)
include kernel/linuxkpi/run.mk
