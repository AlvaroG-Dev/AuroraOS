#!/bin/bash
# Compila el modulo GTK3 de Aurora (cambio de tema en vivo) en el host contra la
# GTK3 de la distro (misma release que el sysroot) y lo deja en precompiled/.
# Necesita: libgtk-3-dev.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/precompiled/usr/lib/aurora-gtk/modules"
mkdir -p "$OUT"
gcc -O2 -fPIC -shared -Wall -o "$OUT/libaurora-theme.so" \
    "$ROOT/user/aurora-gtk-module/aurora-theme-module.c" \
    $(pkg-config --cflags --libs gtk+-3.0)
echo "OK: $OUT/libaurora-theme.so"
