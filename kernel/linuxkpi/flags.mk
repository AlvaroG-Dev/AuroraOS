# Flags comunes de todo lo que se compila contra include/linuxkpi (la capa y
# los drivers de Linux). Como un modulo de Linux x86-64: sin SSE/x87, sin
# red zone, modelo de codigo kernel, y las cabeceras de la capa en lugar de
# las del sistema (-nostdinc).
LKPI_CC      ?= $(KUR_CC)
LKPI_LD      ?= ld
LKPI_INC     := include/linuxkpi
# Cabeceras de interfaz del arbol de Linux (UAPI virtio, drm): ver scripts/lkpi-ext-inc.sh
LKPI_EXTINC  := build/linuxkpi/ext-inc
_LKPI_EXTINC_RUN := $(shell scripts/lkpi-ext-inc.sh >&2)
LKPI_CFLAGS  := -std=gnu11 -nostdinc -ffreestanding -O2 \
                -mcmodel=kernel -mno-red-zone -fno-pic -fno-pie -fno-common \
                -fno-stack-protector -fno-asynchronous-unwind-tables \
                -fno-strict-aliasing -fno-delete-null-pointer-checks -fwrapv \
                -mno-sse -mno-sse2 -mno-mmx -mno-3dnow -mno-avx -mno-80387 \
                -mno-fp-ret-in-387 -ffunction-sections -fdata-sections \
                -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function \
                -Werror=implicit-function-declaration -Werror=incompatible-pointer-types \
                -D__KERNEL__ -DMODULE -I$(LKPI_INC) -idirafter $(LKPI_EXTINC) \
                -include linux/kconfig.h -include linux/compiler_types.h
