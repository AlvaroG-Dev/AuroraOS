#!/bin/bash
# Hito 5.A: apps Wayland de la distro (sin tocar) como ventanas nativas del
# compositor de Aurora, sin Weston. Captura en build/aurora_wayland_shot.png.
# Uso (WSL/Linux): tests/wayland/run-aurora-wayland.sh [cliente...]
set -u
cd "$(dirname "$0")/../.."
bash scripts/build-aurora-wayland.sh >/dev/null || exit 1
bash tests/wayland/build.sh >/dev/null || exit 1

CLIENTS="${*:-/usr/bin/weston-simple-shm /usr/bin/weston-flower /usr/bin/weston-terminal}"
rm -f /tmp/aw_shot.ppm
QMON="${AW_QMON:-screendump /tmp/aw_shot.ppm}" QMON_WAIT="READY-FOR-SHOT" \
  bash scripts/qemu-boot-test.sh -t 120 "AW_HOLD=${AW_HOLD:-6} ${AW_DEBUG:+AW_DEBUG=1} ${STRACE:+STRACE=$STRACE} /usr/bin/aurora-wayland.sh 12 $CLIENTS"
L=/tmp/boot-test.log

rc=0
check() { if grep -aq -e "$2" "$L"; then echo "PASS $1"; else echo "FAIL $1"; rc=1; fi; }
check "servidor listo"  "READY socket=wayland-0"
check "toplevel mapeado" "-> ventana"
check "server exit 0"   "aurora-wayland.sh\] server exit=0"
if [ -f /tmp/aw_shot.ppm ] && command -v convert >/dev/null; then
  convert /tmp/aw_shot.ppm build/aurora_wayland_shot.png && echo "captura: build/aurora_wayland_shot.png"
fi
exit $rc
