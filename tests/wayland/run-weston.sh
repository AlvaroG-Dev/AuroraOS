#!/bin/bash
# Hito del Bloque 5: Weston (DRM + pixman, binarios de Ubuntu sin tocar) con
# clientes Wayland reales, captura de pantalla y cierre limpio por SIGTERM.
# Requiere scripts/fetch-weston.sh. Uso (WSL/Linux): tests/wayland/run-weston.sh
# Deja la captura en build/weston_shot.png.
set -u
cd "$(dirname "$0")/../.."
bash tests/wayland/build.sh >/dev/null || exit 1
[ -x precompiled/usr/bin/weston ] || { echo "falta weston: ejecuta scripts/fetch-weston.sh"; exit 1; }

CLIENTS="/usr/bin/weston-simple-shm /usr/bin/weston-flower /usr/bin/weston-terminal"
rm -f /tmp/weston_shot.ppm
QMON="screendump /tmp/weston_shot.ppm" QMON_WAIT="READY-FOR-SHOT" \
  bash scripts/qemu-boot-test.sh -t 150 "/usr/bin/weston.sh 12 $CLIENTS"
L=/tmp/boot-test.log

rc=0
check() { if grep -aq "$2" "$L"; then echo "PASS $1"; else echo "FAIL $1"; rc=1; fi; }
check "drm backend"      "Output 'Virtual-1' enabled"
check "desktop-shell"    "launching '/usr/libexec/weston-desktop-shell'"
check "SIGTERM limpio"   "caught signal 15"
check "weston exit 0"    "weston.sh\] weston exit=0"
for c in weston-desktop-shell weston-simple-shm weston-flower weston-terminal; do
  check "$c exit 0" "EXIT\].*name='$c' code=0"
done
if [ -f /tmp/weston_shot.ppm ] && command -v convert >/dev/null; then
  convert /tmp/weston_shot.ppm build/weston_shot.png && echo "captura: build/weston_shot.png"
fi
exit $rc
