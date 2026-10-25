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
gcc -O2 -Wall -Wextra -Wno-unused-parameter -o "$ROOT/precompiled/usr/bin/aurora-wayland" \
    "$ROOT/user/aurora-wayland/aurora-wayland.c" "$GEN/xdg-shell-protocol.c" \
    "$GEN/xdg-decoration-unstable-v1-protocol.c" \
    "$GEN/org-kde-kwin-server-decoration-protocol.c" \
    -I"$GEN" -I"$ST/include" -L"$ST/lib/x86_64-linux-gnu" \
    -lwayland-server -l:libxkbcommon.so.0 -Wl,-rpath-link,"$ST/lib/x86_64-linux-gnu"
echo "OK: precompiled/usr/bin/aurora-wayland"
