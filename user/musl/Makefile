# user/musl/Makefile — Userland POSIX compilado con musl.

MUSL_CC := ../../toolchain/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc
SYSDIR  := ../../sysroot
STAGE   := python3 ../../scripts/stage_elf.py

MUSL_CFLAGS := -static -no-pie -Os -fno-stack-protector \
               -ffunction-sections -fdata-sections \
               -Wl,--gc-sections -pthread

# Auto-detecta apps/<name>/main.c
MUSL_APPS := $(patsubst apps/%/main.c,%,$(wildcard apps/*/main.c))

# El paso de instalación es independiente de la compilación: aunque el
# binario ya exista y make no tenga que recompilarlo, siempre debe copiarse
# al sysroot que posteriormente se empaqueta en initrd.tar.
all: $(MUSL_APPS) install

install: $(MUSL_APPS)
	@for app in $(MUSL_APPS); do \
		$(STAGE) $$app $(SYSDIR) /bin; \
	done
	@for app in $(MUSL_APPS); do \
		file $(SYSDIR)/bin/$$app | sed 's/^/  /'; \
	done

.PHONY: FORCE
FORCE:

$(MUSL_APPS): %: apps/%/main.c FORCE
	@if [ ! -x "$(MUSL_CC)" ]; then \
		echo "SKIP: falta toolchain musl en $(MUSL_CC)"; exit 1; \
	fi
	@mkdir -p $(SYSDIR)
	$(MUSL_CC) $(MUSL_CFLAGS) -o $@ $<

clean:
	rm -f $(MUSL_APPS)
	rm -f $(addprefix $(SYSDIR)/bin/,$(MUSL_APPS))

.PHONY: all install clean
