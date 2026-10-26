#!/bin/bash
# Regresion del servidor: foot, weston-terminal, weston-flower, simple-shm,
# portapapeles y seleccion primaria entre procesos, aw_sub_test (subsuperficies).
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
klog() { awk '{ print "RGLOG " substr($0, 1, 200) > "/dev/kmsg"; fflush("/dev/kmsg") }'; }
/usr/bin/aurora-wayland >/tmp/aw.log 2>&1 &
APID=$!
sleep 4
/usr/bin/foot -- /bin/sh -c 'echo FOOT-OK; sleep 300' >/tmp/foot.out 2>&1 &
/usr/bin/weston-terminal >/tmp/wt.out 2>&1 &
/usr/bin/weston-flower >/tmp/fl.out 2>&1 &
/usr/bin/weston-simple-shm >/tmp/ss.out 2>&1 &
/usr/bin/aw_sub_test >/tmp/sub.out 2>&1 &
sleep 25
echo "[regress.sh] SHOT-1"
sleep 8
echo "hola-clip" | /usr/bin/wl-copy &
sleep 6
echo "CLIP=$(timeout 20 /usr/bin/wl-paste -n 2>&1)" | klog
echo "hola-prim" | /usr/bin/wl-copy --primary &
sleep 6
echo "PRIM=$(timeout 20 /usr/bin/wl-paste --primary -n 2>&1)" | klog
kill -0 $APID 2>/dev/null && echo "server vivo" | klog
grep -a "aurora-wayland\]" /tmp/aw.log | head -20 | klog
kill -TERM $APID 2>/dev/null
sleep 2
echo DRIVE-DONE
