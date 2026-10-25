#!/bin/bash
# aurora-wayland: subsuperficies, viewporter, cursor-shape (aw_sub_test),
# seleccion primaria (wl-copy/wl-paste --primary) y pantalla completa (foot).
# El host (scripts/qemu-drive.py) captura la pantalla en cada marcador SHOT-*.
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
/usr/bin/aurora-wayland >/tmp/aw.log 2>&1 &
APID=$!
sleep 3
/usr/bin/aw_sub_test >/tmp/sub.out 2>&1 &
CPID=$!
sleep ${AW_WAIT:-20}
echo "[aw-sub.sh] SHOT-1"
sleep ${AW_HOLD:-25}
kill -TERM $CPID 2>/dev/null
sleep 2
echo "primary-ok-aurora" | /usr/bin/wl-copy --primary >/tmp/pcopy.out 2>&1 &
sleep 8
timeout 20 /usr/bin/wl-paste --primary -n >/tmp/ppaste.out 2>/tmp/ppaste.err
echo "[aw-sub.sh] primary paste rc=$? salida='$(cat /tmp/ppaste.out)' err='$(cat /tmp/ppaste.err)'"
/usr/bin/foot --fullscreen -- /bin/sh -c 'echo FULLSCREEN-FOOT; sleep 120' >/tmp/foot.out 2>&1 &
sleep 15
echo "[aw-sub.sh] SHOT-3"
sleep 10
kill -TERM $APID 2>/dev/null
sleep 2
echo "=== sub.out ==="
grep -a "aw_sub_test\|listener" /tmp/sub.out
echo "=== aw.log ==="
grep -a "aurora-wayland\]" /tmp/aw.log
echo DRIVE-DONE
