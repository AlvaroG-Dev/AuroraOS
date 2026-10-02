# user/musl/Makefile — Userland POSIX compilado con musl.

MUSL_CC := ../../toolchain/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc
SYSDIR  := ../../sysroot/apps

MUSL_CFLAGS := -static -no-pie -Os -fno-stack-protector \
               -ffunction-sections -fdata-sections \
               -Wl,--gc-sections -pthread

# Auto-detecta apps/<name>/main.c
MUSL_APPS := $(patsubst apps/%/main.c,%,$(wildcard apps/*/main.c))

all: $(MUSL_APPS)

$(MUSL_APPS): %: apps/%/main.c
	@if [ ! -x "$(MUSL_CC)" ]; then \
		echo "SKIP: falta toolchain musl en $(MUSL_CC)"; exit 1; \
	fi
	@mkdir -p $(SYSDIR)
	$(MUSL_CC) $(MUSL_CFLAGS) -o $@ $<
	cp $@ $(SYSDIR)/
	@file $(SYSDIR)/$@ | sed 's/^/  /'

clean:
	rm -f $(MUSL_APPS)
	rm -f $(addprefix $(SYSDIR)/,$(MUSL_APPS))

.PHONY: all clean