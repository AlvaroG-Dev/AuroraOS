#!/bin/bash
# Arranca Weston (DRM + pixman), lanza los clientes dados y lo cierra con
# SIGTERM. Uso: weston.sh [segundos] [cliente...]
#   STRACE=nombre  activa la traza de syscalls del kernel para ese proceso.
export XDG_RUNTIME_DIR=/run/user/0
export LIBSEAT_BACKEND=builtin
STRACE=${STRACE:-}
[ -n "$STRACE" ] && echo "strace=$STRACE" > /dev/kmsg
T=${1:-20}
shift
/usr/bin/weston --backend=drm --renderer=pixman --socket=wayland-1 --log=/tmp/weston.log >/tmp/weston.out 2>&1 &
WPID=$!
sleep 8
WAYLAND_DISPLAY=wayland-1
export WAYLAND_DISPLAY
echo "[weston.sh] socket=$WAYLAND_DISPLAY"
for c in "$@"; do
  $c >>/tmp/client.out 2>&1 &
done
sleep "$T"
echo "[weston.sh] READY-FOR-SHOT"
sleep 6
kill -TERM $WPID 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
  kill -0 $WPID 2>/dev/null || break
  sleep 1
done
wait $WPID 2>/dev/null
echo "[weston.sh] weston exit=$?"
echo "=== weston.log ==="
cat /tmp/weston.log
echo "=== weston.out ==="
cat /tmp/weston.out
echo "=== client.out ==="
cat /tmp/client.out 2>/dev/null
echo WESTON-DONE
