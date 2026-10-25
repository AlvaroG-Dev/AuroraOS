#!/bin/bash
# Arranca el servidor Wayland nativo de Aurora, lanza los clientes dados como
# ventanas normales del compositor y lo cierra con SIGTERM.
# Uso: aurora-wayland.sh [segundos] [cliente...]
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
STRACE=${STRACE:-}
[ -n "$STRACE" ] && echo "strace=$STRACE" > /dev/kmsg
T=${1:-15}
shift
/usr/bin/aurora-wayland >/tmp/aw.log 2>&1 &
APID=$!
sleep 3
for c in "$@"; do
  $c >>/tmp/client.out 2>&1 &
done
sleep "$T"
echo "[aurora-wayland.sh] READY-FOR-SHOT"
sleep "${AW_HOLD:-6}"
kill -TERM $APID 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
  kill -0 $APID 2>/dev/null || break
  sleep 1
done
wait $APID 2>/dev/null
echo "[aurora-wayland.sh] server exit=$?"
echo "=== aw.log ==="
cat /tmp/aw.log
echo "=== client.out ==="
cat /tmp/client.out 2>/dev/null
echo AW-DONE
