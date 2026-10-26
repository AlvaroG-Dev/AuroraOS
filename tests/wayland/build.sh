#!/bin/bash
# Compila los tests Wayland contra las libs de la distro (ver
# scripts/fetch-wayland.sh) y los deja en precompiled/usr/bin/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ST="$ROOT/build/wayland_stage/root/usr"
mkdir -p "$ROOT/precompiled/usr/bin"
gcc -O1 -Wall -o "$ROOT/precompiled/usr/bin/wl_test" "$ROOT/tests/wayland/wl_test.c" \
    -I"$ST/include" -L"$ST/lib/x86_64-linux-gnu" -lwayland-server -lwayland-client
echo "OK: precompiled/usr/bin/wl_test"
gcc -O1 -Wall -o "$ROOT/precompiled/usr/bin/udev_test" "$ROOT/tests/wayland/udev_test.c" \
    -I"$ST/include" -L"$ST/lib/x86_64-linux-gnu" -l:libudev.so.1 \
    -Wl,-rpath-link,"$ST/lib/x86_64-linux-gnu" 2>&1 || {
    # libudev.so.1 del host si el extraido no la trae
    gcc -O1 -Wall -o "$ROOT/precompiled/usr/bin/udev_test" "$ROOT/tests/wayland/udev_test.c" \
        -I"$ST/include" -l:libudev.so.1
}
echo "OK: precompiled/usr/bin/udev_test"
# Cliente de prueba de subsuperficies/viewporter/cursor-shape de aurora-wayland.
GEN="$ROOT/build/aurora_wl_client"
SCAN="$ST/bin/wayland-scanner"
PROTO="$ST/share/wayland-protocols"
mkdir -p "$GEN"
cgen() { "$SCAN" client-header "$1" "$GEN/$2-client-protocol.h"; "$SCAN" private-code "$1" "$GEN/$2-protocol.c"; }
cgen "$PROTO/stable/xdg-shell/xdg-shell.xml" xdg-shell
cgen "$PROTO/stable/viewporter/viewporter.xml" viewporter
cgen "$PROTO/staging/cursor-shape/cursor-shape-v1.xml" cursor-shape-v1
cgen "$PROTO/stable/tablet/tablet-v2.xml" tablet-v2
gcc -O1 -Wall -o "$ROOT/precompiled/usr/bin/aw_sub_test" "$ROOT/tests/wayland/aw_sub_test.c" \
    "$GEN/xdg-shell-protocol.c" "$GEN/viewporter-protocol.c" \
    "$GEN/cursor-shape-v1-protocol.c" "$GEN/tablet-v2-protocol.c" \
    -I"$GEN" -I"$ST/include" -L"$ST/lib/x86_64-linux-gnu" -lwayland-client \
    -Wl,-rpath-link,"$ST/lib/x86_64-linux-gnu"
echo "OK: precompiled/usr/bin/aw_sub_test"
# App GTK3 minima (necesita libgtk-3-dev en el host de build; se omite si falta).
if pkg-config --exists gtk+-3.0 2>/dev/null; then
    gcc -O1 -Wall -o "$ROOT/precompiled/usr/bin/hello-gtk" "$ROOT/tests/wayland/hello-gtk.c" \
        $(pkg-config --cflags --libs gtk+-3.0)
    echo "OK: precompiled/usr/bin/hello-gtk"
fi
# Scripts que se ejecutan dentro del guest.
for s in "$ROOT"/tests/wayland/guest/*.sh; do
    cp -f "$s" "$ROOT/precompiled/usr/bin/$(basename "$s")"
    chmod +x "$ROOT/precompiled/usr/bin/$(basename "$s")"
done
gcc -O1 -Wall -Wno-unused-result -o "$ROOT/precompiled/usr/bin/fuzz_test" "$ROOT/tests/wayland/fuzz_test.c"
echo "OK: precompiled/usr/bin/fuzz_test"
gcc -O1 -Wall -o "$ROOT/precompiled/usr/bin/drm_test" "$ROOT/tests/wayland/drm_test.c"
echo "OK: precompiled/usr/bin/drm_test"
bash "$ROOT/scripts/build-aurora-wayland.sh"
