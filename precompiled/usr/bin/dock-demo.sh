#!/bin/bash
# Demo del Dock de Aurora: el host (scripts/qemu-drive.py) hace clic en los
# lanzadores y captura la pantalla en cada marcador DEMO-*.
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
/usr/bin/aurora-wayland >/tmp/aw.log 2>&1 &
APID=$!
sleep 4
for n in 1 2 3 4; do
  echo "[dock-demo.sh] DEMO-$n"
  sleep ${DD_WAIT:-40}
done
# Una app sin lanzador fijado: aparece tras el separador del Dock.
/usr/bin/weston-simple-shm >/dev/null 2>&1 &
sleep ${DD_WAIT:-40}
echo "[dock-demo.sh] DEMO-5"
sleep ${DD_WAIT:-40}
echo "[dock-demo.sh] DEMO-6"
sleep 20
kill -TERM $APID 2>/dev/null
sleep 2
echo "=== aw.log ==="
cat /tmp/aw.log
echo DRIVE-DONE
