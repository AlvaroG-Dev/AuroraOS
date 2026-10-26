#!/bin/bash
# Fuzzing de DRM/AF_UNIX (fuzz_test) con aurora-wayland vivo: el servidor y el
# kernel deben seguir respondiendo despues.
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
klog() { awk '{ print "FZLOG " substr($0, 1, 200) > "/dev/kmsg"; fflush("/dev/kmsg") }'; }
/usr/bin/aurora-wayland >/tmp/aw.log 2>&1 &
APID=$!
sleep 3
/usr/bin/fuzz_test ${FUZZ_SCALE:-1} 2>&1 | klog
echo "FZLOG fuzz rc=${PIPESTATUS[0]}" > /dev/kmsg
# El servidor sigue vivo y atiende clientes nuevos.
/usr/bin/wl_test >/tmp/wl.out 2>&1
echo "FZLOG wl_test rc=$?" > /dev/kmsg
kill -0 $APID 2>/dev/null && echo "FZLOG aurora-wayland vivo" > /dev/kmsg
kill -TERM $APID
sleep 2
echo DRIVE-DONE
