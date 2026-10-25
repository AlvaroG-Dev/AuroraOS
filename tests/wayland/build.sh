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
# Scripts que se ejecutan dentro del guest.
for s in "$ROOT"/tests/wayland/guest/*.sh; do
    cp -f "$s" "$ROOT/precompiled/usr/bin/$(basename "$s")"
    chmod +x "$ROOT/precompiled/usr/bin/$(basename "$s")"
done
gcc -O1 -Wall -o "$ROOT/precompiled/usr/bin/drm_test" "$ROOT/tests/wayland/drm_test.c"
echo "OK: precompiled/usr/bin/drm_test"
