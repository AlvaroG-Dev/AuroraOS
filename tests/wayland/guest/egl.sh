#!/bin/bash
# Clientes EGL/GLES por software (Mesa swrast) como ventanas de Aurora.
# Uso: egl.sh [softpipe|llvmpipe] cliente...
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
export LIBGL_ALWAYS_SOFTWARE=1
export EGL_PLATFORM=wayland
export GALLIUM_DRIVER=${1:-softpipe}
shift
/usr/bin/aurora-wayland >/tmp/aw.log 2>&1 &
APID=$!
sleep 3
for c in "$@"; do
  MESA_DEBUG=1 EGL_LOG_LEVEL=debug $c >>/tmp/egl.out 2>&1 &
done
sleep ${EGL_WAIT:-40}
echo "[egl.sh] SHOT-1"
sleep ${EGL_HOLD:-15}
kill -TERM $APID 2>/dev/null
sleep 2
echo "=== egl.out ==="
grep -a -v "INFO\|DEBUG\|WARN" /tmp/egl.out | head -40
echo "=== aw.log ==="
grep -a "aurora-wayland\]" /tmp/aw.log
echo DRIVE-DONE
