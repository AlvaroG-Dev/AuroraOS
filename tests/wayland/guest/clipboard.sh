#!/bin/bash
# Portapapeles de extremo a extremo: wl-copy (un cliente) -> wl-paste (otro).
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
/usr/bin/aurora-wayland >/tmp/aw.log 2>&1 &
APID=$!
sleep 3
echo "hola-portapapeles-aurora" | /usr/bin/wl-copy >/tmp/copy.out 2>&1 &
sleep 8
timeout 20 /usr/bin/wl-paste -n >/tmp/paste.out 2>/tmp/paste.err
echo "[clipboard.sh] paste rc=$? salida='$(cat /tmp/paste.out)'"
echo "[clipboard.sh] paste stderr: $(cat /tmp/paste.err)"
echo "[clipboard.sh] copy: $(cat /tmp/copy.out)"
kill -TERM $APID 2>/dev/null
sleep 2
cat /tmp/aw.log
echo CLIP-DONE
