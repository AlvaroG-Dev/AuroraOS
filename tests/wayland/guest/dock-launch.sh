#!/bin/bash
# Lanzador del Dock de extremo a extremo: arranca aurora-wayland (fija los
# lanzadores de /usr/share/aurora/dock.conf), espera a que el host haga clic en
# un icono y muestra el log. El host (scripts/qemu-drive.py) sincroniza por los
# marcadores STEP-*.
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
/usr/bin/aurora-wayland >/tmp/aw.log 2>&1 &
APID=$!
sleep 4
echo "[dock-launch.sh] STEP-DOCK-READY"
sleep ${DL_WAIT1:-25}
echo "[dock-launch.sh] STEP-AFTER-CLICK"
sleep ${DL_WAIT2:-25}
echo "[dock-launch.sh] STEP-FINAL"
sleep ${DL_WAIT3:-12}
kill -TERM $APID 2>/dev/null
sleep 2
echo "=== aw.log ==="
cat /tmp/aw.log
echo DRIVE-DONE
