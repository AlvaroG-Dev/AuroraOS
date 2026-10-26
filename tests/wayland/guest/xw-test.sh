#!/bin/bash
# Diagnostico: arranca aurora-wayland (que lanza Xwayland) con trazas y vuelca su log.
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
export AW_XVERBOSE=1
/usr/bin/aurora-wayland >/tmp/aw.log 2>&1 &
APID=$!
sleep ${1:-60}
kill -TERM $APID 2>/dev/null
sleep 2
echo "[xw-test] ls /tmp:"; ls -la /tmp | head -30
echo "[xw-test] aw.log:"
grep -av "INFO\|DEBUG\|WARN" /tmp/aw.log | head -80
echo "[xw-test] fin"
