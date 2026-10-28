# Compila un driver (o un .kur de prueba) contra la capa LinuxKPI.
#   make -f kernel/linuxkpi/drv.mk DRV=<nombre> [install]
# El descriptor es kernel/linuxkpi/drivers/<nombre>.mk o
# tests/linuxkpi/<nombre>/drv.mk y define:
#   LKPI_DRV_SRCDIR   directorio de las fuentes (p. ej. $(LKPI_LINUX)/drivers/gpu/drm/tiny)
#   LKPI_DRV_SRCS     ficheros .c relativos a LKPI_DRV_SRCDIR
#   LKPI_DRV_CFLAGS   flags extra (opcional)
#   LKPI_DRV_INSTALL  subdirectorio de /lib/modules (por defecto linuxkpi)
#   LKPI_DRV_LOCAL_SRCS  .c del repo (relativos a la raiz) que se enlazan tambien
#   LKPI_DRV_CFLAGS_<f.c> flags extra solo para ese fichero (p. ej. renombrar un
#                        module_init)
# El resultado es build/linuxkpi/drv/<nombre>/<nombre>.kur: las fuentes, mas
# el manifiesto .kur, enlazados con ld -r y sellados con kurbuild -H.
KUR_CC ?= gcc
include kernel/linuxkpi/flags.mk
include kernel/linuxkpi/linux-version.mk

# Arbol de Linux fijado (scripts/fetch-linux-drivers.sh).
LKPI_LINUX ?= build/linuxkpi_src/linux

ifeq ($(DRV),)
$(error falta DRV=<nombre>)
endif
LKPI_DRV_DESC := $(firstword $(wildcard kernel/linuxkpi/drivers/$(DRV).mk tests/linuxkpi/$(DRV)/drv.mk))
ifeq ($(LKPI_DRV_DESC),)
$(error no hay descriptor para $(DRV): kernel/linuxkpi/drivers/$(DRV).mk ni tests/linuxkpi/$(DRV)/drv.mk)
endif
LKPI_DRV_INSTALL := linuxkpi
include $(LKPI_DRV_DESC)

D       := build/linuxkpi/drv/$(DRV)
OBJS    := $(patsubst %.c,$(D)/%.o,$(LKPI_DRV_SRCS)) $(D)/__kur_manifest.o \
           $(patsubst %.c,$(D)/local/%.o,$(LKPI_DRV_LOCAL_SRCS))
HDRS    := $(shell find include/linuxkpi -name '*.h') $(wildcard tests/linuxkpi/*.h)
CFLAGS  := $(LKPI_CFLAGS) $(LKPI_DRV_CFLAGS) -DKBUILD_MODNAME='"$(DRV)"' \
           -I$(LKPI_DRV_SRCDIR)

.PHONY: all install
all: $(D)/$(DRV).kur

# Con parche (kernel/linuxkpi/patches/<fichero>.patch): copia + patch; sin
# parche: el fichero del arbol.
$(D)/%.o: $(LKPI_DRV_SRCDIR)/%.c $(HDRS) $(LKPI_DRV_DESC) $(wildcard kernel/linuxkpi/patches/*.patch)
	@mkdir -p $(dir $@)
	@echo "[LKPI-DRV] $(DRV): $<"
	@src=$<; \
	if [ -f kernel/linuxkpi/patches/$(notdir $<).patch ]; then \
		mkdir -p $(D)/src; cp $< $(D)/src/$(notdir $<); \
		echo "[LKPI-PATCH] $(notdir $<)"; \
		patch -s $(D)/src/$(notdir $<) kernel/linuxkpi/patches/$(notdir $<).patch || exit 1; \
		src=$(D)/src/$(notdir $<); fi; \
	$(LKPI_CC) $(CFLAGS) -I$(dir $<) $(LKPI_DRV_CFLAGS_$(notdir $<)) -c $$src -o $@

$(D)/local/%.o: %.c $(HDRS) $(LKPI_DRV_DESC)
	@mkdir -p $(dir $@)
	@echo "[LKPI-DRV] $(DRV): $<"
	@$(LKPI_CC) $(CFLAGS) $(LKPI_DRV_CFLAGS_$(notdir $<)) -c $< -o $@

$(D)/__kur_manifest.o: kernel/linuxkpi/kur_manifest.c $(HDRS)
	@mkdir -p $(D)
	@$(LKPI_CC) $(CFLAGS) -c $< -o $@

$(D)/$(DRV).kur: $(OBJS) tools/kurbuild
	@$(LKPI_LD) -r -o $(D)/$(DRV).o $(OBJS)
	@./tools/kurbuild -H $(D)/$(DRV).o $@
	@echo "[KURBUILD] $(DRV) ($$(stat -c %s $@) bytes)"

install: $(D)/$(DRV).kur
	@mkdir -p sysroot/lib/modules/$(LKPI_DRV_INSTALL)
	@cp -f $< sysroot/lib/modules/$(LKPI_DRV_INSTALL)/$(DRV).kur
