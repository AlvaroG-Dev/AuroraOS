#!/bin/bash
# Compila aurora-wayland (servidor Wayland nativo de Aurora) en el host contra la
# libwayland-server/libxkbcommon de la distro (ver scripts/fetch-wayland.sh) y lo
# deja en precompiled/usr/bin/. Necesita wayland-scanner y wayland-protocols
# (libwayland-bin, wayland-protocols, libxkbcommon-dev extraidos en el stage).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ST="$ROOT/build/wayland_stage/root/usr"
GEN="$ROOT/build/aurora_wl"
SCAN="$ST/bin/wayland-scanner"
PROTO="$ST/share/wayland-protocols"
mkdir -p "$GEN" "$ROOT/precompiled/usr/bin"
gen() { # xml nombre
    "$SCAN" server-header "$1" "$GEN/$2-server-protocol.h"
    "$SCAN" private-code "$1" "$GEN/$2-protocol.c"
}
gen "$PROTO/stable/xdg-shell/xdg-shell.xml" xdg-shell
gen "$PROTO/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml" xdg-decoration-unstable-v1
gen "$ROOT/user/aurora-wayland/org-kde-kwin-server-decoration.xml" org-kde-kwin-server-decoration
gen "$PROTO/stable/viewporter/viewporter.xml" viewporter
gen "$PROTO/staging/fractional-scale/fractional-scale-v1.xml" fractional-scale-v1
gen "$PROTO/unstable/primary-selection/primary-selection-unstable-v1.xml" primary-selection-unstable-v1
gen "$PROTO/staging/cursor-shape/cursor-shape-v1.xml" cursor-shape-v1
gen "$PROTO/stable/tablet/tablet-v2.xml" tablet-v2
gen "$PROTO/unstable/xdg-output/xdg-output-unstable-v1.xml" xdg-output-unstable-v1
gen "$PROTO/stable/presentation-time/presentation-time.xml" presentation-time
gen "$PROTO/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml" idle-inhibit-unstable-v1
gen "$PROTO/staging/xdg-activation/xdg-activation-v1.xml" xdg-activation-v1
gen "$PROTO/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml" linux-dmabuf-unstable-v1
gcc -O2 -Wall -Wextra -Wno-unused-parameter -o "$ROOT/precompiled/usr/bin/aurora-wayland" \
    "$ROOT/user/aurora-wayland/aurora-wayland.c" "$ROOT/user/aurora-wayland/icons.c" "$GEN/xdg-shell-protocol.c" \
    "$GEN/xdg-decoration-unstable-v1-protocol.c" \
    "$GEN/org-kde-kwin-server-decoration-protocol.c" \
    "$GEN/viewporter-protocol.c" "$GEN/fractional-scale-v1-protocol.c" \
    "$GEN/primary-selection-unstable-v1-protocol.c" \
    "$GEN/cursor-shape-v1-protocol.c" "$GEN/tablet-v2-protocol.c" \
    "$GEN/xdg-output-unstable-v1-protocol.c" "$GEN/presentation-time-protocol.c" \
    "$GEN/idle-inhibit-unstable-v1-protocol.c" "$GEN/xdg-activation-v1-protocol.c" \
    "$GEN/linux-dmabuf-unstable-v1-protocol.c" \
    -I"$GEN" -I"$ST/include" -L"$ST/lib/x86_64-linux-gnu" \
    -lwayland-server -l:libxkbcommon.so.0 -lxcb -lxcb-composite -lxcb-xfixes -lpthread -lpng16 -ldl -lm -Wl,-rpath-link,"$ST/lib/x86_64-linux-gnu"
echo "OK: precompiled/usr/bin/aurora-wayland"
