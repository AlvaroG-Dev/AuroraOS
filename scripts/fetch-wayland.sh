#!/bin/bash
# scripts/fetch-wayland.sh — Descarga de los repos de Ubuntu (sin compilar nada)
# las librerias Wayland/DRM/input y las deja en libs/glibc/ (layout que usa el
# Makefile). Tambien extrae las cabeceras de libwayland-dev en
# build/wayland_stage/ para compilar los tests (tests/wayland/).
#
# Uso:  scripts/fetch-wayland.sh [paquete-extra ...]
#
# Las librerias son las de la distro, SIN modificar: el trabajo de Aurora es
# el kernel (ABI Linux), no portar estas bibliotecas.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$ROOT/build/wayland_stage"
DEBS="$STAGE/debs"
EXTRACT="$STAGE/root"
LIBDIR="$ROOT/libs/glibc/lib/x86_64-linux-gnu"
SHARE="$ROOT/precompiled/usr/share"

PKGS=(
    libwayland-client0 libwayland-server0 libwayland-cursor0 libwayland-egl1
    libwayland-dev
    libffi8
    libxkbcommon0 xkb-data
    libpixman-1-0
    libdrm2 libdrm-common
    libdrm-tests drm-info libjson-c5 libpci3
    libinput10 libevdev2 libmtdev1t64 libudev-dev libinput-bin libinput-tools libwacom9 libwacom-common
    "$@"
)

mkdir -p "$DEBS" "$EXTRACT" "$LIBDIR" "$SHARE"
cd "$DEBS"
for p in "${PKGS[@]}"; do
    if ! ls "${p}"_*.deb >/dev/null 2>&1; then
        echo "  apt-get download $p"
        apt-get download "$p" >/dev/null
    fi
done
for d in *.deb; do
    dpkg -x "$d" "$EXTRACT"
done

# Librerias (.so*) al layout glibc del sysroot.
find "$EXTRACT/usr/lib/x86_64-linux-gnu" "$EXTRACT/lib/x86_64-linux-gnu" \
    -maxdepth 1 \( -type f -o -type l \) -name '*.so*' 2>/dev/null |
while read -r f; do
    base="$(basename "$f")"
    cp -Lf "$f" "$LIBDIR/$base"
    echo "  lib: $base"
done || true

# Datos (teclados xkb, etc.)
if [ -d "$EXTRACT/usr/share/X11/xkb" ]; then
    mkdir -p "$SHARE/X11"
    rm -rf "$SHARE/X11/xkb"
    cp -r "$EXTRACT/usr/share/X11/xkb" "$SHARE/X11/xkb"
    echo "  data: /usr/share/X11/xkb"
fi

echo "OK. Cabeceras: $EXTRACT/usr/include   (libs: $EXTRACT/usr/lib/x86_64-linux-gnu)"

# Herramientas de libinput (sin tocar) + sus datos (quirks) para probar el input.
mkdir -p "$ROOT/precompiled/usr/bin" "$SHARE"
for t in libinput-debug-events libinput-list-devices; do
    if [ -f "$EXTRACT/usr/libexec/libinput/$t" ]; then
        cp -f "$EXTRACT/usr/libexec/libinput/$t" "$ROOT/precompiled/usr/bin/$t"
        echo "  bin: $t"
    fi
done
if [ -d "$EXTRACT/usr/share/libinput" ]; then
    rm -rf "$SHARE/libinput"
    cp -r "$EXTRACT/usr/share/libinput" "$SHARE/libinput"
fi
# Dependencias que no estuvieran ya en libs/glibc (glib/gudev via libwacom...).
bash "$ROOT/scripts/wayland-deps.sh" --copy \
    "$EXTRACT/usr/libexec/libinput/libinput-debug-events" \
    "$EXTRACT/usr/libexec/libinput/libinput-list-devices" \
    "$EXTRACT/usr/lib/x86_64-linux-gnu/libinput.so.10" \
    "$EXTRACT/usr/lib/x86_64-linux-gnu/libwacom.so.9" | grep -v '^have' || true

# Herramientas DRM de la distro (sin tocar): modetest (libdrm-tests) y drm_info.
for t in usr/bin/modetest usr/bin/drm_info; do
    if [ -f "$EXTRACT/$t" ]; then
        cp -f "$EXTRACT/$t" "$ROOT/precompiled/usr/bin/$(basename "$t")"
        echo "  bin: $(basename "$t")"
    fi
done
bash "$ROOT/scripts/wayland-deps.sh" --copy \
    "$EXTRACT/usr/bin/modetest" "$EXTRACT/usr/bin/drm_info" | grep -v '^have' || true
