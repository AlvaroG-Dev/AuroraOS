#!/bin/bash
# Apps reales GTK3 y Qt5 (y un cliente GL con llvmpipe) como ventanas de Aurora.
# El entorno (GDK_BACKEND, QT_PLUGIN_PATH, EGL_PLATFORM...) lo pone init.
# Uso: toolkits.sh cliente...   (se lanzan con TK_GAP s de separacion)
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
export XDG_DATA_HOME=/tmp/.local/share
mkdir -p /tmp/.config /tmp/.cache /tmp/.local/share
# Salida hacia el serie sin lanzar un proceso por linea (que se pierden).
klog() { awk '{ print "TKLOG " substr($0, 1, 200) > "/dev/kmsg"; fflush("/dev/kmsg") }'; }
[ -n "$STRACE" ] && echo "strace=$STRACE" > /dev/kmsg
/usr/bin/aurora-wayland >/tmp/aw.log 2>&1 &
APID=$!
sleep 3
n=0
for c in "$@"; do
  n=$((n+1))
  $c >>/tmp/tk-$n.out 2>&1 &
  sleep ${TK_GAP:-45}
  echo "[toolkits.sh] SHOT-$n"
  sleep ${TK_SHOT:-8}
done
sleep ${TK_HOLD:-10}
kill -TERM $APID 2>/dev/null
sleep 2
n=0
for c in "$@"; do
  n=$((n+1))
  echo "=== $c ===" | klog
  ( head -n ${TK_HEAD:-25} /tmp/tk-$n.out; echo "-- ... --"; tail -n ${TK_TAIL:-10} /tmp/tk-$n.out ) | klog
done
echo "=== aw.log ===" | klog
head -n 40 /tmp/aw.log | klog
echo DRIVE-DONE
