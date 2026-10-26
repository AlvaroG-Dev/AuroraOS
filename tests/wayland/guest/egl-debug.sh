#!/bin/bash
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
export LIBGL_ALWAYS_SOFTWARE=1
export EGL_PLATFORM=wayland
export GALLIUM_DRIVER=softpipe
/usr/bin/aurora-wayland >/tmp/aw.log 2>&1 &
APID=$!
sleep 3
echo "strace=weston-simple-egl" > /dev/kmsg
/usr/bin/weston-simple-egl >/tmp/egl.out 2>&1
echo "[egl-debug.sh] rc=$?"
echo "strace=" > /dev/kmsg
kill -TERM $APID
sleep 1
cat /tmp/egl.out
echo DRIVE-DONE
