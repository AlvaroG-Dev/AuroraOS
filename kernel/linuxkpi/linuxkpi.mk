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
linuxkpi: $(LKPI_BUILD)/linuxkpi.kur linuxkpi-tests linuxkpi-firmware
	@mkdir -p $(LKPI_MODDIR)
	@cp -f $(LKPI_BUILD)/linuxkpi.kur $(LKPI_MODDIR)/linuxkpi.kur

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

linuxkpi-clean:
	rm -rf $(LKPI_BUILD) $(LKPI_MODDIR)
